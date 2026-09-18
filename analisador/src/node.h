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
 * analisador_set_alert_sink(). Ver docs/pinout.md.
 *
 * Output (115200 baud, sem cabecalho):
 *   <ch0_%>,<ch1_%>,...,<ch125_%>
 */

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include "../../shared/hopping.h"
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

#if FAST_BT_SCAN
#define SCAN_MIN BT_CH_MIN
#define SCAN_MAX BT_CH_MAX
#else
#define SCAN_MIN 0
#define SCAN_MAX (NUM_CHANNELS - 1)
#endif

// Limiares da heuristica de classificacao (spectrum-attack-classification).
// Valores iniciais a calibrar em bancada (tasks 3.1-3.3 do change).
static const int LIMIAR_ALVO = 70;          // alvo_avg minimo (%) p/ candidato
static const int LIMIAR_CONCENTRACAO = 30;  // alvo_avg - fora_avg minimo (p.p.)
static const uint8_t ATTACK_STREAK_N = 3;   // ciclos candidatos consecutivos

static const int NUM_CHANNELS = 126;
static const int SCAN_SAMPLES = 10;
static const int PLL_DWELL_US = 130;

// Achado de bancada (2026-09-18, validacao end-to-end com jammer real):
// um jammer sequencial de radio unico so ocupa UM canal por vez, e cada
// ciclo dele (~1ms/canal em sweep ou targeted, dominado pelo vTaskDelay(1)
// da task) e muito mais rapido que um sweep completo do analisador
// (~126 canais * ~1.4ms = ~176ms). Isso faz o jammer "passar" por cada
// canal-alvo so ocasionalmente durante a janela de 1.4ms em que o
// analisador amostra ESSE canal especifico -- medido em bancada: so ~1
// dos 16 canais de HOPPING_CHANNELS aparecia com atividade em cada
// scanSpectrum() individual, os outros 15 zerados. Com alvo_avg somando
// os 16 de UM UNICO sweep (classifyAttack() original), isso nunca
// ultrapassava ~6% de media -- LIMIAR_ALVO=70% nunca disparava, mesmo com
// o jammer realmente ativo e mirando exatamente esses canais (targeted).
//
// Fix: channelHeat[] acumula um "calor" por canal que sobe pra 100 a
// cada hit real e decai devagar entre sweeps (HEAT_DECAY_PER_CYCLE por
// ciclo) em vez de resetar a cada scanSpectrum(). Assim, hits raros e
// espalhados ao longo de varios sweeps em canais DIFERENTES ainda somam
// uma media alta quando avaliados juntos, porque o calor de um hit
// antigo ainda nao decaiu totalmente quando um canal diferente e
// atingido. classifyAttack() agora usa channelHeat[], nao channelData[]
// bruto -- channelData[] continua intacto (leitura instantanea) porque
// e o que sendCSV() manda pro heatmap do tools/spectrum/.
static const int HEAT_HIT_THRESHOLD = 20;      // channelData[] acima disso conta como hit
static const int HEAT_ON_HIT = 100;            // calor atribuido a um hit
// Decaimento lento de propósito: medido em bancada que um jammer
// sequencial de rádio único (targeted, 16 canais) só re-visita um canal
// específico a cada ~15-20s nesta cadência -- com decaimento rápido
// (ensaiado em 5/ciclo, ~17s pra zerar) o calor de um canal já sumia
// antes do jammer sequer terminar de passar pelos outros 15, e a media
// dos 16 nunca chegava perto de LIMIAR_ALVO. 1/ciclo (~150s pra zerar,
// SCAN_SAMPLES*NUM_CHANNELS ciclos de ~... ver medição real em bancada)
// dá margem suficiente pra cobrir uma rodada completa dos 16 canais.
static const int HEAT_DECAY_PER_CYCLE = 1;

static SPIClass analisador_hspi(HSPI);
static RF24 analisador_radio(PIN_CE, PIN_CSN);
static int channelData[NUM_CHANNELS];
static int channelHeat[NUM_CHANNELS];

static uint8_t g_attack_streak = 0;
static bool g_attack_active = false;

// Sink alternativo do alerta de ataque (two-board-topology): quando
// registrado (Placa A combinada, mestre+analisador na mesma placa), grava
// numa flag compartilhada em vez do GPIO físico. Default (nullptr) preserva
// o comportamento standalone via digitalWrite(PIN_ALERT, ...).
static void (*g_analisador_alert_sink)(bool) = nullptr;

static void analisador_set_alert_sink(void (*sink)(bool))
{
    g_analisador_alert_sink = sink;
}

static void setAlertPin(bool active)
{
    if (g_analisador_alert_sink)
        g_analisador_alert_sink(active);
    else
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

static inline void updateHeat(int ch)
{
    if (channelData[ch] > HEAT_HIT_THRESHOLD)
        channelHeat[ch] = HEAT_ON_HIT;
    else if (channelHeat[ch] > 0)
        channelHeat[ch] = (channelHeat[ch] > HEAT_DECAY_PER_CYCLE) ? channelHeat[ch] - HEAT_DECAY_PER_CYCLE : 0;
}

static void scanSpectrum()
{
#if FAST_BT_SCAN
    for (int i = 0; i < NUM_CHANNELS; i++)
        channelData[i] = 0;

    for (int ch = BT_CH_MIN; ch <= BT_CH_MAX; ch++)
    {
        channelData[ch] = measureChannel((uint8_t)ch);
        updateHeat(ch);
    }
#else
    for (int ch = 0; ch < NUM_CHANNELS; ch++)
    {
        channelData[ch] = measureChannel((uint8_t)ch);
        updateHeat(ch);
    }
#endif
}

static bool isHoppingChannel(uint8_t ch)
{
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
    {
        if (HOPPING_CHANNELS[i] == ch)
            return true;
    }
    return false;
}

// Compara a atividade media nos 16 canais de HOPPING_CHANNELS (alvo_avg)
// contra o restante do espectro efetivamente varrido nesse ciclo (fora_avg,
// respeitando FAST_BT_SCAN), exige ATTACK_STREAK_N ciclos candidatos
// consecutivos antes de declarar "ataque ativo", e reflete o estado via
// setAlertPin() (HIGH/flag enquanto ativo, LOW/flag assim que um ciclo
// deixa de ser candidato).
static void classifyAttack()
{
    long alvo_sum = 0;
    int alvo_n = 0;
    long fora_sum = 0;
    int fora_n = 0;

    for (int ch = SCAN_MIN; ch <= SCAN_MAX; ch++)
    {
        if (isHoppingChannel((uint8_t)ch))
        {
            alvo_sum += channelHeat[ch];
            alvo_n++;
        }
        else
        {
            fora_sum += channelHeat[ch];
            fora_n++;
        }
    }

    int alvo_avg = (alvo_n > 0) ? (int)(alvo_sum / alvo_n) : 0;
    int fora_avg = (fora_n > 0) ? (int)(fora_sum / fora_n) : 0;

    bool candidate = (alvo_avg >= LIMIAR_ALVO) && (alvo_avg - fora_avg >= LIMIAR_CONCENTRACAO);

    if (candidate)
    {
        g_attack_streak++;
        if (g_attack_streak >= ATTACK_STREAK_N && !g_attack_active)
        {
            g_attack_active = true;
            setAlertPin(true);
            guarded_printf("[ALERTA] Ataque direcionado detectado. alvo_avg=%d fora_avg=%d\n", alvo_avg, fora_avg);
            tlm_printf("analisador", "alert_on", "alvo_avg=%d;fora_avg=%d", alvo_avg, fora_avg);
        }
    }
    else
    {
        g_attack_streak = 0;
        if (g_attack_active)
        {
            g_attack_active = false;
            setAlertPin(false);
            guarded_println("[ALERTA] Ataque encerrado.");
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

    bool combined_mode = (g_analisador_alert_sink != nullptr);

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
    classifyAttack();
    sendCSV();
}

#endif // ANALISADOR_NODE_H
