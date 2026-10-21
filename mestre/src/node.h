#ifndef MESTRE_NODE_H
#define MESTRE_NODE_H

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <printf.h> // Necessário para o printDetails()
#include <string.h>
#include <ctype.h>
#include "../../shared/hopping.h"
#include "../../shared/control_policy.h"
#include "../../shared/channel_quality.h"
#include "../../shared/protocol.h"
#include "../../shared/time_sync.h"
#include "../../shared/pairing.h"
#include "../../shared/telemetry.h"
#include "../../shared/radio_diag.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// -------------------------------------------------------------
// FASE 1: O Mestre (Transmissor do Beacon)
// -------------------------------------------------------------

// CE:   GPIO 21
// CSN:  GPIO 22
// SCK:  GPIO 18
// MOSI: GPIO 23
// MISO: GPIO 19
// VCC:  3.3V
// GND:  GND

// Pinos do SPI para o ESP32 (Forçando VSPI)
#define PIN_CE 22
#define PIN_CSN 21

#define PIN_SCK 18
#define PIN_MISO 19
#define PIN_MOSI 23

// Pino de alerta vindo do analisador (spectrum-attack-classification):
// HIGH enquanto "ataque ativo", LOW caso contrario. Usado como default
// quando nenhuma fonte alternativa é registrada via
// mestre_set_alert_source() (ver two-board-topology). Ver docs/pinout.md.
#define PIN_ALERT_IN 4

// pairing-handshake-hardening: RF24Diag (shared/radio_diag.h) em vez de
// RF24 puro -- so acrescenta um leitor publico de registrador sobre o
// mesmo objeto, nenhuma mudanca de comportamento (ver comentario lá).
static RF24Diag mestre_radio(PIN_CE, PIN_CSN);

// Achado de bancada (2026-09-24): igual ao motivo de HOP_TIMEOUT_MS/o
// delayMicroseconds(500) em mestre_loop() (ver comentário lá) -- 130us
// (o valor nominal do datasheet) não é suficiente pro PLL assentar após um
// salto grande de frequência nestes módulos específicos. Aqui isso
// enviesava measure_and_pick_candidate(): candidato 0 (primeiro avaliado,
// e o canal em que o rádio já estava desde o setup) sempre "ganhava" por
// os demais lerem atividade espúria (settling incompleto, não ruído
// real) -- confirmado em bancada, ~90% das escolhas caindo no candidato 0
// em duas sessões distintas, mesmo sob condições limpas.
static const uint32_t PAIR_MEASURE_DWELL_US = 500;

// Fonte alternativa do alerta de ataque (two-board-topology): quando
// registrada (Placa A combinada, mestre+analisador na mesma placa), lê uma
// flag compartilhada em vez do GPIO físico. Default (nullptr) preserva o
// comportamento standalone via digitalRead(PIN_ALERT_IN).
//
// NOTA para o main.cpp de quem registrar isto: precisa ser chamado ANTES
// de mestre_setup(), não depois -- tanto o guard de pinMode(PIN_ALERT_IN)
// quanto o do blink de LED_BUILTIN (GPIO 2 == MISO do HSPI usado pelo nó
// secundário na placa combinada) decidem uma única vez em mestre_setup().
static bool (*g_mestre_alert_source)() = nullptr;

static void mestre_set_alert_source(bool (*source)())
{
    g_mestre_alert_source = source;
}

// channel-quality-sensing: fonte da máscara de canais degradados do
// analisador (bit i = HOPPING_CHANNELS[i]) na placa combinada. Registrada: o
// mestre funde a máscara com a evidência de ack e usa a política de reseed por
// qualidade. Sem ela: reseed na borda do alerta, como antes.
// Registrar ANTES de mestre_setup().
static uint16_t (*g_mestre_chq_source)() = nullptr;

static void mestre_set_chq_source(uint16_t (*source)())
{
    g_mestre_chq_source = source;
}

// Placa combinada = qualquer fonte registrada: sem pino de alerta nem blink de
// LED_BUILTIN (GPIO 2 é MISO do HSPI).
static bool mestre_is_combined()
{
    return g_mestre_alert_source != nullptr || g_mestre_chq_source != nullptr;
}

/**
 * Lê a seed do console serial (0-100). Janela curta e não-bloqueante: se
 * nada for digitado dentro de SEED_WAIT_WINDOW_MS, assume SEED_DEFAULT --
 * adaptive-rendezvous, para setup() nunca travar em bancada não assistida.
 */
static uint16_t g_seed = 0;
static bool g_seed_initialized = false;
static unsigned long g_seed_send_time = 0;

// pairing-handshake-hardening: declarada aqui (em vez de junto do resto do
// "Estado de pareamento" mais abaixo) porque handle_mestre_command()
// ("seed <n>") já precisa dela antes desse ponto do arquivo.
static bool g_link_established = false;

// Forward decl -- wait_for_seed_window() (abaixo) repassa linhas
// nao-numericas pro mesmo tratador de comandos usado depois do boot (ver
// comentario no "else" abaixo); handle_mestre_command() só é definida mais
// adiante no arquivo.
static void handle_mestre_command(char *line);
static bool try_handle_hopset(char *line);

static bool is_all_digits(const char *s)
{
    if (!s || *s == '\0')
        return false;
    for (const char *p = s; *p; p++)
        if (!isdigit((unsigned char)*p))
            return false;
    return true;
}

static void wait_for_seed_window()
{
    guarded_println("[MESTRE] === CONFIGURAÇÃO DE SEED ===");
    guarded_printf("[MESTRE] Digite a seed desejada (0-100) e pressione ENTER (janela de %lu ms, default=%u):\n",
                  SEED_WAIT_WINDOW_MS, SEED_DEFAULT);

    unsigned long start = millis();
    while (millis() - start < SEED_WAIT_WINDOW_MS)
    {
        if (Serial.available())
        {
            String input = Serial.readStringUntil('\n');
            input.trim();

            if (input.length() == 0)
            {
                // linha vazia -- nada a fazer.
            }
            else if (is_all_digits(input.c_str()))
            {
                int value = input.toInt();
                if (value >= 0 && value <= 100)
                {
                    g_seed = (uint16_t)value;
                    g_seed_initialized = true;
                    guarded_printf("[MESTRE] Seed configurada: %u\n", g_seed);
                    return;
                }
                guarded_printf("[MESTRE] Valor inválido: %d. Digite um valor entre 0 e 100.\n", value);
            }
            else
            {
                // pairing-handshake-hardening: uma linha nao-numerica aqui
                // (ex. "regs") NAO pode virar seed 0 -- String::toInt() da
                // libc do Arduino retorna 0 pra entrada invalida, e 0 é um
                // valor de seed valido, entao o bug era silencioso (a seed
                // virava 0 sem nenhum aviso). Repassa pro tratador de
                // comandos normal (handle_mestre_command(), mesma logica de
                // "regs"/"mode fixed"/etc. usada depois do boot) em vez de
                // descartar ou interpretar como seed.
                char buf[32];
                input.toCharArray(buf, sizeof(buf));
                handle_mestre_command(buf);
            }
        }
        delay(10);
    }

    g_seed = SEED_DEFAULT;
    g_seed_initialized = true;
    guarded_printf("[MESTRE] Nenhuma seed digitada a tempo. Usando default: %u\n", g_seed);
}

// scenario-automation: Cenário A (sem hopping) -- "mode fixed" trava o
// canal de dados em TEST_FIXED_CHANNEL e desativa reseed/blacklist (o
// objetivo é medir o baseline SEM nenhuma adaptação). "mode hopping" volta
// ao comportamento normal (default).
static bool g_fixed_channel_mode = false;

// scenario-automation: Cenário B (hopping fixo, sem self-healing) vs.
// Cenário C (sistema completo, default) -- "mitigation off" desativa só a
// REAÇÃO (reseed/blacklist) sem desativar o hopping em si, ortogonal a
// g_fixed_channel_mode. Combinações válidas: fixed (A) · hopping+mitigation
// off (B) · hopping+mitigation on (C, default).
static bool g_mitigation_enabled = true;

// scenario-automation: comando "reset" força reset_to_pairing() (definida
// mais abaixo) por serial -- fronteira limpa entre cenários da bateria
// A/B/C sem precisar reiniciar a placa (zera g_channel_mask/g_channel_fail/
// g_channel_blacklisted_since, força re-pareamento).
static void reset_to_pairing(bool clear_blacklist = false);

// Comando runtime "seed <n>" -- só tem efeito antes do enlace fechar
// (pairing-handshake-hardening, D3: a seed vai dentro da propria sonda, e
// uma vez aceita pelo ack ela já está em uso no hopping -- trocar depois
// disso dessincronizaria o calculo de canal entre os dois lados). Vale
// pra próxima tentativa de pareamento; reiniciar a placa para trocar de
// seed no meio de uma sessão já ativa.
static char g_cmd_buf[32];
static uint8_t g_cmd_len = 0;

static void handle_mestre_command(char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;

    if (strncmp(line, "seed ", 5) == 0)
    {
        if (g_link_established)
        {
            guarded_println("[MESTRE] Enlace já estabelecido nesta sessão; reinicie para trocar a seed.");
            return;
        }
        int value = atoi(line + 5);
        if (value >= 0 && value <= 100)
        {
            g_seed = (uint16_t)value;
            guarded_printf("[MESTRE] Seed atualizada via comando: %u\n", g_seed);
        }
        else
        {
            guarded_printf("[MESTRE] Valor de seed inválido: %d\n", value);
        }
    }
    else if (strcmp(line, "mode fixed") == 0)
    {
        g_fixed_channel_mode = true;
        tlm_event("mestre", "mode_fixed");
        guarded_printf("[MESTRE] Modo fixo: canal travado em %u (sem hopping, sem reseed/blacklist)\n", TEST_FIXED_CHANNEL);
    }
    else if (strcmp(line, "mode hopping") == 0)
    {
        g_fixed_channel_mode = false;
        tlm_event("mestre", "mode_hopping");
        guarded_println("[MESTRE] Modo hopping normal restaurado.");
    }
    else if (strcmp(line, "mitigation off") == 0)
    {
        g_mitigation_enabled = false;
        tlm_event("mestre", "mitigation_off");
        guarded_println("[MESTRE] Mitigação (reseed/blacklist) desativada -- hopping continua normal (Cenário B).");
    }
    else if (strcmp(line, "mitigation on") == 0)
    {
        g_mitigation_enabled = true;
        tlm_event("mestre", "mitigation_on");
        guarded_println("[MESTRE] Mitigação (reseed/blacklist) reativada (Cenário C, default).");
    }
    else if (strcmp(line, "reset") == 0)
    {
        guarded_println("[MESTRE] Reset solicitado via comando -- voltando ao pareamento.");
        // jamming-mitigation-hardening: reset LIMPO por comando do operador
        // (boot de bancada, fronteira entre cenários da bateria) ZERA a
        // blacklist -- ao contrário do reset causado por jamming, que a
        // preserva. Sem isto, um cenário da bateria herdaria a blacklist do
        // anterior.
        reset_to_pairing(true);
    }
    else if (strcmp(line, "regs") == 0)
    {
        // pairing-handshake-hardening (tarefa 1.2): dump de registradores
        // sob demanda, sem mudar nenhum estado -- útil pra comparar contra
        // o dump automático de boot/entrada em pareamento em bancada.
        tlm_radio_regs("mestre", mestre_radio, "cmd");
        guarded_println("[MESTRE] Registradores do radio despejados (e=regs;why=cmd).");
    }
    // jammer-emission-hardware (caracterizacao): "hopset <n>" forca o enlace a
    // saltar so nos primeiros N canais da tabela -- ver handle_hopset() abaixo
    // (definida depois das globais de controle que ela usa).
    else if (try_handle_hopset(line))
    {
        // tratado em handle_hopset
    }
    else
    {
        guarded_printf("[MESTRE] Comando desconhecido: %s\n", line);
    }
}

static void process_mestre_serial()
{
    while (Serial.available())
    {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r')
        {
            if (g_cmd_len > 0)
            {
                g_cmd_buf[g_cmd_len] = '\0';
                handle_mestre_command(g_cmd_buf);
                g_cmd_len = 0;
            }
            continue;
        }
        if (g_cmd_len < sizeof(g_cmd_buf) - 1)
            g_cmd_buf[g_cmd_len++] = c;
        else
            g_cmd_len = 0;
    }
}

// ---- Estado de pareamento (adaptive-rendezvous) ----
// g_link_established é declarada mais acima (perto de g_seed/wait_for_seed_window())
// -- handle_mestre_command() (comando "seed <n>") precisa dela antes deste
// ponto do arquivo.
static uint8_t g_pair_channel = PAIRING_CANDIDATES[0];
static bool g_camping = false;
static unsigned long g_pair_camp_start = 0;
static unsigned long g_last_link_try = 0;

// pairing-handshake-hardening (D3, tarefa 3.4): nonce de sessão da
// tentativa de pareamento corrente -- novo a cada acampamento
// (esp_random(), gerado em pick_new_pair_channel()), carregado em toda
// LinkProbeV2 desta janela. Só um LinkAckV2 que ecoe este nonce (junto com
// a seed corrente e PAIRING_PROTO_VER) é aceito como fechamento do
// handshake -- ver pair_accept_ack_mestre() em shared/pairing.h.
static uint16_t g_pair_nonce = 0;

// adaptive-rendezvous (2026-09-24): "blacklist de pareamento" -- mesma
// lógica de update_channel_mask() (blacklist-decay), aplicada aos 8
// PAIRING_CANDIDATES em vez dos 16 HOPPING_CHANNELS. g_candidate_mask usa
// só os 8 bits baixos (bit i = 1 -> PAIRING_CANDIDATES[i] disponível pra
// medir/acampar). g_pair_round é o "relógio lógico" aqui (não existe
// hop_counter antes de parear) -- incrementado a cada resultado de
// tentativa (sucesso ou falha) registrado em note_candidate_result().
static uint16_t g_candidate_mask = (1u << PAIRING_CANDIDATE_COUNT) - 1;
static uint8_t g_candidate_fail[PAIRING_CANDIDATE_COUNT] = {0};
static uint32_t g_candidate_quarantined_since[PAIRING_CANDIDATE_COUNT] = {0};
static uint32_t g_pair_round = 0;
static uint8_t g_camped_idx = 0; // índice (em PAIRING_CANDIDATES) do candidato atual, p/ penalizar/confirmar

// pairing-handshake-hardening (D5, tarefa 4.3): evidência da tentativa de
// pareamento CORRENTE no candidato acampado -- usadas por
// reset_to_pairing() pra decidir se o candidato deve ser penalizado (ver
// note_candidate_result()). Regra: só conta falha quando houve ack válido
// (evidência de presença do escravo) E o enlace não chegou a confirmar
// dado real antes de cair; um acampamento sem nenhuma resposta não
// penaliza (ver design.md D5). Zeradas a cada novo acampamento
// (pick_new_pair_channel()).
static bool g_pair_link_ack_received = false; // ack válido aceito nesta tentativa
static bool g_pair_data_confirmed = false;    // primeiro FragmentAck já confirmou dado real

// pairing-handshake-hardening (tarefa 1.3): contadores da janela de
// acampamento corrente -- zerados a cada novo acampamento
// (pick_new_pair_channel()), emitidos como "e=pair_stats" quando a janela
// termina (por timeout ou por sucesso) -- ver design.md H1/H4: distinguir
// "nenhum pacote chegou" (rx_total=0) de "chegou algo que não era o ack
// esperado" (rx_other>0) é o que discrimina as duas hipóteses.
static uint16_t g_pair_probes_ok = 0;
static uint16_t g_pair_probes_fail = 0;
static uint16_t g_pair_rx_total = 0;
static uint16_t g_pair_rx_link_ack = 0;
static uint16_t g_pair_rx_other = 0;
// pairing-handshake-hardening (D3, tarefa 3.6): LinkAckV2 bem formado (tipo
// e tamanho certos) mas com nonce/seed/ver que não batem com a tentativa
// corrente -- FIFO antiga, ack de uma sessão anterior, ou escravo em outra
// sessão (ver pair_accept_ack_mestre() em shared/pairing.h e o Scenario
// "Resposta que não pertence à tentativa corrente" na spec).
static uint16_t g_pair_stale_ack = 0;
// pairing-handshake-hardening (tarefa 2.3): quantas vezes o flush_rx() do
// início do acampamento (pick_new_pair_channel()) encontrou a FIFO
// não-vazia -- mede H1 (design.md): FIFO velha atravessando a troca de
// canal/estado de pareamento.
static uint16_t g_pair_flushed = 0;
// tarefa 1.5: só imprime a linha humana na primeira falha de envio de
// sonda de CADA janela, pra não inundar a serial numa janela ruim inteira
// -- a contagem completa (probes_fail) já vai pro pair_stats de qualquer
// forma.
static bool g_pair_probe_fail_reported = false;

static void tlm_pair_stats(uint8_t ch, unsigned long window_ms)
{
    tlm_printf("mestre", "pair_stats",
               "ch=%u;probes_ok=%u;probes_fail=%u;rx_total=%u;rx_link_ack=%u;rx_other=%u;stale_ack=%u;flushed=%u;ms=%lu",
               ch, g_pair_probes_ok, g_pair_probes_fail, g_pair_rx_total, g_pair_rx_link_ack,
               g_pair_rx_other, g_pair_stale_ack, g_pair_flushed, window_ms);
}

// ---- Estado de hopping / mensagem ----
static uint16_t g_msg_id = 1;
static uint8_t g_current_fragment = 0;
static uint8_t g_total_fragments = 0;
static uint32_t g_hop_counter = 0;
static unsigned long g_hop_start_at = 0;
// time-slotted-hopping: false até o preâmbulo de sincronismo travar o relógio no
// primeiro ack (bootstrap). Enquanto false, o mestre SEGURA o canal do slot 0 e
// retransmite o fragmento 0; no primeiro ack, âncora = agora e começa o salto
// por tempo. Sem isto, o skew de fase do handshake deixaria os dois lados em
// slots (canais) diferentes permanentemente -- não há pacote pra corrigir se
// eles nunca se encontram. Resetado em reset_to_pairing().
static bool g_time_synced = false;
static uint8_t g_current_channel = PAIRING_CANDIDATES[0];
static uint32_t g_hops_since_ack = 0;

// jamming-mitigation-hardening: só atribui falha de canal (para blacklist)
// quando a perda é ISOLADA -- o hop imediatamente anterior teve ack, ou seja
// g_hops_since_ack == 1 logo após o incremento deste miss. Um jammer de canal
// fixo produz exatamente isso: sucessos nos canais limpos e um miss isolado
// no canal atacado. Já um dessincronismo (escravo estacionado) produz uma
// RAJADA de misses -- do 2º em diante g_hops_since_ack > 1 e não atribuímos,
// evitando contaminar canais inocentes (achado 2026-09-25: parks em ch 46/82
// banindo canais que não estavam jammed). O miss ainda conta para
// g_hops_since_ack/LOSS_HOPS; só não vira evidência de canal ruim.
static const uint32_t ATTRIB_MAX_HOPS_SINCE_ACK = 1;

// jamming-mitigation-hardening: índice do canal que o escravo reportou como
// suspeito de jamming no último FragmentAck (SUSPECT_NONE = nenhum). É a
// evidência autoritativa (o escravo enxerga o canal jammed de forma limpa);
// consumida logo após o ack_ok para forçar o ban do canal certo.
static uint8_t g_rx_suspect_idx = SUSPECT_NONE;

// jamming-mitigation-hardening: histerese anti-thrash. Cada troca de máscara
// abre uma breve janela em que mestre e escravo podem ficar em canais
// diferentes (sincronização por apply_at_hop), causando misses -> mais bans
// -> mais trocas (ciclo vicioso observado: mediana de 6-8 canais banidos
// oscilando). Depois de aplicar uma máscara, o mestre não propõe outra troca
// por MASK_HYSTERESIS_HOPS hops -- tempo do enlace estabilizar antes de
// reavaliar. Assim converge para banir só o canal atacado e fica estável.
static uint32_t g_last_mask_change_hop = 0;
// hop-ack-decoupling: re-derivado de 16 (@ ate 220 ms/hop ~= 3,5 s) para
// preservar ~3,5 s de estabilizacao na nova cadencia (~30 ms/hop): 116 -> 112.
static const uint32_t MASK_HYSTERESIS_HOPS = 112;

// ---- dynamic-seed-blacklist: máscara de canais ativos e evidência própria ----
static uint16_t g_channel_mask = 0xFFFF;
static uint8_t g_channel_fail[TABLE_SIZE] = {0};
// blacklist-decay: hop_counter em que cada canal foi excluído pela última
// vez -- só usado enquanto o bit correspondente está em 0 em g_channel_mask
// (ver update_channel_mask() em shared/hopping.h). Zerado em
// reset_to_pairing() junto com g_channel_fail/g_channel_mask (evita
// underflow com g_hop_counter reiniciado -- ver design.md do change).
static uint32_t g_channel_blacklisted_since[TABLE_SIZE] = {0};
static bool g_alert_active = false;
static bool g_reseed_needed = false;

// channel-quality-sensing: janela de miss do enlace por slot (ack em qualquer
// tentativa = ok), fechada a cada THROUGHPUT_WINDOW_HOPS slots. Separada da
// throughput_window, que conta fragmentos e não slots.
static uint32_t g_chq_win_slots = 0;
static uint32_t g_chq_win_miss = 0;
static uint8_t g_chq_post_bl_miss_windows = 0; // janelas consecutivas com miss alto após blacklist
static bool g_chq_reseed_cond_prev = false;    // detector de borda da condição de reseed

// scenario-automation: throughput (fragmentos confirmados/s) por janela de
// hops -- mesmo padrão de janela de plr-metric (escravo, g_window_size).
// hop-ack-decoupling: re-derivado de 50 para 350 (~7x), preservando a mesma
// janela de tempo de medicao na cadencia de ~30 ms/hop (par com g_window_size
// do escravo). ATTRIB_MAX_HOPS_SINCE_ACK fica em 1 (nao re-derivado): conta
// misses ISOLADOS -- com lockstep, uma visita ao canal jammed ainda e 1 hop
// sem ack, entao o gate de "perda isolada = 1 hop" continua correto.
static const uint32_t THROUGHPUT_WINDOW_HOPS = 350;
static uint32_t g_throughput_window_confirmed = 0;
static uint32_t g_throughput_window_hops = 0;
static unsigned long g_throughput_window_start_ms = 0;

// Fila de controle: um pacote pendente por vez (seed tem prioridade sobre
// blacklist -- ver design.md de dynamic-seed-blacklist). 0 = nenhum.
static uint8_t g_control_pending = 0; // MSG_CTRL_SEED ou MSG_CTRL_BLACKLIST
static uint16_t g_control_seed_value = 0;
static uint16_t g_control_mask_value = 0;
static uint32_t g_control_apply_at = 0;
static uint8_t g_control_retries_left = 0;

static bool g_seed_apply_scheduled = false;
static uint16_t g_seed_apply_value = 0;
static uint32_t g_seed_apply_at = 0;

static bool g_mask_apply_scheduled = false;
static uint16_t g_mask_apply_value = 0;
static uint32_t g_mask_apply_at = 0;

static const char kFullMessage[] = "Mensagem louca enviada em partes durante os saltos FHSS para validar continuidade de comunicaaaa.";

static uint8_t find_channel_index(uint8_t ch)
{
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
        if (HOPPING_CHANNELS[i] == ch)
            return i;
    return 0;
}

// -------------------------------------------------------------
// Pareamento: mestre mede atividade e acampa no candidato mais silencioso
// -------------------------------------------------------------
// adaptive-rendezvous (2026-09-24): só considera candidatos habilitados em
// g_candidate_mask -- um candidato "quarentenado" (falhou repetidamente no
// handshake real, mesmo medindo quieto por RPD -- ver note_candidate_result())
// fica de fora da medição/seleção até o cooldown vencer.
static uint8_t measure_and_pick_candidate()
{
    uint8_t best_idx = 255;
    int best_activity = 999;

    for (uint8_t i = 0; i < PAIRING_CANDIDATE_COUNT; i++)
    {
        if (!(g_candidate_mask & (1u << i)))
            continue;

        mestre_radio.setChannel(PAIRING_CANDIDATES[i]);
        mestre_radio.startListening();
        int hits = 0;
        for (uint32_t s = 0; s < PAIR_MEASURE_SAMPLES; s++)
        {
            delayMicroseconds(PAIR_MEASURE_DWELL_US);
            if (mestre_radio.testCarrier())
                hits++;
        }
        mestre_radio.stopListening();

        if (hits < best_activity)
        {
            best_activity = hits;
            best_idx = i;
        }
    }

    // g_candidate_mask nunca deveria chegar a 0 (update_channel_mask() tem
    // a mesma guarda de blacklist-decay), mas se acontecer, não trava sem
    // candidato nenhum -- volta pro primeiro.
    return best_idx == 255 ? 0 : best_idx;
}

// adaptive-rendezvous (2026-09-24): registra o resultado (sucesso/falha) da
// última tentativa de pareamento no candidato atual e reavalia
// g_candidate_mask -- mesma função pura de blacklist-decay
// (update_channel_mask()), agora sobre PAIRING_CANDIDATES. Pairing-
// handshake-hardening (D5, tarefa 4.3) mudou QUANDO isto é chamado (ver
// reset_to_pairing() e o ramo `ack_ok` em mestre_loop()): sucesso é o
// primeiro FragmentAck depois do pareamento, falha é um ack válido aceito
// seguido de reset_to_pairing() sem nenhum FragmentAck em LOSS_HOPS hops --
// um acampamento sem nenhuma resposta NÃO chama esta função (não é mais
// evidência de canal ruim, só de escravo ausente).
static void note_candidate_result(bool success)
{
    uint8_t idx = g_camped_idx;
    if (success)
        g_candidate_fail[idx] = 0;
    else if (g_candidate_fail[idx] < 255)
        g_candidate_fail[idx]++;

    g_pair_round++;
    uint16_t new_mask = update_channel_mask(g_candidate_mask, g_candidate_fail, g_candidate_quarantined_since,
                                             g_pair_round, PAIR_FAIL_THRESHOLD, PAIR_RECOVERY_ROUNDS,
                                             PAIRING_CANDIDATE_COUNT);
    if (new_mask != g_candidate_mask)
    {
        tlm_printf("mestre", "pair_mask", "mask=0x%02X;prev=0x%02X", new_mask, g_candidate_mask);
        g_candidate_mask = new_mask;
    }
}

static void pick_new_pair_channel()
{
    uint8_t idx = measure_and_pick_candidate();
    g_camped_idx = idx;
    g_pair_channel = PAIRING_CANDIDATES[idx];
    g_pair_camp_start = millis();
    g_camping = true;
    // tarefa 1.3: nova janela de acampamento -- zera os contadores da
    // anterior (já emitidos via tlm_pair_stats() por quem chamou isto).
    g_pair_probes_ok = 0;
    g_pair_probes_fail = 0;
    g_pair_rx_total = 0;
    g_pair_rx_link_ack = 0;
    g_pair_rx_other = 0;
    g_pair_stale_ack = 0;
    g_pair_probe_fail_reported = false;

    // D5 (tarefa 4.3): nova tentativa de candidato -- nenhuma evidência
    // ainda de presença do escravo nem de dado confirmado (ver
    // reset_to_pairing()/note_candidate_result()).
    g_pair_link_ack_received = false;
    g_pair_data_confirmed = false;

    // D3 (tarefa 3.4): nonce novo por acampamento -- identifica esta
    // tentativa perante o escravo (ver shared/pairing.h). esp_random() (HW
    // RNG do ESP32) em vez de random()/rand() -- não precisa ser
    // criptograficamente seguro (não-goal do change, ver design.md), só
    // não repetir por coincidência entre tentativas consecutivas.
    g_pair_nonce = (uint16_t)esp_random();

    // pairing-handshake-hardening (tarefa 2.3): measure_and_pick_candidate()
    // liga o receptor (startListening()) em cada candidato avaliado -- um
    // pacote real pode ter caído na FIFO durante a medição. Descarta antes
    // da primeira sonda deste acampamento (D2, design.md) e conta se havia
    // algo pra descartar (mede H1: FIFO velha atravessando troca de canal).
    g_pair_flushed = mestre_radio.available() ? 1 : 0;
    mestre_radio.flush_rx();

    tlm_printf("mestre", "pair_scan", "ch=%u", g_pair_channel);
}

// pairing-handshake-hardening (D3, tarefa 3.4): handshake de 2 mensagens --
// a sonda já carrega g_pair_nonce e g_seed; só um LinkAckV2 que ecoe os
// dois (mais PAIRING_PROTO_VER) fecha o handshake (pair_accept_ack_mestre(),
// shared/pairing.h). Não existe mais uma troca de seed separada:
// send_seed_to_slave() foi removida (era código morto assim que a seed
// passou a viajar na própria sonda/ack) e quem chama try_establish_link()
// agenda o início do salto direto no sucesso.
static bool try_establish_link()
{
    LinkProbeV2 probe;
    probe.type = MSG_LINK_PROBE;
    probe.ver = PAIRING_PROTO_VER;
    probe.nonce = g_pair_nonce;
    probe.seed = g_seed;
    probe.mask = g_channel_mask; // jamming-mitigation-hardening: blacklist persiste

    mestre_radio.setChannel(g_pair_channel);
    mestre_radio.stopListening();
    // radio_send() em vez de write(): ver shared/protocol.h para o porque
    // (bug de retry-forever em RF24::write() nesta versao da lib quando
    // TX_DS/TX_DF nunca setam, reproduzido em bancada com o escravo fora
    // do canal na hora da sonda).
    bool sent = radio_send(mestre_radio, &probe, sizeof(probe));
    if (!sent)
    {
        // tarefa 1.5: falha de radio_send() na sonda deixa de ser
        // silenciosa -- contada em pair_stats, com linha humana só na
        // primeira ocorrência desta janela.
        g_pair_probes_fail++;
        if (!g_pair_probe_fail_reported)
        {
            g_pair_probe_fail_reported = true;
            guarded_printf("[MESTRE] Falha ao enviar sonda de pareamento em ch=%u (radio_send).\n", g_pair_channel);
        }
        mestre_radio.startListening();
        return false;
    }
    g_pair_probes_ok++;

    mestre_radio.startListening();
    unsigned long start_wait = millis();
    while (millis() - start_wait < LINK_ACK_WAIT_MS)
    {
        if (!mestre_radio.available())
            continue;

        g_pair_rx_total++;
        uint8_t size = mestre_radio.getDynamicPayloadSize();
        if (size != sizeof(LinkAckV2))
        {
            uint8_t dump[32];
            mestre_radio.read(dump, size);
            g_pair_rx_other++;
            continue;
        }

        LinkAckV2 ack;
        mestre_radio.read(&ack, sizeof(ack));
        if (ack.type != MSG_LINK_ACK)
        {
            g_pair_rx_other++;
            continue;
        }
        g_pair_rx_link_ack++;

        if (pair_accept_ack_mestre(g_pair_nonce, g_seed, ack.ver, ack.nonce, ack.seed))
            return true;

        // Scenario "Resposta que não pertence à tentativa corrente" da spec:
        // ver/nonce/seed não batem -- FIFO antiga, ack de uma tentativa
        // anterior, ou escravo em outra sessão. Descarta e continua
        // sondando (não retorna false aqui -- ainda há tempo dentro de
        // LINK_ACK_WAIT_MS pra um ack válido chegar).
        g_pair_stale_ack++;
    }

    return false;
}

static uint8_t compute_total_fragments()
{
    size_t len = strlen(kFullMessage);
    return (uint8_t)((len + FRAGMENT_DATA_SIZE - 1) / FRAGMENT_DATA_SIZE);
}

static void fill_fragment(FragmentPacket &pkt, uint8_t fragment_index)
{
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = MSG_FRAGMENT;
    pkt.msg_id = g_msg_id;
    pkt.fragment_index = fragment_index;
    pkt.total_fragments = g_total_fragments;
    pkt.hop_counter = g_hop_counter;

    size_t start = (size_t)fragment_index * FRAGMENT_DATA_SIZE;
    size_t msg_len = strlen(kFullMessage);
    size_t remaining = (start < msg_len) ? (msg_len - start) : 0;
    size_t chunk = remaining > FRAGMENT_DATA_SIZE ? FRAGMENT_DATA_SIZE : remaining;
    pkt.payload_len = (uint8_t)chunk;
    if (chunk > 0)
        memcpy(pkt.payload, kFullMessage + start, chunk);
}

static bool wait_fragment_ack(uint8_t expected_fragment, bool &all_complete, uint32_t timeout_ms)
{
    all_complete = false;
    unsigned long start_wait = millis();
    while (millis() - start_wait < timeout_ms)
    {
        if (!mestre_radio.available())
            continue;

        uint8_t sz = mestre_radio.getDynamicPayloadSize();
        if (sz != sizeof(FragmentAck))
        {
            uint8_t dump[32];
            mestre_radio.read(dump, sz);
            continue;
        }

        FragmentAck ack;
        mestre_radio.read(&ack, sizeof(ack));
        if (ack.type == MSG_FRAGMENT_ACK && ack.msg_id == g_msg_id && ack.fragment_index == expected_fragment)
        {
            all_complete = (ack.complete != 0);
            g_rx_suspect_idx = ack.suspect_idx; // jamming-mitigation-hardening
            return true;
        }
    }
    return false;
}

static bool wait_control_ack(uint32_t expected_apply_at, uint32_t timeout_ms)
{
    unsigned long start_wait = millis();
    while (millis() - start_wait < timeout_ms)
    {
        if (!mestre_radio.available())
            continue;

        uint8_t sz = mestre_radio.getDynamicPayloadSize();
        if (sz != sizeof(ControlAck))
        {
            uint8_t dump[32];
            mestre_radio.read(dump, sz);
            continue;
        }

        ControlAck ack;
        mestre_radio.read(&ack, sizeof(ack));
        if (ack.type == MSG_CTRL_ACK && ack.apply_at_hop == expected_apply_at)
            return true;
    }
    return false;
}

// -------------------------------------------------------------
// dynamic-seed-blacklist / blacklist-decay: gatilho de reseed (borda do
// alerta) e avaliação contínua de blacklist -- exclusão E reinclusão, sem
// depender do alerta estar ativo (ver design.md de blacklist-decay: o
// alerta continua sendo o único gatilho de reseed, uma ação mais
// disruptiva, mas blacklist virou uma correção contínua de baixo custo).
// -------------------------------------------------------------
static uint16_t compute_candidate_mask()
{
    // channel-quality-sensing: o piso de canais ativos só vale com o sensor;
    // sem ele (4 placas) min_active=1 preserva o comportamento standalone.
    uint16_t sensor_mask = g_mestre_chq_source ? g_mestre_chq_source() : 0;
    uint8_t min_active = g_mestre_chq_source ? HOP_MIN_ACTIVE_CHANNELS : 1;
    return update_channel_mask(g_channel_mask, g_channel_fail, g_channel_blacklisted_since,
                                g_hop_counter, CHANNEL_FAIL_THRESHOLD, RECOVERY_COOLDOWN_HOPS,
                                TABLE_SIZE, sensor_mask, min_active);
}

// channel-quality-sensing: contabiliza um slot na janela de miss; ao fechar,
// atualiza as janelas consecutivas com miss alto após blacklist aplicada.
static void chq_note_slot(bool acked)
{
    g_chq_win_slots++;
    if (!acked)
        g_chq_win_miss++;
    if (g_chq_win_slots < THROUGHPUT_WINDOW_HOPS)
        return;
    uint8_t miss_pct = (uint8_t)((g_chq_win_miss * 100u) / g_chq_win_slots);
    g_chq_post_bl_miss_windows = chq_update_post_blacklist_miss_windows(
        g_chq_post_bl_miss_windows, g_channel_mask != 0xFFFF, miss_pct);
    g_chq_win_slots = 0;
    g_chq_win_miss = 0;
}

// Zera a janela/contador em reset_to_pairing().
static void chq_reset_miss_window()
{
    g_chq_win_slots = 0;
    g_chq_win_miss = 0;
    g_chq_post_bl_miss_windows = 0;
    // g_chq_reseed_cond_prev fica de fora: zerar causaria um reseed a cada
    // re-pareamento enquanto a banda seguir degradada.
}

// blacklist-decay (achado de bancada 2026-09-24): mascara-alvo EFETIVA --
// o valor ja agendado (ainda nao aplicado por apply_scheduled_changes())
// quando houver, senao a mascara ja aplicada. Usada por
// decide_control_proposal() (shared/control_policy.h) pra nunca reproposta
// algo identico ao que ja esta a caminho.
static uint16_t effective_channel_mask()
{
    return g_mask_apply_scheduled ? g_mask_apply_value : g_channel_mask;
}

// jammer-emission-hardware (caracterizacao): forca uma mascara de N canais
// (primeiros N bits da tabela) e a propaga pela MESMA maquinaria de blacklist
// (control packet + ack + apply nos dois lados). Usar com "mitigation off"
// para a blacklist automatica nao sobrescrever. Retorna true se tratou a linha.
static bool try_handle_hopset(char *line)
{
    if (strncmp(line, "hopset ", 7) != 0)
        return false;
    int n = atoi(line + 7);
    if (n < 1)
        n = 1;
    if (n > TABLE_SIZE)
        n = TABLE_SIZE;
    uint16_t m = (n >= TABLE_SIZE) ? 0xFFFF : (uint16_t)((1u << n) - 1);
    if (g_control_pending == 0)
    {
        g_control_pending = MSG_CTRL_BLACKLIST;
        g_control_mask_value = m;
        g_control_apply_at = g_hop_counter + CONTROL_RETRY_HOPS + CONTROL_APPLY_MARGIN;
        g_control_retries_left = CONTROL_RETRY_HOPS;
        tlm_printf("mestre", "hopset_tx", "n=%d;mask=0x%04X;apply=%lu", n, m, g_control_apply_at);
        guarded_printf("[MESTRE] hopset n=%d mask=0x%04X (propagando; use com mitigation off)\n", n, m);
    }
    else
        guarded_println("[MESTRE] controle pendente; repita o hopset em ~1s");
    return true;
}

static void maybe_enqueue_control()
{
    uint16_t candidate = compute_candidate_mask();
    ControlProposalDecision decision = decide_control_proposal(
        g_control_pending != 0, g_fixed_channel_mode, g_mitigation_enabled,
        g_seed_apply_scheduled, g_mask_apply_scheduled, g_reseed_needed,
        candidate, effective_channel_mask());

    if (decision.kind == ControlProposalKind::kNone)
        return;

    // jamming-mitigation-hardening: histerese anti-thrash -- não propõe nova
    // troca de máscara logo após aplicar uma (deixa o enlace estabilizar).
    // Não afeta seed/reseed (que é resposta a alerta, mais raro e disruptivo).
    if (decision.kind == ControlProposalKind::kBlacklist &&
        (g_hop_counter - g_last_mask_change_hop) < MASK_HYSTERESIS_HOPS)
        return;

    if (decision.kind == ControlProposalKind::kSeed)
    {
        g_control_pending = MSG_CTRL_SEED;
        g_control_seed_value = (uint16_t)random(0, 101);
        g_control_apply_at = g_hop_counter + CONTROL_RETRY_HOPS + CONTROL_APPLY_MARGIN;
        g_control_retries_left = CONTROL_RETRY_HOPS;
        g_reseed_needed = false;
        tlm_printf("mestre", "rekey_tx", "kind=seed;seed=%u;apply=%lu", g_control_seed_value, g_control_apply_at);
        return;
    }

    // ControlProposalKind::kBlacklist
    g_control_pending = MSG_CTRL_BLACKLIST;
    g_control_mask_value = decision.mask_value;
    g_control_apply_at = g_hop_counter + CONTROL_RETRY_HOPS + CONTROL_APPLY_MARGIN;
    g_control_retries_left = CONTROL_RETRY_HOPS;
    tlm_printf("mestre", "rekey_tx", "kind=blacklist;mask=0x%04X;apply=%lu", g_control_mask_value, g_control_apply_at);
}

// scenario-automation: throughput = fragmentos confirmados / segundo,
// numa janela de THROUGHPUT_WINDOW_HOPS hops (mesmo padrão de janela de
// plr-metric no escravo) -- conta tanto hops de controle quanto de dados
// no tamanho da janela, mas só fragmentos realmente confirmados (ack_ok)
// no numerador.
static void note_hop_for_throughput(bool fragment_confirmed)
{
    if (g_throughput_window_hops == 0)
        g_throughput_window_start_ms = millis();

    g_throughput_window_hops++;
    if (fragment_confirmed)
        g_throughput_window_confirmed++;

    if (g_throughput_window_hops >= THROUGHPUT_WINDOW_HOPS)
    {
        unsigned long elapsed_ms = millis() - g_throughput_window_start_ms;
        float frags_per_s = elapsed_ms > 0 ? (1000.0f * (float)g_throughput_window_confirmed / (float)elapsed_ms) : 0.0f;
        tlm_printf("mestre", "throughput", "frags_per_s=%.2f;confirmed=%u;window_ms=%lu",
                   frags_per_s, g_throughput_window_confirmed, elapsed_ms);
        g_throughput_window_hops = 0;
        g_throughput_window_confirmed = 0;
    }
}

static void reset_throughput_window()
{
    g_throughput_window_hops = 0;
    g_throughput_window_confirmed = 0;
    g_throughput_window_start_ms = 0;
}

static void apply_scheduled_changes()
{
    if (g_seed_apply_scheduled && g_hop_counter >= g_seed_apply_at)
    {
        g_seed = g_seed_apply_value;
        g_seed_apply_scheduled = false;
        tlm_printf("mestre", "rekey_apply", "kind=seed;seed=%u;hop=%lu", g_seed, g_hop_counter);
    }
    if (g_mask_apply_scheduled && g_hop_counter >= g_mask_apply_at)
    {
        uint16_t prev_mask = g_channel_mask;
        g_channel_mask = g_mask_apply_value;
        g_mask_apply_scheduled = false;
        g_last_mask_change_hop = g_hop_counter; // jamming-mitigation-hardening: histerese
        // blacklist-decay: prev= permite tools/logger e plot_scenario.py
        // distinguirem visualmente exclusão (bits somem) de reinclusão
        // (bits voltam) sem precisar rastrear estado próprio.
        tlm_printf("mestre", "blacklist", "mask=0x%04X;prev=0x%04X;hop=%lu", g_channel_mask, prev_mask, g_hop_counter);
    }
}

// Retorna true se o hop atual foi consumido por um pacote de controle (em
// vez de um fragmento de dado) -- fragmento fica pausado até o controle
// resolver (confirmar ou esgotar retries). `acked_out` reporta se ESTA
// tentativa em particular foi confirmada (usado pelo chamador pra manter
// g_hops_since_ack coerente -- ver achado de bancada 2026-09-24 no
// comentário de reset_to_pairing() abaixo).
static bool send_pending_control_if_any(bool &acked_out, uint32_t ack_wait_ms)
{
    acked_out = false;
    if (g_control_pending == 0)
        return false;

    bool acked = false;

    if (g_control_pending == MSG_CTRL_SEED)
    {
        ControlSeedPacket pkt;
        pkt.type = MSG_CTRL_SEED;
        pkt.new_seed = g_control_seed_value;
        pkt.apply_at_hop = g_control_apply_at;
        // hop-resync (tarefa 1.2a, design.md D3a): hop corrente do mestre --
        // um escravo estacionado (RESYNC_MISS_HOPS, shared/protocol.h) pode
        // nunca ver um FragmentPacket enquanto há controle pendente (este
        // pacote vai NO LUGAR do fragmento em todo hop até ser ackado), e
        // precisa deste campo pra adotar o contador e sair do
        // estacionamento (ver escravo/src/node.h).
        pkt.hop_counter = g_hop_counter;

        mestre_radio.stopListening();
        radio_send(mestre_radio, &pkt, sizeof(pkt));
        mestre_radio.startListening();
        acked = wait_control_ack(g_control_apply_at, ack_wait_ms);

        if (acked)
        {
            g_seed_apply_scheduled = true;
            g_seed_apply_value = g_control_seed_value;
            g_seed_apply_at = g_control_apply_at;
        }
    }
    else if (g_control_pending == MSG_CTRL_BLACKLIST)
    {
        ControlBlacklistPacket pkt;
        pkt.type = MSG_CTRL_BLACKLIST;
        pkt.mask = g_control_mask_value;
        pkt.apply_at_hop = g_control_apply_at;
        // hop-resync (tarefa 1.2a): mesmo motivo do ramo MSG_CTRL_SEED acima.
        pkt.hop_counter = g_hop_counter;

        mestre_radio.stopListening();
        radio_send(mestre_radio, &pkt, sizeof(pkt));
        mestre_radio.startListening();
        acked = wait_control_ack(g_control_apply_at, ack_wait_ms);

        if (acked)
        {
            g_mask_apply_scheduled = true;
            g_mask_apply_value = g_control_mask_value;
            g_mask_apply_at = g_control_apply_at;
        }
    }

    if (acked)
    {
        tlm_printf("mestre", "rekey_ack", "kind=%s;apply=%lu",
                   g_control_pending == MSG_CTRL_SEED ? "seed" : "blacklist", g_control_apply_at);
        g_control_pending = 0;
    }
    else if (g_control_retries_left > 0)
    {
        g_control_retries_left--;
    }
    else
    {
        tlm_printf("mestre", "rekey_failed", "kind=%s", g_control_pending == MSG_CTRL_SEED ? "seed" : "blacklist");
        g_control_pending = 0;
    }

    acked_out = acked;
    return true;
}

// pairing-handshake-hardening (tarefa 2.1): sequência de configuração do
// rádio extraída de mestre_setup() (D2, design.md) -- fonte única de
// verdade: setup() e toda (re)entrada no estado de pareamento chamam
// exatamente este código, na mesma ordem. Ordem de registrador importa
// nestes clones de NRF24 -- dois bugs reais já vieram de configuração de
// feature/ack fora de ordem (ver comentário do enableAckPayload() abaixo),
// então isto só MOVE o que já existia em mestre_setup(), sem reordenar.
// Não inclui Serial/SPI.begin()/radio.begin()/pinMode/printDetails/boot --
// isso continua só em mestre_setup() (não faz sentido repetir a cada
// reentrada em pareamento).
static void radio_reinit()
{
    // D2: power-cycle lógico -- força o rádio pra um estado conhecido
    // antes de reconfigurar. powerUp() (RF24 1.6.2, RF24.cpp) já espera
    // RF24_POWERUP_DELAY (5000us) internamente antes de retornar -- dentro
    // da janela de 1.5-5ms do datasheet (Tpd2stby) -- então nenhuma espera
    // adicional é necessária aqui.
    mestre_radio.powerDown();
    mestre_radio.powerUp();

    mestre_radio.setPALevel(RF24_PA_LOW);
    mestre_radio.setDataRate(RF24_1MBPS);
    mestre_radio.setChannel(g_pair_channel);

    mestre_radio.enableDynamicPayloads();
    // NAO chamar enableAckPayload(): achado de bancada -- nestes módulos
    // (clones comuns baseados em SI24R1), habilitar ack payload junto com
    // envios NO_ACK (radio_send(), usado em todo write() deste projeto)
    // faz o receptor descartar o pacote silenciosamente, sem nunca disparar
    // available() -- reproduzido de forma isolada e 100% repetível com
    // Tests/AntennaCross (setAutoAck(true) sozinho: ~100% de entrega;
    // + enableAckPayload(): ~0%). writeAckPayload() nunca é chamado neste
    // protocolo (a confirmação real vem de pacotes de resposta explícitos
    // -- LinkAck/SeedAck/FragmentAck/ControlAck), então a feature nunca fez
    // falta.
    //
    // adaptive-rendezvous: enableDynamicAck() é necessário para
    // write(..., multicast=true) na sonda de pareamento (ver
    // try_establish_link) -- sem isso o bit EN_DYN_ACK do FEATURE register
    // nao é setado e o comando W_TX_PAYLOAD_NO_ACK não tem efeito.
    mestre_radio.enableDynamicAck();

    mestre_radio.setCRCLength(RF24_CRC_16);
    mestre_radio.openWritingPipe(slaveAddress);
    mestre_radio.openReadingPipe(1, masterAddress);

    mestre_radio.flush_tx();
    mestre_radio.setAutoAck(true);
    // D2: limpa os bits de IRQ de STATUS explicitamente -- o estado final
    // abaixo é stopListening(), que (ao contrário de startListening(), ver
    // radio_reinit() do escravo) não escreve STATUS sozinho.
    mestre_radio.clearStatusFlags();
    mestre_radio.stopListening();
}

static void reset_to_pairing(bool clear_blacklist)
{
    // jamming-mitigation-hardening: reset por jamming PRESERVA a blacklist
    // (clear_blacklist=false, default); reset limpo por comando do operador a
    // ZERA (clear_blacklist=true). Ver o comando "reset" em
    // handle_mestre_command() e o design da persistência.
    if (clear_blacklist)
    {
        g_channel_mask = 0xFFFF;
        memset(g_channel_fail, 0, sizeof(g_channel_fail));
        memset(g_channel_blacklisted_since, 0, sizeof(g_channel_blacklisted_since));
    }
    // D5 (tarefa 4.3): avalia o candidato ANTES de zerar as flags da
    // tentativa -- só conta falha quando houve ack válido aceito (evidência
    // de presença do escravo) e o enlace não chegou a confirmar dado real
    // (FragmentAck) antes de cair. reset_to_pairing() só é chamado depois
    // de LOSS_HOPS sem ack (ou por comando "reset" manual), nunca por um
    // acampamento sem resposta nenhuma -- esse caso nem chega a chamar
    // esta função (ver o bloco `if (!g_link_established)` em mestre_loop()).
    if (g_pair_link_ack_received && !g_pair_data_confirmed)
        note_candidate_result(false);
    g_pair_link_ack_received = false;
    g_pair_data_confirmed = false;

    g_link_established = false;
    g_camping = false;
    g_hop_counter = 0;
    g_time_synced = false; // time-slotted-hopping: re-sincroniza no próximo pareamento
    g_hops_since_ack = 0;
    g_current_fragment = 0;
    g_control_pending = 0;
    g_seed_apply_scheduled = false;
    g_mask_apply_scheduled = false;
    // jamming-mitigation-hardening: NÃO zerar a blacklist num reset causado
    // por jamming -- sob um jammer forte o enlace re-pareia toda hora, e
    // zerar aqui fazia a máscara oscilar sem nunca convergir para banir o
    // canal atacado (achado de bancada 2026-09-25, jammer-emission-hardware:
    // mitigação recuperava só ~17%). Mantemos g_channel_mask/g_channel_fail;
    // como g_hop_counter voltou a 0, blacklisted_since precisa voltar a 0
    // também (senão hop_counter - blacklisted_since faz underflow unsigned e
    // reincluiria tudo na hora -- ver blacklist-decay). Com os dois em 0, o
    // cooldown de recuperação recomeça do início da nova sessão: o canal
    // banido continua banido por RECOVERY_COOLDOWN_HOPS e só então é
    // re-testado (dá chance de recuperar se o jammer saiu). A máscara viaja
    // na LinkProbeV2, então o escravo re-pareado adota o mesmo conjunto.
    memset(g_channel_blacklisted_since, 0, sizeof(g_channel_blacklisted_since));
    reset_throughput_window();
    chq_reset_miss_window();
    tlm_event("mestre", "loss_of_sync");
    // tarefa 1.2: dump de registradores toda vez que o mestre entra (ou
    // volta a entrar) no estado de pareamento -- compara contra o dump do
    // boot pra confirmar/descartar H4 (design.md).
    // pairing-handshake-hardening (tarefa 2.2): dump ANTES do
    // radio_reinit() (why=loss_pre) -- sem isto o reinit apagaria o exato
    // estado de rádio que motivou o reset antes de registrá-lo, e é
    // justamente esse estado que discrimina H4 (design.md).
    tlm_radio_regs("mestre", mestre_radio, "loss_pre");
    radio_reinit();
    tlm_radio_regs("mestre", mestre_radio, "loss");
}

static void mestre_setup()
{
    Serial.begin(115200);
    delay(1000); // Aguarda o monitor serial abrir

    pinMode(LED_BUILTIN, OUTPUT);
    if (!mestre_is_combined())
        pinMode(PIN_ALERT_IN, INPUT_PULLDOWN);

    guarded_println("\n[MESTRE] Iniciando transmissor (FIXO)...");

    delay(2000);

    printf_begin();

    pinMode(PIN_CSN, OUTPUT);
    digitalWrite(PIN_CSN, HIGH);
    delay(10);

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);

    if (!mestre_radio.begin())
    {
        guarded_println(F("[ERRO MESTRE] Falha fatal! O Módulo NRF24 não respondeu no SPI. Verifique a fiação."));
    }

    // pairing-handshake-hardening (tarefa 2.1): configuração do rádio
    // extraída para radio_reinit() (definida acima) -- setup() e toda
    // (re)entrada em pareamento usam exatamente a mesma sequência, na
    // mesma ordem (fonte única de verdade, D2 em design.md).
    radio_reinit();

    guarded_println("\n--- STATUS DO RÁDIO MESTRE (REGISTRADORES) ---");
    mestre_radio.printDetails();
    guarded_println("----------------------------------------------\n");

    g_total_fragments = compute_total_fragments();
    guarded_printf("[MESTRE] Rádio configurado. Total de fragmentos: %u\n", g_total_fragments);

    wait_for_seed_window();

    tlm_event("mestre", "boot");
    // tarefa 1.2: número-base de registradores no boot, pra comparar
    // contra os dumps de entrada em pareamento por perda (design.md H4).
    tlm_radio_regs("mestre", mestre_radio, "boot");
}

static void mestre_loop()
{
    process_mestre_serial();

    // ---- BLINK NÃO-BLOQUEANTE (HEARTBEAT) ----
    // Só na placa standalone: na Placa A combinada, GPIO 2 é o MISO do
    // barramento HSPI usado pela task do analisador (two-board-topology
    // seção 7) -- a mesma condição usada para o pino de alerta indica
    // modo combinado.
    if (!mestre_is_combined())
    {
        static unsigned long last_blink = 0;
        if (millis() - last_blink >= 500)
        {
            last_blink = millis();
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        }
    }

    // ---- ALERTA DO ANALISADOR (spectrum-attack-classification) ----
    // channel-quality-sensing: com fonte de máscara, alerta = máscara != 0
    // (só telemetria) e o reseed vem da política abaixo, não da borda.
    uint16_t chq_mask = g_mestre_chq_source ? g_mestre_chq_source() : 0;
    bool alert_now = g_mestre_chq_source ? (chq_mask != 0)
                     : (g_mestre_alert_source ? g_mestre_alert_source() : (digitalRead(PIN_ALERT_IN) == HIGH));
    if (g_mestre_chq_source)
    {
        // Dispara por borda: g_reseed_needed é nível, e uma condição
        // persistente que reseed não resolve (ex. Wi-Fi larga) reseedaria
        // a cada liberação da guarda. No braço de miss, zera o contador.
        bool reseed_cond = should_reseed_for_channel_quality(
            mask_popcount16(chq_mask), g_chq_post_bl_miss_windows);
        if (reseed_cond && !g_chq_reseed_cond_prev)
        {
            g_reseed_needed = true;
            if (g_chq_post_bl_miss_windows >= CHQ_RESEED_MISS_WINDOWS)
                g_chq_post_bl_miss_windows = 0;
        }
        g_chq_reseed_cond_prev = reseed_cond;
    }
    if (alert_now != g_alert_active)
    {
        if (alert_now)
        {
            if (!g_mestre_chq_source)
                g_reseed_needed = true;
            guarded_println("[MESTRE] ALERTA: ataque detectado (analisador).");
            tlm_event("mestre", "alert_on");
        }
        else
        {
            guarded_println("[MESTRE] ALERTA: ataque encerrado.");
            tlm_event("mestre", "alert_off");
        }
        g_alert_active = alert_now;
    }

    if (!g_link_established)
    {
        // pairing-handshake-hardening (D5, tarefa 4.3): um acampamento que
        // esgota sem NENHUMA resposta válida não é mais evidência de canal
        // ruim (ver design.md D5 e o Scenario "Escravo ausente não esvazia
        // a lista de candidatos") -- nao chama note_candidate_result() aqui.
        // A avaliação do candidato agora acontece só depois de um ack
        // válido ter sido aceito (ver o `if (try_establish_link())` abaixo
        // e reset_to_pairing()).
        bool camp_expired = g_camping && (millis() - g_pair_camp_start >= PAIR_CAMP_MS);
        if (camp_expired)
        {
            // tarefa 1.3: fecha a janela de acampamento que acabou de
            // esgotar -- emitir ANTES de pick_new_pair_channel() zerar os
            // contadores e trocar g_pair_channel.
            tlm_pair_stats(g_pair_channel, (unsigned long)(millis() - g_pair_camp_start));
        }
        if (!g_camping || camp_expired)
            pick_new_pair_channel();

        if (millis() - g_last_link_try >= LINK_RETRY_MS)
        {
            g_last_link_try = millis();
            if (try_establish_link())
            {
                // D3 (tarefa 3.4): a seed já foi confirmada pelo próprio
                // ack (ela veio ecoada nele, ver pair_accept_ack_mestre())
                // -- não existe mais uma troca de seed separada depois
                // disto; agenda o início do salto direto (mesma semântica
                // do antigo g_seed_confirmed).
                g_link_established = true;
                g_pair_link_ack_received = true; // D5: evidência de presença do escravo nesta tentativa
                g_hop_counter = 0;
                g_hops_since_ack = 0;
                g_hop_start_at = millis() + HOP_START_DELAY_MS;
                guarded_printf("[MESTRE] Pareamento confirmado em ch=%u nonce=%u. Iniciando salto...\n", g_pair_channel, g_pair_nonce);
                // tarefa 1.3: pareamento também fecha a janela corrente
                // (sucesso, não timeout) -- mesmos contadores acumulados
                // desde pick_new_pair_channel().
                tlm_pair_stats(g_pair_channel, (unsigned long)(millis() - g_pair_camp_start));
                tlm_printf("mestre", "pair_ok", "ch=%u;nonce=%u", g_pair_channel, g_pair_nonce);
            }
        }
        return;
    }

    if (millis() < g_hop_start_at)
        return;

    // time-slotted-hopping: o slot vem do TEMPO decorrido desde a âncora comum
    // do pareamento (g_hop_start_at), não de eventos. Cada chamada trata UM slot
    // por completo (bloqueante, como a era por-evento tratava um hop), mas o
    // canal e as fronteiras vêm do relógio -- o escravo calcula o mesmo slot do
    // mesmo tempo, então os dois estão no mesmo canal por construção, sem
    // depender de pacote pra se realinhar. Dentro do slot o mestre transmite o
    // fragmento corrente repetidamente (avançando o DADO só no ack -- split
    // hop-ack-decoupling); ao fim do slot, salta no tempo.
    // Ver openspec/changes/time-slotted-hopping/design.md.

    // Preâmbulo de sincronismo (bootstrap): enquanto o relógio não travou,
    // SEGURA o canal do slot 0 e retransmite o fragmento 0 até o primeiro ack.
    // Nesse instante, âncora = agora (slot 0 começa aqui) -- os dois lados
    // partem do mesmo evento (skew ~1 trânsito, << SLOT_MS), e daí em diante o
    // salto é por tempo. Sem isto, o skew de fase do handshake deixaria os dois
    // em canais diferentes permanentemente.
    if (!g_time_synced)
    {
        uint8_t ch0 = g_fixed_channel_mode ? TEST_FIXED_CHANNEL : get_next_active_channel(g_seed, 0, g_channel_mask);
        g_hop_counter = 0;
        g_current_channel = ch0;
        mestre_radio.setChannel(ch0);
        delayMicroseconds(500);
        mestre_radio.flush_rx();

        FragmentPacket pkt;
        fill_fragment(pkt, g_current_fragment);
        // slot-phase-servo: ancora so fixada quando o ack chega (abaixo)
        pkt.slot_phase_ms = PHASE_UNKNOWN;
        mestre_radio.stopListening();
        bool sent = radio_send(mestre_radio, &pkt, sizeof(pkt));
        mestre_radio.startListening();
        bool completed = false;
        if (sent && wait_fragment_ack(g_current_fragment, completed, ATTEMPT_ACK_WAIT_MS))
        {
            g_hop_start_at = millis(); // âncora real do relógio: slot 0 começa agora
            g_time_synced = true;
            g_hops_since_ack = 0;
            if (!g_pair_data_confirmed)
            {
                g_pair_data_confirmed = true;
                note_candidate_result(true);
            }
            note_hop_for_throughput(true);
            g_current_fragment++;
            if (completed || g_current_fragment >= g_total_fragments)
            {
                tlm_event("mestre", "msg_done");
                g_msg_id++;
                g_current_fragment = 0;
            }
            tlm_printf("mestre", "time_sync", "ch=%u", ch0);
        }
        else
        {
            g_hops_since_ack++;
            if (g_hops_since_ack >= LOSS_HOPS)
                reset_to_pairing();
        }
        return;
    }

    uint32_t slot = current_slot(millis(), g_hop_start_at);
    g_hop_counter = slot;

    maybe_enqueue_control();

    uint8_t channel = g_fixed_channel_mode ? TEST_FIXED_CHANNEL : get_next_active_channel(g_seed, slot, g_channel_mask);
    g_current_channel = channel;
    mestre_radio.setChannel(channel);
    delayMicroseconds(500);   // settle do PLL (500us, achado de bancada)
    mestre_radio.flush_rx();  // descarta ack velho retido na FIFO do canal anterior

    // fim absoluto deste slot e orçamento de transmissão (até SLOT_GUARD_MS antes
    // da fronteira, pra o envio não cruzar pro próximo slot).
    unsigned long slot_end = g_hop_start_at + (uint32_t)(slot + 1) * SLOT_MS;
    long tx_budget = (long)(slot_end - SLOT_GUARD_MS) - (long)millis();
    if (tx_budget <= 0)
    {
        // entrou tarde no slot (raro -- outro trabalho no loop() atrasou); não
        // penaliza o canal, só espera a fronteira e sai.
        while ((long)(slot_end - millis()) > 0)
            ;
        return;
    }

    unsigned long attempt_started_at = millis();
    uint8_t idx = find_channel_index(channel);
    bool acked_any = false;
    bool completed = false;

    // controle pendente tem prioridade: vai NO LUGAR do fragmento neste slot,
    // com a espera de ack limitada ao que resta do slot.
    bool control_acked = false;
    if (send_pending_control_if_any(control_acked, (uint32_t)tx_budget))
    {
        if (control_acked)
        {
            g_hops_since_ack = 0;
            acked_any = true;
        }
    }
    else
    {
        // transmite o fragmento corrente até o fim do slot ou até o ack; no ack
        // avança o DADO e segue com o próximo fragmento no MESMO slot (pipeline)
        // -- o canal (slot) só muda na fronteira de tempo.
        while ((long)(slot_end - SLOT_GUARD_MS) - (long)millis() > 0)
        {
            FragmentPacket pkt;
            fill_fragment(pkt, g_current_fragment);
            // slot-phase-servo: fase medida o mais perto possivel do envio
            pkt.slot_phase_ms = phase_in_slot_ms(millis(), g_hop_start_at, slot);
            mestre_radio.stopListening();
            bool sent = radio_send(mestre_radio, &pkt, sizeof(pkt));
            mestre_radio.startListening();
            if (!sent)
                break;

            long remaining = (long)(slot_end - SLOT_GUARD_MS) - (long)millis();
            if (remaining <= 0)
                break;
            uint32_t w = (uint32_t)remaining;
            if (w > ATTEMPT_ACK_WAIT_MS)
                w = ATTEMPT_ACK_WAIT_MS;
            bool ack_ok = wait_fragment_ack(g_current_fragment, completed, w);
            if (!ack_ok)
                continue; // sem ack nesta tentativa: tenta de novo enquanto houver slot

            // --- ack recebido ---
            acked_any = true;
            g_hops_since_ack = 0;
            if (g_channel_fail[idx] > 0)
                g_channel_fail[idx]--;
            // jamming-mitigation-hardening: o escravo reporta o canal jammed no
            // ack -- força-o ao limiar de ban (caminho autoritativo).
            if (g_rx_suspect_idx < TABLE_SIZE && (g_channel_mask & (1u << g_rx_suspect_idx)))
            {
                if (g_channel_fail[g_rx_suspect_idx] < CHANNEL_FAIL_THRESHOLD)
                    g_channel_fail[g_rx_suspect_idx] = CHANNEL_FAIL_THRESHOLD;
            }
            g_rx_suspect_idx = SUSPECT_NONE;
            if (!g_pair_data_confirmed)
            {
                g_pair_data_confirmed = true;
                note_candidate_result(true);
            }
            note_hop_for_throughput(true);
            uint32_t latency_ms = (uint32_t)(millis() - attempt_started_at);
            tlm_printf("mestre", "ack", "ch=%u;frag=%u;ms=%lu", channel, g_current_fragment, latency_ms);
            g_current_fragment++;
            if (completed || g_current_fragment >= g_total_fragments)
            {
                tlm_event("mestre", "msg_done");
                g_msg_id++;
                g_current_fragment = 0;
            }
            attempt_started_at = millis(); // latência do próximo fragmento (pipeline)
        }
    }

    // --- contabiliza o slot (uma vez, na saída) ---
    if (g_mestre_chq_source)
        chq_note_slot(acked_any);
    if (acked_any)
    {
        tlm_printf("mestre", "hop", "ch=%u;n=%lu;frag=%u", channel, g_hop_counter, g_current_fragment);
    }
    else
    {
        g_hops_since_ack++;
        // atribui falha só se a perda for ISOLADA (jamming-mitigation-hardening):
        // com salto por tempo o escravo nunca estaciona, então uma visita ao
        // canal jammed é um miss isolado -- o gate mantém canais inocentes fora.
        if (g_hops_since_ack <= ATTRIB_MAX_HOPS_SINCE_ACK && g_channel_fail[idx] < 255)
            g_channel_fail[idx]++;
        note_hop_for_throughput(false);
        tlm_printf("mestre", "miss", "ch=%u;n=%lu;frag=%u", channel, g_hop_counter, g_current_fragment);
    }

    apply_scheduled_changes();
    if (g_hops_since_ack >= LOSS_HOPS)
    {
        reset_to_pairing();
        return;
    }

    // espera a fronteira do slot pra a próxima chamada cair no próximo slot (o
    // escravo avança pela mesma fronteira de tempo -- lockstep sem relógio
    // compartilhado). Spin curto: no máximo ~SLOT_GUARD_MS.
    while ((long)(slot_end - millis()) > 0)
        ;
}

#endif // MESTRE_NODE_H
