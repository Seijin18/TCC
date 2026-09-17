/*
 * Teste minimo de antena entre placas: este lado (TX) manda uma
 * mensagem simples por segundo em CADA um dos dois radios (VSPI e
 * HSPI), cada um em um canal fixo diferente -- sem FHSS, sem
 * pareamento, sem nenhuma logica do projeto principal. So "manda e
 * imprime se deu certo".
 *
 * Objetivo: confirmar, isoladamente, se cada modulo desta placa
 * consegue transmitir algo que o modulo correspondente da OUTRA placa
 * (rodando Tests/AntennaCross/rx) recebe.
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

#define CH_VSPI 100
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
};

static bool safeSend(RF24 &r, const Msg &m)
{
    r.flush_tx();
    r.writeFast(&m, sizeof(m), true); // multicast=true (NO_ACK) -- nunca trava
    return r.txStandBy(50, true);
}

static void setupRadio(RF24 &r, SPIClass &spi, uint8_t ce, uint8_t csn, uint8_t ch)
{
    r.begin(&spi, ce, csn);
    r.setPALevel(RF24_PA_MAX);
    r.setDataRate(RF24_1MBPS);
    r.setChannel(ch);
    r.setAutoAck(false);
    r.enableDynamicAck();
    r.setCRCLength(RF24_CRC_16);
    r.setPayloadSize(sizeof(Msg));
    r.openWritingPipe(addr);
    r.stopListening();
}

static uint32_t counter = 0;

void setup()
{
    Serial.begin(115200);
    delay(1500);
    Serial.println("\n=== TESTE DE ANTENA: TX ===");

    vspi.begin(PIN_SCK_V, PIN_MISO_V, PIN_MOSI_V, PIN_CSN_V);
    hspi.begin(PIN_SCK_H, PIN_MISO_H, PIN_MOSI_H, PIN_CSN_H);

    if (!radioV.begin(&vspi, PIN_CE_V, PIN_CSN_V) || !radioV.isChipConnected())
        Serial.println("[ERRO] radioV (VSPI) nao detectado no SPI");
    if (!radioH.begin(&hspi, PIN_CE_H, PIN_CSN_H) || !radioH.isChipConnected())
        Serial.println("[ERRO] radioH (HSPI) nao detectado no SPI");

    setupRadio(radioV, vspi, PIN_CE_V, PIN_CSN_V, CH_VSPI);
    setupRadio(radioH, hspi, PIN_CE_H, PIN_CSN_H, CH_HSPI);

    Serial.printf("VSPI TX em ch=%u | HSPI TX em ch=%u | 1 msg/segundo em cada\n", CH_VSPI, CH_HSPI);
}

void loop()
{
    counter++;

    Msg mv;
    mv.counter = counter;
    memcpy(mv.tag, "VSPI", 5);
    bool okV = safeSend(radioV, mv);

    Msg mh;
    mh.counter = counter;
    memcpy(mh.tag, "HSPI", 5);
    bool okH = safeSend(radioH, mh);

    Serial.printf("[TX] n=%lu VSPI(ch=%u)=%s HSPI(ch=%u)=%s\n",
                  counter, CH_VSPI, okV ? "enviado" : "FALHOU", CH_HSPI, okH ? "enviado" : "FALHOU");

    delay(1000);
}
