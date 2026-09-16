/*
 * TCC Validacao — Jammer + Analisador no mesmo ESP32 (2 NRF24 / 2 antenas)
 *
 * Radio 1 (VSPI): JAMMER — CW hopping aleatorio ch 2–80 (comando serial)
 * Radio 2 (HSPI): ANALISADOR — espectro CSV
 *
 * Comandos: jammer on | jammer off | status
 */

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <cstring>
#include "../../shared/hopping.h"

#define PIN_CE_JAM 22
#define PIN_CSN_JAM 21
#define PIN_SCK_JAM 18
#define PIN_MISO_JAM 19
#define PIN_MOSI_JAM 23

#define PIN_CE_ANA 16
#define PIN_CSN_ANA 15
#define PIN_SCK_ANA 4
#define PIN_MISO_ANA 2
#define PIN_MOSI_ANA 17

#define BT_CH_MIN 2
#define BT_CH_MAX 80

#define NUM_CHANNELS 126
#define SCAN_SAMPLES 10
#define ANALYSOR_STEPS_PER_LOOP 10
#define PLL_SETTLE_US 130
#define JAMMER_HOP_DWELL_US 800
#define CMD_BUF_SIZE 32

SPIClass spiJam(VSPI);
SPIClass spiAna(HSPI);

RF24 jammer(PIN_CE_JAM, PIN_CSN_JAM);
RF24 analisador(PIN_CE_ANA, PIN_CSN_ANA);

int channelData[NUM_CHANNELS];

static uint8_t jamCh = BT_CH_MIN;
static uint8_t scanCh = BT_CH_MIN;
static uint8_t scanSample = 0;
static uint8_t scanHits = 0;
static bool scanActive = false;
static bool jammerEnabled = false;
static bool jammerCarrierRunning = false;

enum JamMode
{
    JAM_MODE_SWEEP,
    JAM_MODE_TARGETED
};

static JamMode g_mode = JAM_MODE_SWEEP;

static char cmdBuf[CMD_BUF_SIZE];
static uint8_t cmdLen = 0;

static void configureJammer(RF24 &r)
{
    r.setAutoAck(false);
    r.setRetries(0, 0);
    r.disableCRC();
    r.setDataRate(RF24_2MBPS);
    r.setPALevel(RF24_PA_MAX);
    r.stopListening();
}

static void configureAnalisador(RF24 &r)
{
    r.setAutoAck(false);
    r.stopListening();
    r.setPALevel(RF24_PA_MIN);
    r.setDataRate(RF24_1MBPS);
}

static uint8_t randomBtChannel()
{
    return (uint8_t)random(BT_CH_MIN, BT_CH_MAX + 1);
}

static uint8_t randomTargetedChannel()
{
    return HOPPING_CHANNELS[random(0, TABLE_SIZE)];
}

static void jammerStopCarrier()
{
    if (!jammerCarrierRunning)
        return;

    jammer.stopConstCarrier();
    jammerCarrierRunning = false;
    delay(5);
}

static bool jammerPrepareRadio()
{
    jammerStopCarrier();
    jammer.powerUp();
    delay(5);
    configureJammer(jammer);

    if (jammer.isChipConnected())
        return true;

    if (!jammer.begin(&spiJam, PIN_CE_JAM, PIN_CSN_JAM) || !jammer.isChipConnected())
    {
        Serial.println("[JAMMER] ERRO reiniciar radio VSPI");
        return false;
    }

    configureJammer(jammer);
    return true;
}

static bool jammerStartCarrier(uint8_t channel)
{
    if (!jammerPrepareRadio())
        return false;

    jammer.startConstCarrier(RF24_PA_MAX, channel);
    jammerCarrierRunning = true;
    jamCh = channel;
    return true;
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

static void beginScanPass()
{
    for (int i = 0; i < NUM_CHANNELS; i++)
        channelData[i] = 0;

    scanCh = BT_CH_MIN;
    scanSample = 0;
    scanHits = 0;
    scanActive = true;

    analisador.setChannel(scanCh);
    delayMicroseconds(PLL_SETTLE_US);
}

static void stepAnalisador()
{
    if (!scanActive)
        beginScanPass();

    analisador.startListening();
    delayMicroseconds(PLL_SETTLE_US);
    if (analisador.testCarrier())
        scanHits++;
    analisador.stopListening();
    scanSample++;

    if (scanSample < SCAN_SAMPLES)
        return;

    channelData[scanCh] = (scanHits * 100) / SCAN_SAMPLES;
    scanCh++;
    scanSample = 0;
    scanHits = 0;

    if (scanCh > BT_CH_MAX)
    {
        sendCSV();
        scanActive = false;
        return;
    }

    analisador.setChannel(scanCh);
    delayMicroseconds(PLL_SETTLE_US);
}

static void stepJammer()
{
    if (!jammerEnabled || !jammerCarrierRunning)
        return;

    jamCh = (g_mode == JAM_MODE_TARGETED) ? randomTargetedChannel() : randomBtChannel();
    jammer.setChannel(jamCh);
    delayMicroseconds(JAMMER_HOP_DWELL_US);
}

static void jammerStart()
{
    jammerEnabled = false;
    jammerStopCarrier();

    uint8_t ch = (g_mode == JAM_MODE_TARGETED) ? randomTargetedChannel() : randomBtChannel();
    if (!jammerStartCarrier(ch))
    {
        jammerEnabled = false;
        jammerCarrierRunning = false;
        return;
    }

    jammerEnabled = true;
    Serial.printf("[JAMMER] ON ch=%u (hop %u-%u)\n", ch, BT_CH_MIN, BT_CH_MAX);
    Serial.flush();
}

static void jammerStop()
{
    jammerEnabled = false;
    jammerStopCarrier();
    Serial.println("[JAMMER] OFF");
    Serial.flush();
}

static bool cmdMatches(const char *buf, const char *cmd)
{
    return strcasecmp(buf, cmd) == 0;
}

static void handleCommand(char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;

    for (char *p = line; *p; p++)
    {
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p - 'A' + 'a');
    }

    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t'))
        line[--len] = '\0';

    if (len == 0)
        return;

    if (cmdMatches(line, "jammer on") || cmdMatches(line, "j on"))
        jammerStart();
    else if (cmdMatches(line, "jammer off") || cmdMatches(line, "j off"))
        jammerStop();
    else if (cmdMatches(line, "mode sweep"))
    {
        g_mode = JAM_MODE_SWEEP;
        Serial.println("[MODE] sweep");
    }
    else if (cmdMatches(line, "mode targeted"))
    {
        g_mode = JAM_MODE_TARGETED;
        Serial.println("[MODE] targeted");
    }
    else if (cmdMatches(line, "status"))
        Serial.printf("[STATUS] jammer=%s carrier=%s ch=%u mode=%s\n",
                      jammerEnabled ? "ON" : "OFF",
                      jammerCarrierRunning ? "RUN" : "STOP",
                      jamCh,
                      g_mode == JAM_MODE_TARGETED ? "targeted" : "sweep");
    else
        Serial.printf("[ERRO] comando desconhecido: %s\n", line);
}

static void processSerial()
{
    while (Serial.available())
    {
        char c = (char)Serial.read();

        if (c == '\n' || c == '\r')
        {
            if (cmdLen > 0)
            {
                cmdBuf[cmdLen] = '\0';
                handleCommand(cmdBuf);
                cmdLen = 0;
            }
            continue;
        }

        if (cmdLen < CMD_BUF_SIZE - 1)
            cmdBuf[cmdLen++] = c;
        else
            cmdLen = 0;
    }
}

void setup()
{
    Serial.begin(115200);
    delay(2000);
    randomSeed(esp_random());

    Serial.println("\n=== TCC Validacao — Jammer + Analisador ===");
    Serial.printf("Jammer: hop aleatorio ch %u-%u | Comandos: jammer on/off\n", BT_CH_MIN, BT_CH_MAX);
    Serial.println("Jammer inicia DESLIGADO\n");

    spiJam.begin(PIN_SCK_JAM, PIN_MISO_JAM, PIN_MOSI_JAM, PIN_CSN_JAM);
    spiAna.begin(PIN_SCK_ANA, PIN_MISO_ANA, PIN_MOSI_ANA, PIN_CSN_ANA);

    if (!jammer.begin(&spiJam, PIN_CE_JAM, PIN_CSN_JAM) || !jammer.isChipConnected())
    {
        Serial.println("[ERRO] Radio jammer (VSPI) nao detectado.");
        while (true)
            delay(1000);
    }

    if (!analisador.begin(&spiAna, PIN_CE_ANA, PIN_CSN_ANA) || !analisador.isChipConnected())
    {
        Serial.println("[ERRO] Radio analisador (HSPI) nao detectado.");
        while (true)
            delay(1000);
    }

    configureJammer(jammer);
    configureAnalisador(analisador);
    Serial.println("[OK] Radios prontos.\n");

    processSerial();
}

void loop()
{
    processSerial();
    stepJammer();
    for (int i = 0; i < ANALYSOR_STEPS_PER_LOOP; i++)
        stepAnalisador();
}
