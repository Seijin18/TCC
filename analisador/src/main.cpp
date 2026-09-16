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
 * ao GPIO 4 do mestre (entrada) + GND comum entre as duas placas. Ver
 * docs/pinout.md.
 *
 * Output (115200 baud, sem cabecalho):
 *   <ch0_%>,<ch1_%>,...,<ch125_%>
 */

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include "../../shared/hopping.h"

#define PIN_CE 22
#define PIN_CSN 21
#define PIN_SCK 18
#define PIN_MISO 19
#define PIN_MOSI 23

// Pino de alerta para o mestre (spectrum-attack-classification) — nivel
// HIGH enquanto "ataque ativo", LOW caso contrario. Ver docs/pinout.md.
#define PIN_ALERT 4

// 1 = varre apenas ch 2–80 (banda BT, refresh ~3x mais rapido)
// 0 = varredura completa 126 canais (2400–2525 MHz)
#define FAST_BT_SCAN 1

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

RF24 radio(PIN_CE, PIN_CSN);
int channelData[NUM_CHANNELS];

static uint8_t g_attack_streak = 0;
static bool g_attack_active = false;

static void configureReceiver(RF24 &r)
{
    r.setAutoAck(false);
    r.stopListening();
    r.setPALevel(RF24_PA_MIN);
    r.setDataRate(RF24_1MBPS);
}

static int measureChannel(uint8_t ch)
{
    radio.setChannel(ch);
    delayMicroseconds(PLL_DWELL_US);

    int hits = 0;
    for (int s = 0; s < SCAN_SAMPLES; s++)
    {
        radio.startListening();
        delayMicroseconds(PLL_DWELL_US);
        if (radio.testCarrier())
            hits++;
        radio.stopListening();
    }

    return (hits * 100) / SCAN_SAMPLES;
}

static void scanSpectrum()
{
#if FAST_BT_SCAN
    for (int i = 0; i < NUM_CHANNELS; i++)
        channelData[i] = 0;

    for (int ch = BT_CH_MIN; ch <= BT_CH_MAX; ch++)
        channelData[ch] = measureChannel((uint8_t)ch);
#else
    for (int ch = 0; ch < NUM_CHANNELS; ch++)
        channelData[ch] = measureChannel((uint8_t)ch);
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
// consecutivos antes de declarar "ataque ativo", e reflete o estado no
// PIN_ALERT (HIGH enquanto ativo, LOW assim que um ciclo deixa de ser
// candidato).
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
            alvo_sum += channelData[ch];
            alvo_n++;
        }
        else
        {
            fora_sum += channelData[ch];
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
            digitalWrite(PIN_ALERT, HIGH);
            Serial.printf("[ALERTA] Ataque direcionado detectado. alvo_avg=%d fora_avg=%d\n", alvo_avg, fora_avg);
        }
    }
    else
    {
        g_attack_streak = 0;
        if (g_attack_active)
        {
            g_attack_active = false;
            digitalWrite(PIN_ALERT, LOW);
            Serial.println("[ALERTA] Ataque encerrado.");
        }
    }
}

static void sendCSV()
{
    Serial.print(channelData[0]);
    for (int i = 1; i < NUM_CHANNELS; i++)
    {
        Serial.print(',');
        Serial.print(channelData[i]);
    }
    Serial.println();
}

void setup()
{
    Serial.begin(115200);
    delay(2000);

    Serial.println("\n=== TCC Analisador v1 — Espectro 2.4 GHz ===");

    pinMode(PIN_ALERT, OUTPUT);
    digitalWrite(PIN_ALERT, LOW);

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);

    bool radioOK = false;
    for (int attempt = 1; attempt <= 5; attempt++)
    {
        if (radio.begin())
        {
            radioOK = true;
            break;
        }
        Serial.printf("radio.begin() falhou, tentativa %d/5...\n", attempt);
        delay(500);
    }

    if (!radioOK || !radio.isChipConnected())
    {
        while (true)
        {
            Serial.println("[ERRO] NRF24L01 nao detectado. Verifique fiação e 3.3V.");
            delay(3000);
        }
    }

    configureReceiver(radio);

#if FAST_BT_SCAN
    Serial.printf("Modo: FAST_BT_SCAN (ch %d–%d, 2402–2480 MHz)\n", BT_CH_MIN, BT_CH_MAX);
#else
    Serial.println("Modo: FULL (ch 0–125, 2400–2525 MHz)");
#endif
    Serial.println("Enviando CSV: ch0,ch1,...,ch125 (atividade 0–100%)\n");
}

void loop()
{
    scanSpectrum();
    classifyAttack();
    sendCSV();
}
