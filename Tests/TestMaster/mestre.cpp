#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <printf.h> // Necessário para o printDetails()
#include "../../shared/hopping.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// -------------------------------------------------------------
// FASE 1: O Mestre (Transmissor do Beacon)
// -------------------------------------------------------------

// CE:   GPIO 21
// CSN:  GPIO 22
// SCK:  GPIO 18
// MOSI: GPIO 23
// MISO: GPIO 19
// VCC:  3.3V
// GND:  GND

// Pinos do SPI para o ESP32 (Forçando VSPI)
#define PIN_CE 22
#define PIN_CSN 21

#define PIN_SCK 18
#define PIN_MISO 19
#define PIN_MOSI 23

RF24 radio(PIN_CE, PIN_CSN);

const byte masterAddress[6] = "MSTR1"; // Endereço deste dispositivo (mestre)
const byte slaveAddress[6] = "SLV01";  // Endereço do escravo

// ---- ESTRUTURAS DE COMUNICAÇÃO ----
enum MessageType : uint8_t
{
    MSG_DATA = 1,
    MSG_ACK1 = 2,
    MSG_CONFIRM = 3,
    MSG_FINAL = 4
};

const uint8_t TEXT_SIZE = 25; // 24 chars + '\0', mantendo o payload total em 32 bytes

struct __attribute__((packed)) DataMessage
{
    uint8_t type;         // MSG_DATA
    uint32_t id_mestre;   // Código do Mestre
    uint16_t seq;         // Número de sequência da mensagem
    char text[TEXT_SIZE]; // Mensagem "maior"
};

static_assert(sizeof(DataMessage) <= 32, "DataMessage excede 32 bytes do NRF24");

struct SmallAck
{
    uint8_t type;        // MSG_ACK1 ou MSG_FINAL
    uint16_t seq;        // Número de sequência que está sendo reconhecido
    uint32_t id_escravo; // Código do Escravo
};

bool communication_finished = false;
unsigned long last_send_time = 0;
uint16_t seq_counter = 1;

void setup()
{
    Serial.begin(115200);
    delay(1000); // Aguarda o monitor serial abrir
    Serial.println("\n[TESTE] Iniciando o Blink básico...");
    pinMode(LED_BUILTIN, OUTPUT);

    Serial.println("\n[MESTRE] Iniciando transmissor (FIXO)...");

    delay(2000);

    // Configurar lib de Debug para vermos as entranhas da antena
    printf_begin();

    // Desliga explicitamente o Chip Select (CSN) antes de iniciar o barramento SPI
    pinMode(PIN_CSN, OUTPUT);
    digitalWrite(PIN_CSN, HIGH);
    delay(10);

    // Forçamos os pinos exatos do ESP32 para não depender do "default" da placa genérica
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);

    // Inicializa o rádio
    if (!radio.begin())
    {
        Serial.println(F("[ERRO MESTRE] Falha fatal! O Módulo NRF24 não respondeu no SPI. Verifique a fiação."));
    }

    // Configurações base do rádio - Canal fixo 125
    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_1MBPS);
    radio.setChannel(125);

    // Habilita Dynamic Payloads e envio de payload no ACK
    radio.enableDynamicPayloads();
    radio.enableAckPayload();

    radio.setCRCLength(RF24_CRC_16);
    radio.openWritingPipe(slaveAddress);
    radio.openReadingPipe(1, masterAddress); // permite receber respostas do escravo

    // Limpa a memória de pacotes não enviados que ficaram presos antes do mestre reiniciar
    radio.flush_tx();

    // O rádio envia e o hardware automaticamente espera o ACK
    radio.setAutoAck(true);

    radio.stopListening(); // Modo de Transmissão (inicialmente)

    Serial.println("\n--- STATUS DO RÁDIO MESTRE (REGISTRADORES) ---");
    radio.printDetails();
    Serial.println("----------------------------------------------\n");

    Serial.println("[MESTRE] Rádio configurado no Canal 125.");
}

void loop()
{
    // ---- BLINK NÃO-BLOQUEANTE (HEARTBEAT) ----
    static unsigned long last_blink = 0;
    if (millis() - last_blink >= 500)
    {
        last_blink = millis();
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }

    // Se já finalizamos a comunicação, não fazemos mais nada
    if (communication_finished)
        return;

    // Tentativa periódica de enviar a mensagem e realizar handshake 3-vias
    if (millis() - last_send_time >= 1500)
    {
        last_send_time = millis();

        // Monta a mensagem maior
        DataMessage msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_DATA;
        msg.id_mestre = 0x1A2B3C4D;
        msg.seq = seq_counter;
        const char *text = "Mensagem maior do Mestre";
        strncpy(msg.text, text, TEXT_SIZE - 1);

        // Envia (modo transmissão)
        radio.stopListening();
        bool sent = radio.write(&msg, sizeof(msg));
        if (!sent)
        {
            Serial.println("[MESTRE] Falha no envio do DataMessage.");
            radio.startListening();
            return;
        }

        Serial.printf("[MESTRE] DataMessage enviado (seq=%d). Aguardando ACK1...\n", msg.seq);

        // Espera por ACK1 do escravo
        radio.startListening();
        unsigned long start_wait = millis();
        bool ack1_received = false;
        while (millis() - start_wait < 1000)
        {
            if (radio.available())
            {
                SmallAck ack;
                uint8_t pipe;
                uint8_t size = radio.getDynamicPayloadSize();
                if (size == sizeof(ack))
                {
                    radio.read(&ack, sizeof(ack));
                    if (ack.type == MSG_ACK1 && ack.seq == msg.seq)
                    {
                        Serial.printf("[MESTRE] ACK1 recebido do Escravo (id=0x%X, seq=%d)\n", ack.id_escravo, ack.seq);
                        ack1_received = true;
                        break;
                    }
                }
                else
                {
                    // descarta
                    uint8_t dump[32];
                    radio.read(dump, size);
                }
            }
        }

        if (!ack1_received)
        {
            Serial.println("[MESTRE] Timeout aguardando ACK1. Irei tentar novamente depois.");
            return;
        }

        // Envia confirmação final (MSG_CONFIRM)
        radio.stopListening();
        SmallAck confirm;
        confirm.type = MSG_CONFIRM;
        confirm.seq = msg.seq;
        confirm.id_escravo = 0; // não usamos aqui, apenas um confirm
        bool sent_confirm = radio.write(&confirm, sizeof(confirm));
        if (!sent_confirm)
        {
            Serial.println("[MESTRE] Falha ao enviar CONFIRM.");
            radio.startListening();
            return;
        }
        Serial.println("[MESTRE] CONFIRM enviado. Aguardando ACK final (3/3)...");

        // Aguarda ACK final do escravo
        radio.startListening();
        start_wait = millis();
        bool final_received = false;
        while (millis() - start_wait < 1000)
        {
            if (radio.available())
            {
                SmallAck ack2;
                uint8_t size = radio.getDynamicPayloadSize();
                if (size == sizeof(ack2))
                {
                    radio.read(&ack2, sizeof(ack2));
                    if (ack2.type == MSG_FINAL && ack2.seq == msg.seq)
                    {
                        Serial.printf("[MESTRE] ACK final recebido (seq=%d). Comunicação encerrada com sucesso.\n", ack2.seq);
                        final_received = true;
                        break;
                    }
                }
                else
                {
                    uint8_t dump[32];
                    radio.read(dump, size);
                }
            }
        }

        if (final_received)
        {
            communication_finished = true;
        }
        else
        {
            Serial.println("[MESTRE] Timeout aguardando ACK final. Reinicie o handshake.");
        }

        // incrementa sequência para próxima tentativa
        seq_counter++;
    }
}
