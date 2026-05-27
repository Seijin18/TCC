#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <printf.h>

// Pinos Rádio 1 (VSPI)
#define PIN_CE_VSPI 22
#define PIN_CSN_VSPI 21
#define PIN_SCK_VSPI 18
#define PIN_MISO_VSPI 19
#define PIN_MOSI_VSPI 23

// Pinos Rádio 2 (HSPI)
#define PIN_CE_HSPI 16   // RX2
#define PIN_CSN_HSPI 15  // D15
#define PIN_SCK_HSPI 4   // D4
#define PIN_MISO_HSPI 2  // D2
#define PIN_MOSI_HSPI 17 // TX2

SPIClass vspi(VSPI);
SPIClass hspi(HSPI);

RF24 radio1(PIN_CE_VSPI, PIN_CSN_VSPI);
RF24 radio2(PIN_CE_HSPI, PIN_CSN_HSPI);

void setup()
{
    Serial.begin(115200);
    delay(2000);
    printf_begin(); // Necessário para o radio.printDetails() funcionar

    Serial.println("\n\n==================================================");
    Serial.println("--- TESTE DE HARDWARE: ANTENAS DUAL NRF24L01 ---");
    Serial.println("==================================================");

    // Inicia barramento SPI primário (VSPI)
    vspi.begin(PIN_SCK_VSPI, PIN_MISO_VSPI, PIN_MOSI_VSPI, PIN_CSN_VSPI);
    // Inicia barramento SPI secundário (HSPI)
    hspi.begin(PIN_SCK_HSPI, PIN_MISO_HSPI, PIN_MOSI_HSPI, PIN_CSN_HSPI);

    // --- TESTE RADIO 1 ---
    Serial.println("\n[1] Analisando Conexao SPI do Rádio 1 (VSPI)...");
    Serial.println("    Pinos: SCK=18, MISO=19, MOSI=23, CE=22, CSN=21");
    // Passando os 3 argumentos pro begin(SPIClass*, ce_pin, csn_pin)
    bool r1_begin = radio1.begin(&vspi, PIN_CE_VSPI, PIN_CSN_VSPI);
    bool r1_chip = radio1.isChipConnected();

    if (r1_begin && r1_chip)
    {
        Serial.println("    -> [SUCESSO] Rádio 1 COMUNICA PERFEITAMENTE!");
    }
    else
    {
        Serial.println("    -> [FALHA] Rádio 1 NÃO ESTÁ RESPONDENDO!");
        if (!r1_chip)
            Serial.println("       * O chip físico (isChipConnected) retornou FALSO. Verifique MISO/MOSI/CSN.");
    }

    Serial.println("\n-- Registros do Rádio 1 --");
    radio1.printDetails();

    // --- TESTE RADIO 2 ---
    Serial.println("\n\n[2] Analisando Conexao SPI do Rádio 2 (HSPI)...");
    Serial.println("    Pinos: SCK=4(D4), MISO=2(D2), MOSI=17(TX2), CE=16(RX2), CSN=15(D15)");

    bool r2_begin = radio2.begin(&hspi, PIN_CE_HSPI, PIN_CSN_HSPI);
    bool r2_chip = radio2.isChipConnected();

    if (r2_begin && r2_chip)
    {
        Serial.println("    -> [SUCESSO] Rádio 2 COMUNICA PERFEITAMENTE!");
    }
    else
    {
        Serial.println("    -> [FALHA] Rádio 2 NÃO ESTÁ RESPONDENDO!");
        if (!r2_chip)
            Serial.println("       * O chip físico (isChipConnected) retornou FALSO. Verifique fiação.");
    }

    Serial.println("\n-- Registros do Rádio 2 --");
    radio2.printDetails();

    Serial.println("\n==================================================");
    Serial.println("FIM DO TESTE.");

    if (!r1_chip || !r2_chip)
    {
        Serial.println("! DICA: Se as infos imprimirem tudo '0x00' ou '0xff',");
        Serial.println("        voce tem um problema de mau contato (Jumpers ou Protoboard),");
        Serial.println("        ou falta de energia (NRF24 exige 3.3V puro, não ligue no 5V).");
    }
}

void loop()
{
    // Fica ocioso
    delay(1000);
}
