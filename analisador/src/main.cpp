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
 * Output (115200 baud, sem cabecalho):
 *   <ch0_%>,<ch1_%>,...,<ch125_%>
 */

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>

#define PIN_CE 22
#define PIN_CSN 21
#define PIN_SCK 18
#define PIN_MISO 19
#define PIN_MOSI 23

// 1 = varre apenas ch 2–80 (banda BT, refresh ~3x mais rapido)
// 0 = varredura completa 126 canais (2400–2525 MHz)
#define FAST_BT_SCAN 1

#define BT_CH_MIN 2
#define BT_CH_MAX 80

static const int NUM_CHANNELS = 126;
static const int SCAN_SAMPLES = 10;
static const int PLL_DWELL_US = 130;

RF24 radio(PIN_CE, PIN_CSN);
int channelData[NUM_CHANNELS];

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
    sendCSV();
}
