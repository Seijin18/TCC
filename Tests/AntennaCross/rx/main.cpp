/*
 * Teste minimo de antena entre placas: este lado (RX) so escuta os
 * dois radios (VSPI e HSPI), cada um no seu canal fixo, e imprime
 * tudo que chegar -- contraparte de Tests/AntennaCross/tx.
 */

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>

#define PIN_CE_V 22
#define PIN_CSN_V 21
#define PIN_SCK_V 18
#define PIN_MISO_V 19
#define PIN_MOSI_V 23

#define PIN_CE_H 16
#define PIN_CSN_H 15
#define PIN_SCK_H 4
#define PIN_MISO_H 2
#define PIN_MOSI_H 17

#define CH_VSPI 76
#define CH_HSPI 90

static SPIClass vspi(VSPI);
static SPIClass hspi(HSPI);
static RF24 radioV(PIN_CE_V, PIN_CSN_V);
static RF24 radioH(PIN_CE_H, PIN_CSN_H);

static const uint8_t addr[6] = "PING1";

struct __attribute__((packed)) Msg
{
    uint32_t counter;
    char tag[5];
    uint8_t padding[21]; // DIAG: aproxima do tamanho do FragmentPacket real (30 bytes)
};

static void setupRadio(RF24 &r, SPIClass &spi, uint8_t ce, uint8_t csn, uint8_t ch)
{
    r.begin(&spi, ce, csn);
    r.setPALevel(RF24_PA_MAX);
    r.setDataRate(RF24_1MBPS);
    r.setChannel(ch);
    r.setAutoAck(true);
    r.setCRCLength(RF24_CRC_16);
    r.enableDynamicPayloads(); // DIAG: testando se DPL e o problema
    r.openReadingPipe(1, addr);
    r.startListening();
}

static bool okV = false;
static bool okH = false;
static uint32_t rxCountV = 0;
static uint32_t rxCountH = 0;

void setup()
{
    Serial.begin(115200);
    delay(1500);
    Serial.println("\n=== TESTE DE ANTENA: RX ===");

    vspi.begin(PIN_SCK_V, PIN_MISO_V, PIN_MOSI_V, PIN_CSN_V);
    hspi.begin(PIN_SCK_H, PIN_MISO_H, PIN_MOSI_H, PIN_CSN_H);

    okV = radioV.begin(&vspi, PIN_CE_V, PIN_CSN_V) && radioV.isChipConnected();
    if (!okV)
        Serial.println("[ERRO] radioV (VSPI) nao detectado no SPI");
    okH = radioH.begin(&hspi, PIN_CE_H, PIN_CSN_H) && radioH.isChipConnected();
    if (!okH)
        Serial.println("[ERRO] radioH (HSPI) nao detectado no SPI");

    // Sem essa guarda, available()/read() num radio nao inicializado
    // (chip ausente/nao detectado) pode retornar lixo da SPI e imprimir
    // "recebido" falso -- confundiu o diagnostico quando um dos modulos
    // foi removido pra teste cruzado.
    if (okV)
        setupRadio(radioV, vspi, PIN_CE_V, PIN_CSN_V, CH_VSPI);
    if (okH)
        setupRadio(radioH, hspi, PIN_CE_H, PIN_CSN_H, CH_HSPI);

    Serial.printf("VSPI RX em ch=%u | HSPI RX em ch=%u | escutando...\n", CH_VSPI, CH_HSPI);
}

void loop()
{
    if (okV && radioV.available())
    {
        Msg m;
        radioV.read(&m, sizeof(m));
        rxCountV++;
        Serial.printf("[RX-VSPI] n=%lu tag=%s total=%lu\n", m.counter, m.tag, rxCountV);
    }

    if (okH && radioH.available())
    {
        Msg m;
        radioH.read(&m, sizeof(m));
        rxCountH++;
        Serial.printf("[RX-HSPI] n=%lu tag=%s total=%lu\n", m.counter, m.tag, rxCountH);
    }

    static unsigned long lastStatus = 0;
    if (millis() - lastStatus > 3000)
    {
        lastStatus = millis();
        Serial.printf("[STATUS] total_recebido_VSPI=%lu total_recebido_HSPI=%lu\n", rxCountV, rxCountH);
    }
}
