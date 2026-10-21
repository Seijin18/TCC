#ifndef ANALISADOR_NODE_H
#define ANALISADOR_NODE_H

/*
 * TCC Analisador de Espectro — NRF24L01 + ESP32
 *
 * Varre a banda 2.4 GHz e envia atividade por canal (0–100%) via serial CSV.
 * Baseado em espReceiver.ino (projeto IC).
 *
 * Wiring (NRF24L01 -> ESP32, padrao TCC mestre/jammer):
 *   VCC  -> 3.3 V      GND  -> GND
 *   CE   -> GPIO 22    CSN  -> GPIO 21
 *   SCK  -> GPIO 18    MOSI -> GPIO 23    MISO -> GPIO 19
 *
 * Alerta ao mestre (spectrum-attack-classification): GPIO 4 (saida) ligado
 * ao GPIO 4 do mestre (entrada) + GND comum entre as duas placas, quando
 * standalone (4 placas). Numa Placa A combinada (two-board-topology), o
 * alerta usa uma flag compartilhada em vez de GPIO — ver
 * analisador_set_alert_sink() / analisador_set_chq_sink(). Ver docs/pinout.md.
 *
 * channel-quality-sensing: quem decide o alerta/máscara é a ocupação por
 * canal com histerese (shared/channel_quality.h), agnóstica à causa. O
 * classificador CFAR/votos abaixo virou só telemetria (`chmap`, cfar_on/off).
 *
 * Output (115200 baud, sem cabecalho):
 *   <ch0_%>,<ch1_%>,...,<ch125_%>
 */

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <math.h>
#include "../../shared/hopping.h"
#include "../../shared/channel_quality.h"
#include "../../shared/telemetry.h"

// Pinagem standalone (4 placas, barramento único): CE/CSN/SCK/MISO/MOSI
// abaixo. Numa Placa A combinada (two-board-topology seção 7), o VSPI
// pertence ao mestre -- o analisador usa o barramento HSPI (PIN_*_HSPI),
// selecionado em analisador_setup() quando analisador_set_alert_sink() já
// foi registrado antes da chamada.
#define PIN_CE 22
#define PIN_CSN 21
#define PIN_SCK 18
#define PIN_MISO 19
#define PIN_MOSI 23

#define PIN_CE_HSPI 16
#define PIN_CSN_HSPI 15
#define PIN_SCK_HSPI 4
#define PIN_MISO_HSPI 2
#define PIN_MOSI_HSPI 17

// Pino de alerta para o mestre (spectrum-attack-classification) — nivel
// HIGH enquanto "ataque ativo", LOW caso contrario. Usado como default
// quando nenhum sink alternativo é registrado via
// analisador_set_alert_sink() (ver two-board-topology). Ver docs/pinout.md.
// GPIO 4 também é o SCK do barramento HSPI usado pelo analisador quando
// combinado -- por isso o pinMode() só roda no cenário standalone.
#define PIN_ALERT 4

// 1 = varre apenas ch 2–80 (banda BT, refresh ~3x mais rapido)
// 0 = varredura completa 126 canais (2400–2525 MHz) -- necessário para
// enxergar os canais de escape 94–125 usados pelo dynamic-seed-blacklist.
#define FAST_BT_SCAN 0

#define BT_CH_MIN 2
#define BT_CH_MAX 80

static const uint8_t ATTACK_STREAK_N = 2;   // ciclos candidatos consecutivos (debounce final, leve -- ver baseline adaptativo abaixo)
static const uint8_t ATTACK_MIN_CHANNELS = 1; // canais de HOPPING_CHANNELS comprometidos simultaneamente p/ candidato

static const int NUM_CHANNELS = 126;
static const int SCAN_SAMPLES = 10;
static const int PLL_DWELL_US = 130;

// Achado de bancada (2026-09-18): LIMIAR_ALVO/LIMIAR_CONCENTRACAO (media
// de channelData[]/channelHeat[] sobre os 16 canais de HOPPING_CHANNELS
// vs. o resto do espectro) nunca disparava contra um jammer real --
// diagnosticado em duas causas. (1) um jammer sequencial de radio unico
// so ocupa 1-4 dos 16 canais-alvo por vez, mesmo em modo targeted
// (confirmado canal-a-canal, nao so na media); (2) o piso de ruido desta
// bancada recai sobre metade dos 16 canais de HOPPING_CHANNELS mesmo com
// o jammer desligado. Combinado, ruido ambiente e jamming real produzem
// a MESMA ordem de grandeza quando resumidos numa media/contagem sobre
// os 16 -- nenhum LIMIAR_CONCENTRACAO separa as duas classes (varredura
// de grade completa, 2026-09-23, documentada em
// openspec/changes/spectrum-attack-classification/design.md "Achado de
// calibração 2026-09-23").
//
// Fix (2026-09-23): baseline adaptativo POR CANAL em vez de comparar
// contra a média dos outros 15. Cada canal aprende seu próprio "normal"
// (EWMA lenta, `g_baseline`) e sua própria variabilidade típica (desvio
// absoluto médio, `g_mad`) — um canal com WiFi/BT presente o tempo todo
// simplesmente tem baseline mais alta, sem contar como suspeito. Só
// conta como candidato a comprometido quando o valor ATUAL desvia da
// PRÓPRIA baseline por vários `g_mad` (ADAPTIVE_K) E por uma margem
// absoluta mínima (ADAPTIVE_MIN_ABS_DEV, evita falso alarme em canal
// naturalmente muito estável onde g_mad é minúsculo). Equivalente a um
// detector CFAR (comum em radar, para o mesmo problema de limiar fixo
// contra piso de ruído variável), aplicado no tempo em vez de entre
// canais vizinhos.
//
// Persistência por JANELA DESLIZANTE (votos M-de-W), não por sweeps
// consecutivos: medido que o sinal real do jammer por canal TAMBÉM é
// intermitente (ele visita os outros 15 canais entre uma passagem e
// outra por este) -- exigir sweeps consecutivos descartava o sinal real
// junto com rajadas isoladas de ruído (ex.: um dispositivo ligando por
// ~3s). Um buffer circular de `ADAPTIVE_WINDOW` booleans por canal
// (`g_vote_history`) + contagem corrente (`g_vote_count`, mantida em
// O(1) por ciclo) implementam literalmente "M candidatos dentre os
// últimos W sweeps" -- separa rajada isolada (poucos votos, some antes
// de acumular) de ataque real (votos recorrentes ao longo de toda a
// janela de ataque). Custo de memória (126 canais * ADAPTIVE_WINDOW
// bytes) é irrelevante nos 320KB de RAM do ESP32 -- ver nota abaixo
// sobre por que isso importa.
//
// A baseline só se adapta enquanto o canal NÃO está marcado como
// comprometido -- senão um ataque sustentado seria lentamente "aprendido"
// como o novo normal.
//
// Achado de processo (2026-09-23, parte 1): a primeira versão testada
// aqui trocou este buffer circular por uma aproximação EWMA "pra
// economizar memória", sem revalidar a aproximação -- rodado em bancada
// real, a máscara nunca saiu de zero a sessão inteira. Só foi pego
// porque tools/validation/adaptive_baseline_sim.py foi atualizado pra
// simular a MESMA aproximação EWMA e reproduziu exatamente o mesmo
// resultado (zero detecções) offline, confirmando que a EWMA não era
// equivalente à janela literal já validada. Lição: nunca trocar um
// mecanismo já validado por uma aproximação não testada quando o
// recurso que ela economiza (aqui, ~1,9KB de RAM) não é escasso.
//
// Achado de processo (2026-09-23, parte 2): com o buffer literal
// corrigido, ADAPTIVE_WINDOW=15/ADAPTIVE_VOTES=4 (validado contra as
// primeiras 5 sessões) ainda ficou SILENCIOSO (mask sempre 0x0000) em
// duas sessões novas rodadas na sequência, mesmo com jamming real
// confirmado (PLR subindo). Causa: o jammer só visita cada canal-alvo
// raramente (ver "Achado de calibração 2026-09-23" em
// spectrum-attack-classification/design.md) -- em quantas dessas
// visitas o analisador realmente amostra aquele canal específico
// depende da fase relativa entre dois ciclos periódicos não
// sincronizados (varredura do analisador vs. rodízio do jammer), que
// varia por sessão. Com janela de 15 sweeps, o número esperado de
// coincidências pode cair abaixo de 1 dependendo dessa fase -- exigir 4
// delas simplesmente nunca acontecia em sessões com fase desfavorável.
// Recalibrado contra as 7 sessões (as 5 originais + as 2 que
// silenciaram): janela bem maior (60 sweeps) com exigência de voto
// proporcionalmente modesta (5) dá tempo suficiente pra coincidências
// raras se acumularem independente da fase -- zero falsos positivos e
// detecção real em TODAS as 7 sessões (antes: só 4 de 5, e olhe lá).
// Custo: mais latência até o primeiro disparo possível (até ~60 sweeps
// de histórico, dezenas de segundos a poucos minutos dependendo do
// ciclo real de scanSpectrum()) -- aceitável, já que o alternativa
// (janela curta) simplesmente não funcionava de forma confiável.
//
// Parâmetros validados offline contra CSV bruto real de 7 sessões de
// bancada (tools/validation/adaptive_baseline_sim.py, buffer circular
// literal) -- zero falsos positivos em baseline nas 7, detecção real
// confirmada nas 7 (55-93% das leituras da fase de jamming com pelo
// menos 1 canal sinalizado, a depender da sessão); valores no centro da
// região robusta da varredura de grade (não um pico isolado).
static const float ADAPTIVE_ALPHA = 0.1f;         // velocidade de adaptação da baseline/mad
static const float ADAPTIVE_K = 3.0f;             // desvios (em unidades de mad) acima da baseline p/ candidato
static const float ADAPTIVE_MIN_ABS_DEV = 10.0f;  // margem absoluta minima (pontos percentuais) p/ candidato
static const float ADAPTIVE_MIN_DEV_FLOOR = 3.0f; // piso de g_mad -- evita explosão do score em canal parado
static const uint8_t ADAPTIVE_WINDOW = 60;        // tamanho da janela deslizante (em sweeps)
static const uint8_t ADAPTIVE_VOTES = 5;          // votos exigidos dentro da janela p/ declarar comprometido

static SPIClass analisador_hspi(HSPI);
static RF24 analisador_radio(PIN_CE, PIN_CSN);
static int channelData[NUM_CHANNELS];

// Estado do baseline adaptativo por canal -- ver comentário acima
// (ADAPTIVE_*). float é seguro aqui: nada disto é tocado por interrupção
// nem compartilhado com outra task diretamente (só o boolean agregado de
// classifyAttack() cruza pra fora, e agora só via tlm_printf).
static float g_baseline[NUM_CHANNELS];
static float g_mad[NUM_CHANNELS];
static bool g_primed[NUM_CHANNELS];
static bool g_channel_compromised[NUM_CHANNELS];
// Janela deslizante de votos (candidato sim/não) por canal, buffer
// circular + contagem corrente mantida em O(1) por ciclo -- ver nota
// "Achado de processo" acima sobre por que isto é um buffer literal e
// não uma aproximação EWMA.
static bool g_vote_history[NUM_CHANNELS][ADAPTIVE_WINDOW];
static uint8_t g_vote_write_idx[NUM_CHANNELS];
static uint8_t g_vote_count[NUM_CHANNELS];

static uint8_t g_attack_streak = 0;
static bool g_attack_active = false; // só telemetria (CFAR) -- não aciona o alerta

// channel-quality-sensing (tarefa 3.1): qualidade por canal medida a cada
// varredura completa -- ocupação + histerese (shared/channel_quality.h).
// É ESTE estado que decide o alerta e a máscara entregue ao mestre.
static ChannelQualityState g_analisador_chq;
static bool g_chq_alert_active = false; // (máscara != 0), borda -> alert_on/alert_off

// Sink alternativo do alerta de ataque (two-board-topology): quando
// registrado (Placa A combinada, mestre+analisador na mesma placa), grava
// numa flag compartilhada em vez do GPIO físico. Default (nullptr) preserva
// o comportamento standalone via digitalWrite(PIN_ALERT, ...).
static void (*g_analisador_alert_sink)(bool) = nullptr;

// channel-quality-sensing (tarefa 3.1): sink da MÁSCARA de 16 bits (bit i =
// HOPPING_CHANNELS[i] degradado) na placa combinada. Quando registrado, a
// placa é combinada (HSPI, sem GPIO de alerta) e o mestre consome a máscara
// inteira em vez do booleano. Sem ele (4 placas) o booleano
// (máscara != 0) continua indo pelo pino PIN_ALERT, como antes.
static void (*g_analisador_chq_sink)(uint16_t) = nullptr;

static void analisador_set_alert_sink(void (*sink)(bool))
{
    g_analisador_alert_sink = sink;
}

static void analisador_set_chq_sink(void (*sink)(uint16_t))
{
    g_analisador_chq_sink = sink;
}

// Placa combinada = qualquer sink registrado (bool ou máscara): o VSPI é do
// mestre e GPIO 4 é o SCK do HSPI do analisador, então nunca mexe em PIN_ALERT.
static bool analisador_is_combined()
{
    return g_analisador_alert_sink != nullptr || g_analisador_chq_sink != nullptr;
}

static void setAlertPin(bool active)
{
    if (g_analisador_alert_sink)
        g_analisador_alert_sink(active);
    else if (!g_analisador_chq_sink)
        digitalWrite(PIN_ALERT, active ? HIGH : LOW);
}

static void configureReceiver(RF24 &r)
{
    r.setAutoAck(false);
    r.stopListening();
    r.setPALevel(RF24_PA_MIN);
    r.setDataRate(RF24_1MBPS);
}

static int measureChannel(uint8_t ch)
{
    analisador_radio.setChannel(ch);
    delayMicroseconds(PLL_DWELL_US);

    int hits = 0;
    for (int s = 0; s < SCAN_SAMPLES; s++)
    {
        analisador_radio.startListening();
        delayMicroseconds(PLL_DWELL_US);
        if (analisador_radio.testCarrier())
            hits++;
        analisador_radio.stopListening();
    }

    return (hits * 100) / SCAN_SAMPLES;
}

// Atualiza o baseline adaptativo de UM canal com a leitura atual --
// ver o bloco de comentário em ADAPTIVE_* para a lógica completa.
static inline void updateAdaptiveBaseline(int ch, int reading)
{
    float v = (float)reading;

    if (!g_primed[ch])
    {
        g_baseline[ch] = v;
        g_mad[ch] = ADAPTIVE_MIN_DEV_FLOOR;
        g_primed[ch] = true;
        return;
    }

    float dev = v - g_baseline[ch];
    float score = dev / g_mad[ch];
    bool candidate = (score > ADAPTIVE_K) && (dev > ADAPTIVE_MIN_ABS_DEV);

    // Janela deslizante literal (buffer circular + contagem em O(1)):
    // substitui o voto mais antigo pelo atual e ajusta g_vote_count sem
    // precisar re-somar o buffer inteiro a cada ciclo.
    uint8_t idx = g_vote_write_idx[ch];
    bool evicted = g_vote_history[ch][idx];
    g_vote_history[ch][idx] = candidate;
    g_vote_write_idx[ch] = (uint8_t)((idx + 1) % ADAPTIVE_WINDOW);
    if (evicted && !candidate)
        g_vote_count[ch]--;
    else if (!evicted && candidate)
        g_vote_count[ch]++;
    g_channel_compromised[ch] = g_vote_count[ch] >= ADAPTIVE_VOTES;

    // Só deixa a baseline se adaptar quando o canal NÃO está marcado
    // como comprometido -- senão um ataque sustentado seria lentamente
    // "aprendido" como o novo normal.
    if (!g_channel_compromised[ch])
    {
        g_baseline[ch] += ADAPTIVE_ALPHA * (v - g_baseline[ch]);
        float abs_dev = fabsf(v - g_baseline[ch]);
        g_mad[ch] += ADAPTIVE_ALPHA * (abs_dev - g_mad[ch]);
        if (g_mad[ch] < ADAPTIVE_MIN_DEV_FLOOR)
            g_mad[ch] = ADAPTIVE_MIN_DEV_FLOOR;
    }
}

static void scanSpectrum()
{
#if FAST_BT_SCAN
    for (int i = 0; i < NUM_CHANNELS; i++)
        channelData[i] = 0;

    for (int ch = BT_CH_MIN; ch <= BT_CH_MAX; ch++)
    {
        channelData[ch] = measureChannel((uint8_t)ch);
        updateAdaptiveBaseline(ch, channelData[ch]);
    }
#else
    for (int ch = 0; ch < NUM_CHANNELS; ch++)
    {
        channelData[ch] = measureChannel((uint8_t)ch);
        updateAdaptiveBaseline(ch, channelData[ch]);
    }
#endif
}

// Compara a atividade media nos 16 canais de HOPPING_CHANNELS (alvo_avg)
// contra o restante do espectro efetivamente varrido nesse ciclo (fora_avg,
// respeitando FAST_BT_SCAN), exige ATTACK_STREAK_N ciclos candidatos
// consecutivos antes de declarar "ataque ativo", e reflete o estado via
// setAlertPin() (HIGH/flag enquanto ativo, LOW/flag assim que um ciclo
// deixa de ser candidato).
// Conta quantos dos 16 canais de HOPPING_CHANNELS o baseline adaptativo
// (updateAdaptiveBaseline(), chamado por scanSpectrum() a cada canal)
// marcou como comprometido neste ciclo, e monta a máscara correspondente
// (bit i = 1 -> HOPPING_CHANNELS[i] comprometido) -- mesma convenção de
// bit usada em g_channel_mask (dynamic-seed-blacklist), só que aqui é
// evidência de RF local do analisador, não de falha de ACK do mestre.
static uint16_t computeCompromisedMask(uint8_t *out_count)
{
    uint16_t mask = 0;
    uint8_t count = 0;
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
    {
        if (g_channel_compromised[HOPPING_CHANNELS[i]])
        {
            mask |= (uint16_t)(1u << i);
            count++;
        }
    }
    *out_count = count;
    return mask;
}

// Declara "ataque ativo" quando pelo menos ATTACK_MIN_CHANNELS canais de
// HOPPING_CHANNELS estão simultaneamente marcados como comprometidos
// pelo baseline adaptativo (updateAdaptiveBaseline(), por canal -- ver
// bloco ADAPTIVE_* acima), por ATTACK_STREAK_N ciclos seguidos.
// channel-quality-sensing: agora é SÓ TELEMETRIA -- registra o veredito
// como cfar_on/cfar_off, sem acionar setAlertPin(). Emite o mapa completo de
// canais comprometidos via telemetria (`chmap`) a cada ciclo, para
// observabilidade/depuração independente do alerta agregado.
static void classifyAttack()
{
    uint8_t compromised_count = 0;
    uint16_t mask = computeCompromisedMask(&compromised_count);

    tlm_printf("analisador", "chmap", "mask=0x%04X;count=%u", mask, compromised_count);

    // channel-quality-sensing (tarefa 3.2): o veredito do CFAR NÃO aciona
    // mais o alerta (quem faz isso é classifyChannelQuality()). Fica como
    // telemetria comparável (cfar_on/cfar_off, antigos alert_on/alert_off)
    // até a validação de bancada da change.
    bool candidate = compromised_count >= ATTACK_MIN_CHANNELS;

    if (candidate)
    {
        g_attack_streak++;
        if (g_attack_streak >= ATTACK_STREAK_N && !g_attack_active)
        {
            g_attack_active = true;
            tlm_printf("analisador", "cfar_on", "count=%u;mask=0x%04X", compromised_count, mask);
        }
    }
    else
    {
        g_attack_streak = 0;
        if (g_attack_active)
        {
            g_attack_active = false;
            tlm_event("analisador", "cfar_off");
        }
    }
}

// channel-quality-sensing (tarefa 3.1): alimenta a janela de ocupação com a
// varredura recém-concluída, avalia a histerese por canal e entrega o
// resultado: telemetria `chq`, máscara ao sink da placa combinada e o
// booleano (máscara != 0) ao alerta legado (GPIO de 4 placas / sink bool).
static void classifyChannelQuality()
{
    // occ[i] = channelData[HOPPING_CHANNELS[i]] = % (0..100) das
    // SCAN_SAMPLES leituras de testCarrier() que acenderam naquele canal
    // nesta varredura -- exatamente o valor da célula do CSV/spectrum.
    uint8_t occ[TABLE_SIZE];
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
    {
        int v = channelData[HOPPING_CHANNELS[i]];
        occ[i] = (uint8_t)(v < 0 ? 0 : (v > 100 ? 100 : v));
    }

    chq_push_sweep(g_analisador_chq, occ);
    uint16_t mask = chq_degraded_mask(g_analisador_chq);

    // Até 3 canais de maior ocupação média da janela (canal RF:ocupação),
    // só os com média > 0. Seleção simples sobre 16 itens.
    char top[48];
    int pos = 0;
    top[0] = '\0';
    bool used[TABLE_SIZE] = {false};
    for (uint8_t n = 0; n < 3; n++)
    {
        int best = -1;
        uint8_t best_mean = 0;
        for (uint8_t i = 0; i < TABLE_SIZE; i++)
        {
            uint8_t m = chq_mean(g_analisador_chq, i);
            if (!used[i] && m > best_mean)
            {
                best = i;
                best_mean = m;
            }
        }
        if (best < 0)
            break;
        used[best] = true;
        pos += snprintf(top + pos, sizeof(top) - (size_t)pos, "%s%u:%u", n ? "," : "", HOPPING_CHANNELS[best], best_mean);
    }
    tlm_printf("analisador", "chq", "mask=0x%04X;top=%s", mask, top);

    if (g_analisador_chq_sink)
        g_analisador_chq_sink(mask);

    bool alert = (mask != 0);
    if (alert != g_chq_alert_active)
    {
        g_chq_alert_active = alert;
        setAlertPin(alert);
        if (alert)
        {
            guarded_printf("[ALERTA] Canais degradados=%u mask=0x%04X\n", mask_popcount16(mask), mask);
            tlm_printf("analisador", "alert_on", "count=%u;mask=0x%04X", mask_popcount16(mask), mask);
        }
        else
        {
            guarded_println("[ALERTA] Canais voltaram ao normal.");
            tlm_event("analisador", "alert_off");
        }
    }
}

static void sendCSV()
{
    // Monta a linha inteira num buffer local e imprime de uma vez só: um
    // guarded_print() por número (como antes) libera o lock entre cada
    // chamada, dando à outra task (mestre, na Placa A combinada) chance de
    // intercalar sua própria linha no meio do CSV -- reproduzido em
    // bancada como linhas de espectro corrompidas.
    char line[NUM_CHANNELS * 4 + 1];
    int pos = 0;
    pos += snprintf(line + pos, sizeof(line) - (size_t)pos, "%d", channelData[0]);
    for (int i = 1; i < NUM_CHANNELS && pos < (int)sizeof(line) - 8; i++)
        pos += snprintf(line + pos, sizeof(line) - (size_t)pos, ",%d", channelData[i]);
    guarded_println(line);
}

static void analisador_setup()
{
    Serial.begin(115200);
    delay(2000);

    guarded_println("\n=== TCC Analisador v1 — Espectro 2.4 GHz ===");

    bool combined_mode = analisador_is_combined();
    chq_reset(g_analisador_chq);

    if (!combined_mode)
    {
        pinMode(PIN_ALERT, OUTPUT);
        digitalWrite(PIN_ALERT, LOW);
    }

    if (combined_mode)
        analisador_hspi.begin(PIN_SCK_HSPI, PIN_MISO_HSPI, PIN_MOSI_HSPI, PIN_CSN_HSPI);
    else
        SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);

    bool radioOK = false;
    for (int attempt = 1; attempt <= 5; attempt++)
    {
        bool began = combined_mode
                         ? analisador_radio.begin(&analisador_hspi, PIN_CE_HSPI, PIN_CSN_HSPI)
                         : analisador_radio.begin();
        if (began)
        {
            radioOK = true;
            break;
        }
        guarded_printf("radio.begin() falhou, tentativa %d/5...\n", attempt);
        delay(500);
    }

    if (!radioOK || !analisador_radio.isChipConnected())
    {
        while (true)
        {
            guarded_println("[ERRO] NRF24L01 nao detectado. Verifique fiação e 3.3V.");
            delay(3000);
        }
    }

    configureReceiver(analisador_radio);

#if FAST_BT_SCAN
    guarded_printf("Modo: FAST_BT_SCAN (ch %d–%d, 2402–2480 MHz)\n", BT_CH_MIN, BT_CH_MAX);
#else
    guarded_println("Modo: FULL (ch 0–125, 2400–2525 MHz)");
#endif
    guarded_println("Enviando CSV: ch0,ch1,...,ch125 (atividade 0–100%)\n");
    tlm_event("analisador", "boot");
}

static void analisador_loop()
{
    scanSpectrum();
    classifyAttack();        // telemetria (chmap, cfar_on/off) -- não decide o alerta
    classifyChannelQuality(); // ocupação + histerese: decide alerta e máscara
    sendCSV();
}

#endif // ANALISADOR_NODE_H
