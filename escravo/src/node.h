#ifndef ESCRAVO_NODE_H
#define ESCRAVO_NODE_H

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <printf.h>
#include <string.h>
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

static RF24 escravo_radio(PIN_CE, PIN_CSN);

static const byte masterAddress[6] = "MSTR1"; // Endereço do mestre
static const byte myAddress[6] = "SLV01";     // Endereço deste escravo

enum MessageType : uint8_t
{
    MSG_LINK_PROBE = 1,
    MSG_LINK_ACK = 2,
    MSG_FRAGMENT = 3,
    MSG_FRAGMENT_ACK = 4,
    MSG_SEED_INIT = 5,
    MSG_SEED_ACK = 6
};

static const uint8_t FRAGMENT_DATA_SIZE = 20;
static const uint8_t MAX_FRAGMENTS = 16;
static const uint16_t MAX_MESSAGE_SIZE = FRAGMENT_DATA_SIZE * MAX_FRAGMENTS;
static const uint8_t PAIRING_CHANNEL = 76;
static const uint32_t HOP_START_DELAY_MS = 900;

struct __attribute__((packed)) FragmentPacket
{
    uint8_t type;
    uint16_t msg_id;
    uint8_t fragment_index;
    uint8_t total_fragments;
    uint8_t payload_len;
    char payload[FRAGMENT_DATA_SIZE];
};

struct __attribute__((packed)) FragmentAck
{
    uint8_t type;
    uint16_t msg_id;
    uint8_t fragment_index;
    uint8_t complete;
};

struct __attribute__((packed)) LinkPacket
{
    uint8_t type;
};

struct __attribute__((packed)) SeedAckPacket
{
    uint8_t type;
    uint16_t seed;      // Confirmação da seed recebida
};

static_assert(sizeof(FragmentPacket) <= 32, "FragmentPacket excede 32 bytes do NRF24");
static_assert(sizeof(FragmentAck) <= 32, "FragmentAck excede 32 bytes do NRF24");
static_assert(sizeof(LinkPacket) <= 32, "LinkPacket excede 32 bytes do NRF24");
static_assert(sizeof(SeedAckPacket) <= 32, "SeedAckPacket excede 32 bytes do NRF24");

static char assembled_message[MAX_MESSAGE_SIZE + 1];
static bool received_bitmap[MAX_FRAGMENTS];
static uint8_t expected_total_fragments = 0;
static uint16_t current_msg_id = 0;
static uint8_t received_count = 0;
static bool completion_logged = false;
static bool link_established = false;
static uint32_t hop_counter = 0;
static unsigned long last_hop_time = 0;
static unsigned long hop_start_at = 0;
static uint8_t current_channel = PAIRING_CHANNEL;
static const uint32_t HOP_INTERVAL_MS = 220;

// Variáveis para controle da seed
static uint16_t g_seed = 0;
static bool g_seed_initialized = false;

// ---- Métricas de PLR (Packet Loss Rate) ----
static const uint32_t g_window_size = 50; // hops por janela (~11s a 220ms/hop)
static uint32_t g_window_expected = 0;
static uint32_t g_window_received = 0;
static uint32_t g_session_expected = 0;
static uint32_t g_session_received = 0;
static bool g_hop_received_flag = false; // já contabilizou recepção do hop atual?

// Sinal de "handshake concluído" (seed confirmada, hopping prestes a
// começar) para quem precisar segurar alguma ação até esse ponto — hoje
// usado pelo jammer em placa_b (start-after-handshake) via
// escravo_set_ready_sink(), mesmo padrão de callback de
// mestre_set_alert_source()/analisador_set_alert_sink() do
// spectrum-attack-classification. Sem fonte registrada (nós standalone),
// não tem efeito nenhum.
static void (*g_escravo_ready_sink)(bool) = nullptr;

static void escravo_set_ready_sink(void (*sink)(bool))
{
    g_escravo_ready_sink = sink;
}

static void notify_ready(bool ready)
{
    if (g_escravo_ready_sink)
        g_escravo_ready_sink(ready);
}

static void reset_plr_counters()
{
    g_window_expected = 0;
    g_window_received = 0;
    g_session_expected = 0;
    g_session_received = 0;
    g_hop_received_flag = false;
}

static void log_plr_window()
{
    float plr_window = (g_window_expected > 0)
                            ? 100.0f * (1.0f - ((float)g_window_received / (float)g_window_expected))
                            : 0.0f;
    float plr_session = (g_session_expected > 0)
                             ? 100.0f * (1.0f - ((float)g_session_received / (float)g_session_expected))
                             : 0.0f;

    Serial.printf("PLR;t_ms=%lu;window=%.1f;session=%.1f;recv=%u;exp=%u\n",
                  millis(), plr_window, plr_session, g_window_received, g_window_expected);
}

static void reset_message_state(uint16_t msg_id, uint8_t total_fragments)
{
    memset(assembled_message, 0, sizeof(assembled_message));
    memset(received_bitmap, 0, sizeof(received_bitmap));
    current_msg_id = msg_id;
    expected_total_fragments = total_fragments;
    received_count = 0;
    completion_logged = false;
}

static bool is_message_complete()
{
    return expected_total_fragments > 0 && received_count >= expected_total_fragments;
}

static void escravo_setup()
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
    if (!escravo_radio.begin())
    {
        Serial.println(F("[ERRO ESCRAVO] Falha fatal! O Módulo NRF24 não respondeu no SPI. Verifique a fiação."));
    }

    // Configurações base para bater com o transmissor
    escravo_radio.setPALevel(RF24_PA_LOW);
    escravo_radio.setDataRate(RF24_1MBPS);
    escravo_radio.setChannel(PAIRING_CHANNEL);

    // Obrigatório para AckPayload funcionar, precisamos de tamanhos dinâmicos habilitados
    escravo_radio.enableDynamicPayloads();
    escravo_radio.enableAckPayload();

    escravo_radio.setCRCLength(RF24_CRC_16);

    // Abre APENAS o pipe 1. Fecha o Pipe 0, que é usado primariamente por transmissores.
    // Isso evita captar lixo do ar que pinga no pipe padrão da biblioteca (gerando Pipe 0 ou 7).
    escravo_radio.closeReadingPipe(0);

    // Limpa qualquer lixo que tenha ficado preso na memória da antena de testes anteriores
    escravo_radio.flush_rx();

    // Auto ACK para ele responder o transmissor (Habilitado por default, mas é bom reforçar)
    escravo_radio.setAutoAck(true);

    // Abre pipe de leitura para receber do mestre e configura pipe de escrita para responder
    escravo_radio.openReadingPipe(1, myAddress); // Inicia modo de escuta
    escravo_radio.openWritingPipe(masterAddress);
    escravo_radio.startListening();

    // Limpa a memória pra garantir inicialização fria limpa
    escravo_radio.flush_rx();
    escravo_radio.flush_tx();

    Serial.println("\n--- STATUS DO RÁDIO ESCRAVO (REGISTRADORES) ---");
    escravo_radio.printDetails();
    Serial.println("-----------------------------------------------\n");

    Serial.printf("[ESCRAVO] Aguardando pareamento no canal fixo %u...\n", PAIRING_CHANNEL);
}

static void escravo_loop()
{
    // ---- BLINK NÃO-BLOQUEANTE (HEARTBEAT) ----
    static unsigned long last_blink = 0;
    if (millis() - last_blink >= 500)
    {
        last_blink = millis();
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }

    if (!link_established)
    {
        escravo_radio.setChannel(PAIRING_CHANNEL);

        uint8_t pipe;
        if (escravo_radio.available(&pipe))
        {
            uint8_t size = escravo_radio.getDynamicPayloadSize();
            if (size == sizeof(LinkPacket))
            {
                LinkPacket p;
                escravo_radio.read(&p, sizeof(p));
                if (p.type == MSG_LINK_PROBE)
                {
                    LinkPacket ack = {MSG_LINK_ACK};
                    escravo_radio.stopListening();
                    bool wrote = escravo_radio.write(&ack, sizeof(ack));
                    escravo_radio.startListening();

                    if (wrote)
                    {
                        link_established = true;
                        hop_counter = 0;
                        hop_start_at = millis() + HOP_START_DELAY_MS;
                        last_hop_time = millis();
                        Serial.println("[ESCRAVO] Pareamento confirmado. Aguardando seed...");
                    }
                }
            }
            else
            {
                uint8_t dump[32];
                escravo_radio.read(dump, size);
            }
        }

        return;
    }

    // Se o link foi estabelecido mas a seed ainda não foi recebida
    if (!g_seed_initialized)
    {
        escravo_radio.setChannel(PAIRING_CHANNEL);

        uint8_t pipe;
        if (escravo_radio.available(&pipe))
        {
            uint8_t size = escravo_radio.getDynamicPayloadSize();
            if (size == sizeof(SeedPacket))
            {
                SeedPacket seed_pkt;
                escravo_radio.read(&seed_pkt, sizeof(seed_pkt));
                g_seed = seed_pkt.seed;
                g_seed_initialized = true;
                reset_plr_counters();

                Serial.printf("[ESCRAVO] Seed recebida: %u\n", g_seed);

                // Enviar confirmação de seed
                SeedAckPacket seed_ack = {MSG_SEED_ACK, g_seed};
                escravo_radio.stopListening();
                bool wrote = escravo_radio.write(&seed_ack, sizeof(seed_ack));
                escravo_radio.startListening();

                if (wrote)
                {
                    Serial.println("[ESCRAVO] Confirmação de seed enviada.");
                    hop_start_at = millis() + HOP_START_DELAY_MS;
                    Serial.println("[ESCRAVO] Iniciando hopping em breve...");
                    notify_ready(true);
                }
            }
            else
            {
                uint8_t dump[32];
                escravo_radio.read(dump, size);
            }
        }

        return;
    }

    if (millis() < hop_start_at)
        return;

    if (millis() - last_hop_time >= HOP_INTERVAL_MS)
    {
        last_hop_time = millis();

        // Fecha a contagem do hop que está terminando: sempre esperado,
        // recebido só se algum FragmentPacket válido chegou durante ele.
        g_window_expected++;
        g_session_expected++;
        if (g_hop_received_flag)
        {
            g_window_received++;
            g_session_received++;
        }
        g_hop_received_flag = false;

        if (g_window_expected >= g_window_size)
        {
            log_plr_window();
            g_window_expected = 0;
            g_window_received = 0;
        }

        current_channel = get_next_channel(g_seed, hop_counter);
        escravo_radio.setChannel(current_channel);
        hop_counter++;
    }

    // Se recebeu um dado no canal atual
    uint8_t current_pipe;
    if (escravo_radio.available(&current_pipe))
    {
        if (current_pipe == 1)
        {
            uint8_t dt_size = escravo_radio.getDynamicPayloadSize();
            if (dt_size == sizeof(FragmentPacket))
            {
                FragmentPacket pkt;
                escravo_radio.read(&pkt, sizeof(pkt));

                if (pkt.type == MSG_FRAGMENT && pkt.total_fragments > 0 && pkt.total_fragments <= MAX_FRAGMENTS)
                {
                    g_hop_received_flag = true;

                    if (current_msg_id != pkt.msg_id)
                    {
                        reset_message_state(pkt.msg_id, pkt.total_fragments);
                        Serial.printf("[ESCRAVO] Nova mensagem msg_id=%u com %u fragmentos\n", pkt.msg_id, pkt.total_fragments);
                    }

                    if (pkt.fragment_index < expected_total_fragments && pkt.payload_len <= FRAGMENT_DATA_SIZE)
                    {
                        size_t offset = (size_t)pkt.fragment_index * FRAGMENT_DATA_SIZE;
                        if (offset + pkt.payload_len <= MAX_MESSAGE_SIZE)
                        {
                            memcpy(assembled_message + offset, pkt.payload, pkt.payload_len);
                            if (!received_bitmap[pkt.fragment_index])
                            {
                                received_bitmap[pkt.fragment_index] = true;
                                received_count++;
                            }

                            char fragment_text[FRAGMENT_DATA_SIZE + 1];
                            memset(fragment_text, 0, sizeof(fragment_text));
                            memcpy(fragment_text, pkt.payload, pkt.payload_len);

                            Serial.printf("[ESCRAVO] Canal=%u frag=%u/%u recebida (len=%u)\n",
                                          current_channel,
                                          pkt.fragment_index + 1,
                                          expected_total_fragments,
                                          pkt.payload_len);
                            Serial.printf("[ESCRAVO] Texto do fragmento: %s\n", fragment_text);
                        }
                    }

                    bool complete = is_message_complete();

                    // Envia ACK do fragmento
                    FragmentAck ack;
                    ack.type = MSG_FRAGMENT_ACK;
                    ack.msg_id = pkt.msg_id;
                    ack.fragment_index = pkt.fragment_index;
                    ack.complete = complete ? 1 : 0;

                    escravo_radio.stopListening();
                    bool wrote = escravo_radio.write(&ack, sizeof(ack));
                    if (wrote)
                        Serial.printf("[ESCRAVO] ACK frag=%u enviado (complete=%u)\n", ack.fragment_index + 1, ack.complete);
                    else
                        Serial.println("[ESCRAVO] Falha ao enviar ACK do fragmento");
                    escravo_radio.startListening();

                    if (complete)
                    {
                        if (!completion_logged)
                        {
                            assembled_message[MAX_MESSAGE_SIZE] = '\0';
                            Serial.printf("[ESCRAVO] Mensagem completa reconstruida: %s\n", assembled_message);
                            completion_logged = true;
                        }
                    }
                }
                else
                {
                    Serial.println("[ESCRAVO] Pacote ignorado por formato invalido.");
                }
            }
            else
            {
                // descarta payloads inesperados
                uint8_t dump[32];
                escravo_radio.read(dump, dt_size);
                Serial.printf("[ESCRAVO] Payload inesperado recebido (%d bytes), descartado.\n", dt_size);
            }
        }
        else
        {
            // Descarta completamente pacotes que caírem nos pipes errados
            uint32_t dump_buffer;
            escravo_radio.read(&dump_buffer, sizeof(dump_buffer));
        }
    }
}

#endif // ESCRAVO_NODE_H
