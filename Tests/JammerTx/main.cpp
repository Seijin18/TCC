/*
 * JammerTx probe (jammer-emission-hardware, tarefa 1): testa DIRETAMENTE o
 * transmissor do modulo do jammer, isolando-o de todo o resto.
 *
 * Na Placa B (ver docs/pinout.md): escravo = VSPI (CE22), jammer = HSPI (CE16).
 * Aqui invertemos os papeis em relacao ao env:validacao (que usava VSPI como
 * TX): o TX e o modulo do JAMMER (HSPI) e o detector e o modulo do ESCRAVO
 * (VSPI, sabidamente bom em RX pois o enlace funciona). Os dois ficam a
 * centimetros na mesma placa -> deteccao por RPD/testCarrier e o teste mais
 * sensivel possivel de "o modulo do jammer emite alguma coisa?".
 *
 * Nenhuma logica de pareamento/hopping. Comandos via serial (logs/cmd.in):
 *   cw on [ch]     - portadora constante (startConstCarrier) PA_MAX no canal
 *   cw off
 *   flood on [ch]  - inundacao de payload (writeFast) continua no canal
 *   flood off
 *   status
 * Saida: uma linha por ~1s comecando com "JAMTX" (capturada pelo logger no
 * jsonl da sessao, campo raw), com o hit% do canal de TX e o pico da banda.
 */
#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>

// Modulo do JAMMER na Placa B (HSPI) -> usado como TRANSMISSOR aqui
#define PIN_CE_TX 16
#define PIN_CSN_TX 15
#define PIN_SCK_TX 4
#define PIN_MISO_TX 2
#define PIN_MOSI_TX 17

// Modulo do ESCRAVO na Placa B (VSPI) -> usado como DETECTOR (RPD) aqui
#define PIN_CE_RX 22
#define PIN_CSN_RX 21
#define PIN_SCK_RX 18
#define PIN_MISO_RX 19
#define PIN_MOSI_RX 23

#define NUM_CH 126
#define SCAN_SAMPLES 10
#define SETTLE_US 130

SPIClass spiTx(HSPI);
SPIClass spiRx(VSPI);
RF24 tx(PIN_CE_TX, PIN_CSN_TX);
RF24 rx(PIN_CE_RX, PIN_CSN_RX);

static int chData[NUM_CH];
static uint8_t txCh = 125;
static bool cwOn = false;
static bool floodOn = false;
static const uint8_t floodPayload[32] = {0xAA};

#define CMD_BUF 32
static char cmdBuf[CMD_BUF];
static uint8_t cmdLen = 0;

static void configTx()
{
    tx.setAutoAck(false);
    tx.setRetries(0, 0);
    tx.disableCRC();
    tx.setDataRate(RF24_2MBPS);
    tx.setPALevel(RF24_PA_MAX);
    tx.stopListening();
}

static void configRx()
{
    rx.setAutoAck(false);
    rx.stopListening();
    rx.setPALevel(RF24_PA_MIN);
    rx.setDataRate(RF24_1MBPS);
}

static void stopTx()
{
    if (cwOn)
    {
        tx.stopConstCarrier();
        cwOn = false;
    }
    floodOn = false;
    delay(5);
}

static void startCw(uint8_t ch)
{
    stopTx();
    tx.powerUp();
    delay(5);
    configTx();
    txCh = ch;
    tx.startConstCarrier(RF24_PA_MAX, ch);
    cwOn = true;
    Serial.printf("JAMTX cw ON ch=%u pvariant=%d\n", ch, (int)tx.isPVariant());
}

static void startFlood(uint8_t ch)
{
    stopTx();
    tx.powerUp();
    delay(5);
    configTx();
    txCh = ch;
    tx.setChannel(ch);
    // endereco arbitrario fixo; sem ACK
    uint8_t addr[5] = {0xE7, 0xE7, 0xE7, 0xE7, 0xE7};
    tx.openWritingPipe(addr);
    tx.stopListening();
    floodOn = true;
    Serial.printf("JAMTX flood ON ch=%u pvariant=%d\n", ch, (int)tx.isPVariant());
}

static int measureCh(uint8_t ch)
{
    rx.setChannel(ch);
    delayMicroseconds(SETTLE_US);
    int hits = 0;
    for (int s = 0; s < SCAN_SAMPLES; s++)
    {
        rx.startListening();
        delayMicroseconds(SETTLE_US);
        if (rx.testCarrier())
            hits++;
        rx.stopListening();
    }
    return (hits * 100) / SCAN_SAMPLES;
}

static void handleCmd(char *l)
{
    while (*l == ' ' || *l == '\t')
        l++;
    for (char *p = l; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p = *p - 'A' + 'a';

    if (strncmp(l, "cw on", 5) == 0)
    {
        int ch = (l[5]) ? atoi(l + 5) : txCh;
        if (ch < 0 || ch > 125)
            ch = txCh;
        startCw((uint8_t)ch);
    }
    else if (strcmp(l, "cw off") == 0)
    {
        stopTx();
        Serial.println("JAMTX cw OFF");
    }
    else if (strncmp(l, "flood on", 8) == 0)
    {
        int ch = (l[8]) ? atoi(l + 8) : txCh;
        if (ch < 0 || ch > 125)
            ch = txCh;
        startFlood((uint8_t)ch);
    }
    else if (strcmp(l, "flood off") == 0)
    {
        stopTx();
        Serial.println("JAMTX flood OFF");
    }
    else if (strcmp(l, "status") == 0)
    {
        Serial.printf("JAMTX status cw=%d flood=%d ch=%u tx_conn=%d rx_conn=%d\n",
                      (int)cwOn, (int)floodOn, txCh,
                      (int)tx.isChipConnected(), (int)rx.isChipConnected());
    }
    else if (l[0])
        Serial.printf("JAMTX erro cmd=%s\n", l);
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
                handleCmd(cmdBuf);
                cmdLen = 0;
            }
            continue;
        }
        if (cmdLen < CMD_BUF - 1)
            cmdBuf[cmdLen++] = c;
        else
            cmdLen = 0;
    }
}

void setup()
{
    Serial.begin(115200);
    delay(2000);
    Serial.println("\nJAMTX probe: TX=modulo jammer(HSPI CE16), RX=modulo escravo(VSPI CE22)");

    spiTx.begin(PIN_SCK_TX, PIN_MISO_TX, PIN_MOSI_TX, PIN_CSN_TX);
    spiRx.begin(PIN_SCK_RX, PIN_MISO_RX, PIN_MOSI_RX, PIN_CSN_RX);

    bool txok = tx.begin(&spiTx, PIN_CE_TX, PIN_CSN_TX) && tx.isChipConnected();
    bool rxok = rx.begin(&spiRx, PIN_CE_RX, PIN_CSN_RX) && rx.isChipConnected();
    Serial.printf("JAMTX boot tx_conn=%d rx_conn=%d tx_pvariant=%d rx_pvariant=%d\n",
                  (int)txok, (int)rxok, (int)tx.isPVariant(), (int)rx.isPVariant());

    configTx();
    configRx();
    Serial.println("JAMTX pronto. cmds: cw on [ch] | cw off | flood on [ch] | flood off | status");
}

void loop()
{
    static unsigned long last = 0;
    static uint8_t scanCh = 0;
    static int maxCh = 0, maxHit = 0;
    static long sum = 0;

    processSerial();

    // Mantem a inundacao de payload viva (sem espera de ACK). CW nao precisa.
    if (floodOn)
    {
        tx.writeFast(floodPayload, sizeof(floodPayload));
        tx.txStandBy(0);
    }

    // Varre um canal por iteracao (nao bloqueia a inundacao por muito tempo)
    chData[scanCh] = measureCh(scanCh);
    if (chData[scanCh] > maxHit)
    {
        maxHit = chData[scanCh];
        maxCh = scanCh;
    }
    sum += chData[scanCh];
    scanCh++;

    if (scanCh >= NUM_CH)
    {
        scanCh = 0;
        // relatorio por varredura completa
        Serial.printf("JAMTX scan txon=%d ch=%u hit_txch=%d max_ch=%d max_hit=%d mean=%ld\n",
                      (int)(cwOn || floodOn), txCh, chData[txCh], maxCh, maxHit, sum / NUM_CH);
        maxCh = 0;
        maxHit = 0;
        sum = 0;
    }
    (void)last;
}
