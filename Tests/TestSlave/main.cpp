#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <printf.h>
#include "../../shared/hopping.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// -------------------------------------------------------------
// FASE 1: O Escravo (Receptor do Beacon e Cálculo de QoS)
// -------------------------------------------------------------

// Pinos conforme o código de exemplo NRF24L01_LED_RX
// CE:   GPIO 22
// CSN:  GPIO 21
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

const byte masterAddress[6] = "MSTR1"; // Endereço do mestre
const byte myAddress[6] = "SLV01";     // Endereço deste escravo

enum MessageType : uint8_t
{
    MSG_DATA = 1,
    MSG_ACK1 = 2,
    MSG_CONFIRM = 3,
    MSG_FINAL = 4
};
const uint8_t TEXT_SIZE = 25;

struct __attribute__((packed)) DataMessage
{
    uint8_t type;
    uint32_t id_mestre;
    uint16_t seq;
    char text[TEXT_SIZE];
};

static_assert(sizeof(DataMessage) <= 32, "DataMessage excede 32 bytes do NRF24");

struct SmallAck
{
    uint8_t type; // MSG_ACK1 ou MSG_FINAL
    uint16_t seq;
    uint32_t id_escravo;
};

void setup()
{
    Serial.begin(115200);
    delay(1000); // Aguarda o monitor serial abrir
    Serial.println("\n[TESTE] Iniciando o Blink básico...");
    pinMode(LED_BUILTIN, OUTPUT);

    Serial.println("\n[ESCRAVO] Iniciando receptor (FIXO)...");

    // Configurar lib de Debug caso o Begin() da antena morra
    printf_begin();

    // Dá um tempo para a fonte de alimentação do NRF24L01 estabilizar antes de tocar no SPI
    delay(2000);

    // FIX: Desliga explicitamente o Chip Select (CSN) antes de iniciar o barramento SPI
    pinMode(PIN_CSN, OUTPUT);
    digitalWrite(PIN_CSN, HIGH);
    delay(10);

    // Iniciamos o barramento forçando os pinos do VSPI pra placa não se confundir
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);

    // Inicializa o rádio
    if (!radio.begin())
    {
        Serial.println(F("[ERRO ESCRAVO] Falha fatal! O Módulo NRF24 não respondeu no SPI. Verifique a fiação."));
    }

    // Configurações base para bater com o transmissor
    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_1MBPS);
    radio.setChannel(125);

    // Obrigatório para AckPayload funcionar, precisamos de tamanhos dinâmicos habilitados
    radio.enableDynamicPayloads();
    radio.enableAckPayload();

    radio.setCRCLength(RF24_CRC_16);

    // Abre APENAS o pipe 1. Fecha o Pipe 0, que é usado primariamente por transmissores.
    // Isso evita captar lixo do ar que pinga no pipe padrão da biblioteca (gerando Pipe 0 ou 7).
    radio.closeReadingPipe(0);

    // Limpa qualquer lixo que tenha ficado preso na memória da antena de testes anteriores
    radio.flush_rx();

    // Auto ACK para ele responder o transmissor (Habilitado por default, mas é bom reforçar)
    radio.setAutoAck(true);

    // Abre pipe de leitura para receber do mestre e configura pipe de escrita para responder
    radio.openReadingPipe(1, myAddress); // Inicia modo de escuta
    radio.openWritingPipe(masterAddress);
    radio.startListening();

    // Limpa a memória pra garantir inicialização fria limpa
    radio.flush_rx();
    radio.flush_tx();

    Serial.println("\n--- STATUS DO RÁDIO ESCRAVO (REGISTRADORES) ---");
    radio.printDetails();
    Serial.println("-----------------------------------------------\n");

    Serial.println("[ESCRAVO] Escutando no Canal 125 (Esperando mensagens para dar ACK customizado)...");
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

    // Se recebeu um dado no canal fixo
    uint8_t current_pipe;
    if (radio.available(&current_pipe))
    {
        // O Pipe 1 é o nosso Pipe oficial
        if (current_pipe == 1)
        {
            uint8_t dt_size = radio.getDynamicPayloadSize();
            if (dt_size == sizeof(DataMessage))
            {
                DataMessage msg;
                radio.read(&msg, sizeof(msg));

                if (msg.type == MSG_DATA && msg.id_mestre == 0x1A2B3C4D)
                {
                    Serial.printf("[ESCRAVO] DataMessage recebido (seq=%d): %s\n", msg.seq, msg.text);

                    // Envia ACK1 ao mestre
                    SmallAck ack1;
                    ack1.type = MSG_ACK1;
                    ack1.seq = msg.seq;
                    ack1.id_escravo = 0xD4C3B2A1;

                    radio.stopListening();
                    bool wrote = radio.write(&ack1, sizeof(ack1));
                    if (wrote)
                        Serial.printf("[ESCRAVO] ACK1 enviado (seq=%d)\n", ack1.seq);
                    else
                        Serial.println("[ESCRAVO] Falha ao enviar ACK1");

                    // Volta a escutar aguardando o CONFIRM do mestre
                    radio.startListening();
                    unsigned long start_wait = millis();
                    bool confirm_received = false;
                    while (millis() - start_wait < 1000)
                    {
                        if (radio.available())
                        {
                            uint8_t size = radio.getDynamicPayloadSize();
                            if (size == sizeof(SmallAck))
                            {
                                SmallAck r;
                                radio.read(&r, sizeof(r));
                                if (r.type == MSG_CONFIRM && r.seq == msg.seq)
                                {
                                    confirm_received = true;
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

                    if (confirm_received)
                    {
                        // Envia ACK final (3/3)
                        SmallAck final_ack;
                        final_ack.type = MSG_FINAL;
                        final_ack.seq = msg.seq;
                        final_ack.id_escravo = 0xD4C3B2A1;

                        radio.stopListening();
                        bool wrote2 = radio.write(&final_ack, sizeof(final_ack));
                        if (wrote2)
                            Serial.printf("[ESCRAVO] ACK final enviado (seq=%d). Comunicação encerrada.\n", final_ack.seq);
                        else
                            Serial.println("[ESCRAVO] Falha ao enviar ACK final");

                        radio.startListening();
                    }
                    else
                    {
                        Serial.println("[ESCRAVO] Timeout aguardando CONFIRM do Mestre.");
                    }
                }
                else
                {
                    Serial.println("[ESCRAVO] DataMessage ignorado: formato inválido ou id mismatch.");
                }
            }
            else
            {
                // descarta payloads inesperados
                uint8_t dump[32];
                radio.read(dump, dt_size);
                Serial.printf("[ESCRAVO] Payload inesperado recebido (%d bytes), descartado.\n", dt_size);
            }
        }
        else
        {
            // Descarta completamente pacotes que caírem nos pipes errados
            uint32_t dump_buffer;
            radio.read(&dump_buffer, sizeof(dump_buffer));
        }
    }
}
