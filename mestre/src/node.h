#ifndef MESTRE_NODE_H
#define MESTRE_NODE_H

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <printf.h> // Necessário para o printDetails()
#include <string.h>
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

// Pino de alerta vindo do analisador (spectrum-attack-classification):
// HIGH enquanto "ataque ativo", LOW caso contrario. Usado como default
// quando nenhuma fonte alternativa é registrada via
// mestre_set_alert_source() (ver two-board-topology). Ver docs/pinout.md.
#define PIN_ALERT_IN 4

static RF24 mestre_radio(PIN_CE, PIN_CSN);

static const byte masterAddress[6] = "MSTR1"; // Endereço deste dispositivo (mestre)
static const byte slaveAddress[6] = "SLV01";  // Endereço do escravo

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
static const uint8_t PAIRING_CHANNEL = 76;
static const uint32_t LINK_RETRY_MS = 300;
static const uint32_t LINK_ACK_WAIT_MS = 120;
static const uint32_t HOP_START_DELAY_MS = 900;
static const uint32_t HOP_INTERVAL_MS = 220;
static const uint32_t ACK_TIMEOUT_MS = 120;

struct __attribute__((packed)) LinkPacket
{
    uint8_t type;
};

struct __attribute__((packed)) SeedAckPacket
{
    uint8_t type;
    uint16_t seed;      // Confirmação da seed recebida
};

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

static_assert(sizeof(FragmentPacket) <= 32, "FragmentPacket excede 32 bytes do NRF24");
static_assert(sizeof(FragmentAck) <= 32, "FragmentAck excede 32 bytes do NRF24");
static_assert(sizeof(LinkPacket) <= 32, "LinkPacket excede 32 bytes do NRF24");
static_assert(sizeof(SeedAckPacket) <= 32, "SeedAckPacket excede 32 bytes do NRF24");

static const char kFullMessage[] = "Mensagem louca enviada em partes durante os saltos FHSS para validar continuidade de comunicaaaa.";

static uint16_t g_msg_id = 1;
static uint8_t g_current_fragment = 0;
static uint8_t g_total_fragments = 0;
static uint32_t g_hop_counter = 0;
static unsigned long g_last_hop_time = 0;
static bool g_done = false;
static bool g_link_established = false;
static unsigned long g_last_link_try = 0;
static unsigned long g_hop_start_at = 0;
static uint8_t g_current_channel = PAIRING_CHANNEL;

// Variáveis para controle da seed
static uint16_t g_seed = 0;
static bool g_seed_initialized = false;
static bool g_seed_confirmed = false;
static unsigned long g_seed_send_time = 0;
static const uint32_t SEED_ACK_WAIT_MS = 500;

// Fonte alternativa do alerta de ataque (two-board-topology): quando
// registrada (Placa A combinada, mestre+analisador na mesma placa), lê uma
// flag compartilhada em vez do GPIO físico. Default (nullptr) preserva o
// comportamento standalone via digitalRead(PIN_ALERT_IN).
static bool (*g_mestre_alert_source)() = nullptr;

static void mestre_set_alert_source(bool (*source)())
{
    g_mestre_alert_source = source;
}

/**
 * Lê a seed do console serial (0-100)
 * Bloqueia até receber uma entrada válida
 */
static void wait_for_seed()
{
    Serial.println("[MESTRE] === CONFIGURAÇÃO DE SEED ===");
    Serial.println("[MESTRE] Digite a seed desejada (0-100) e pressione ENTER:");

    while (!g_seed_initialized)
    {
        if (Serial.available())
        {
            String input = Serial.readStringUntil('\n');
            input.trim();

            int value = input.toInt();
            if (value >= 0 && value <= 100)
            {
                g_seed = (uint16_t)value;
                g_seed_initialized = true;
                Serial.printf("[MESTRE] Seed configurada: %u\n", g_seed);
                Serial.println("[MESTRE] Aguardando pareamento...\n");
            }
            else
            {
                Serial.printf("[MESTRE] Valor inválido: %d. Digite um valor entre 0 e 100.\n", value);
                Serial.println("[MESTRE] Digite a seed desejada (0-100) e pressione ENTER:");
            }
        }
        delay(10);
    }
}

/**
 * Envia a seed para o escravo e aguarda confirmação
 */
static bool send_seed_to_slave()
{
    SeedPacket seed_pkt = {g_seed};

    mestre_radio.setChannel(PAIRING_CHANNEL);
    mestre_radio.stopListening();
    bool sent = mestre_radio.write(&seed_pkt, sizeof(seed_pkt));
    if (!sent)
    {
        Serial.println("[MESTRE] Transmissão de seed falhou.");
        mestre_radio.startListening();
        return false;
    }

    Serial.printf("[MESTRE] Seed %u transmitida. Aguardando confirmação...\n", g_seed);

    mestre_radio.startListening();
    unsigned long start_wait = millis();
    while (millis() - start_wait < SEED_ACK_WAIT_MS)
    {
        if (!mestre_radio.available())
            continue;

        uint8_t size = mestre_radio.getDynamicPayloadSize();
        if (size != sizeof(SeedAckPacket))
        {
            uint8_t dump[32];
            mestre_radio.read(dump, size);
            continue;
        }

        SeedAckPacket ack;
        mestre_radio.read(&ack, sizeof(ack));
        if (ack.type == MSG_SEED_ACK && ack.seed == g_seed)
        {
            Serial.printf("[MESTRE] Confirmação de seed recebida do escravo.\n");
            return true;
        }
    }

    Serial.println("[MESTRE] Timeout esperando confirmação de seed.");
    return false;
}

static bool try_establish_link()
{
    LinkPacket probe = {MSG_LINK_PROBE};

    mestre_radio.setChannel(PAIRING_CHANNEL);
    mestre_radio.stopListening();
    bool sent = mestre_radio.write(&probe, sizeof(probe));
    if (!sent)
    {
        Serial.println("[MESTRE] Pareamento: sem resposta do escravo no canal fixo.");
        mestre_radio.startListening();
        return false;
    }

    mestre_radio.startListening();
    unsigned long start_wait = millis();
    while (millis() - start_wait < LINK_ACK_WAIT_MS)
    {
        if (!mestre_radio.available())
            continue;

        uint8_t size = mestre_radio.getDynamicPayloadSize();
        if (size != sizeof(LinkPacket))
        {
            uint8_t dump[32];
            mestre_radio.read(dump, size);
            continue;
        }

        LinkPacket ack;
        mestre_radio.read(&ack, sizeof(ack));
        if (ack.type == MSG_LINK_ACK)
        {
            return true;
        }
    }

    return false;
}

static uint8_t compute_total_fragments()
{
    size_t len = strlen(kFullMessage);
    return (uint8_t)((len + FRAGMENT_DATA_SIZE - 1) / FRAGMENT_DATA_SIZE);
}

static void fill_fragment(FragmentPacket &pkt, uint8_t fragment_index)
{
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = MSG_FRAGMENT;
    pkt.msg_id = g_msg_id;
    pkt.fragment_index = fragment_index;
    pkt.total_fragments = g_total_fragments;

    size_t start = (size_t)fragment_index * FRAGMENT_DATA_SIZE;
    size_t msg_len = strlen(kFullMessage);
    size_t remaining = (start < msg_len) ? (msg_len - start) : 0;
    size_t chunk = remaining > FRAGMENT_DATA_SIZE ? FRAGMENT_DATA_SIZE : remaining;
    pkt.payload_len = (uint8_t)chunk;
    if (chunk > 0)
    {
        memcpy(pkt.payload, kFullMessage + start, chunk);
    }
}

static bool wait_fragment_ack(uint8_t expected_fragment, bool &all_complete)
{
    all_complete = false;
    unsigned long start_wait = millis();
    while (millis() - start_wait < ACK_TIMEOUT_MS)
    {
        if (!mestre_radio.available())
            continue;

        uint8_t sz = mestre_radio.getDynamicPayloadSize();
        if (sz != sizeof(FragmentAck))
        {
            uint8_t dump[32];
            mestre_radio.read(dump, sz);
            continue;
        }

        FragmentAck ack;
        mestre_radio.read(&ack, sizeof(ack));
        if (ack.type == MSG_FRAGMENT_ACK && ack.msg_id == g_msg_id && ack.fragment_index == expected_fragment)
        {
            all_complete = (ack.complete != 0);
            return true;
        }
    }
    return false;
}

static void mestre_setup()
{
    Serial.begin(115200);
    delay(1000); // Aguarda o monitor serial abrir
    Serial.println("\n[TESTE] Iniciando o Blink básico...");
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(PIN_ALERT_IN, INPUT_PULLDOWN);

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
    if (!mestre_radio.begin())
    {
        Serial.println(F("[ERRO MESTRE] Falha fatal! O Módulo NRF24 não respondeu no SPI. Verifique a fiação."));
    }

    // Configurações base do rádio
    mestre_radio.setPALevel(RF24_PA_LOW);
    mestre_radio.setDataRate(RF24_1MBPS);
    mestre_radio.setChannel(PAIRING_CHANNEL);

    // Habilita Dynamic Payloads e envio de payload no ACK
    mestre_radio.enableDynamicPayloads();
    mestre_radio.enableAckPayload();

    mestre_radio.setCRCLength(RF24_CRC_16);
    mestre_radio.openWritingPipe(slaveAddress);
    mestre_radio.openReadingPipe(1, masterAddress); // permite receber respostas do escravo

    // Limpa a memória de pacotes não enviados que ficaram presos antes do mestre reiniciar
    mestre_radio.flush_tx();

    // O rádio envia e o hardware automaticamente espera o ACK
    mestre_radio.setAutoAck(true);

    mestre_radio.stopListening(); // Modo de Transmissão (inicialmente)

    Serial.println("\n--- STATUS DO RÁDIO MESTRE (REGISTRADORES) ---");
    mestre_radio.printDetails();
    Serial.println("----------------------------------------------\n");

    g_total_fragments = compute_total_fragments();

    Serial.printf("[MESTRE] Rádio configurado. Total de fragmentos: %u\n", g_total_fragments);
    Serial.printf("[MESTRE] Aguardando pareamento no canal fixo %u...\n", PAIRING_CHANNEL);

    // Aguarda a seed ser configurada via serial antes de qualquer coisa
    wait_for_seed();
}

static void mestre_loop()
{
    // ---- BLINK NÃO-BLOQUEANTE (HEARTBEAT) ----
    static unsigned long last_blink = 0;
    if (millis() - last_blink >= 500)
    {
        last_blink = millis();
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }

    // ---- ALERTA DO ANALISADOR (spectrum-attack-classification) ----
    static bool g_alert_active = false;
    bool alert_now = g_mestre_alert_source ? g_mestre_alert_source() : (digitalRead(PIN_ALERT_IN) == HIGH);
    if (alert_now != g_alert_active)
    {
        g_alert_active = alert_now;
        if (alert_now)
            Serial.println("[MESTRE] ALERTA: ataque detectado (analisador).");
        else
            Serial.println("[MESTRE] ALERTA: ataque encerrado.");
    }

    // Se já finalizamos a comunicação, não fazemos mais nada
    if (g_done)
        return;

    if (!g_link_established)
    {
        if (millis() - g_last_link_try >= LINK_RETRY_MS)
        {
            g_last_link_try = millis();
            if (try_establish_link())
            {
                g_link_established = true;
                g_hop_counter = 0;
                g_hop_start_at = millis() + HOP_START_DELAY_MS;
                g_last_hop_time = millis();
                Serial.println("[MESTRE] Pareamento confirmado. Iniciando envio de seed...");
            }
        }
        return;
    }

    // Se o link está estabelecido mas a seed ainda não foi confirmada, enviar agora
    if (!g_seed_confirmed)
    {
        if (send_seed_to_slave())
        {
            g_seed_confirmed = true;
            g_hop_start_at = millis() + HOP_START_DELAY_MS;
            Serial.println("[MESTRE] Seed confirmada. Iniciando hopping em breve...");
        }
        else
        {
            // Se falhar, volta ao estado de link não estabelecido para tentar novamente
            g_link_established = false;
        }
        return;
    }

    if (millis() < g_hop_start_at)
        return;

    // Salto periódico de canal com envio de um fragmento por hop
    if (millis() - g_last_hop_time >= HOP_INTERVAL_MS)
    {
        g_last_hop_time = millis();

        uint8_t channel = get_next_channel(g_seed, g_hop_counter);
        g_current_channel = channel;
        mestre_radio.setChannel(channel);

        FragmentPacket pkt;
        fill_fragment(pkt, g_current_fragment);

        // Envia fragmento no canal atual
        mestre_radio.stopListening();
        bool sent = mestre_radio.write(&pkt, sizeof(pkt));
        if (!sent)
        {
            Serial.printf("[MESTRE] Falha de envio no hop=%lu canal=%u frag=%u\n", g_hop_counter, g_current_channel, g_current_fragment);
            mestre_radio.startListening();
            g_hop_counter++;
            return;
        }

        Serial.printf("[MESTRE] Hop=%lu canal=%u frag=%u/%u len=%u enviada.\n",
                      g_hop_counter,
                      g_current_channel,
                      g_current_fragment + 1,
                      g_total_fragments,
                      pkt.payload_len);

        // Espera ACK do fragmento no mesmo hop/canal
        mestre_radio.startListening();
        bool completed = false;
        bool ack_ok = wait_fragment_ack(g_current_fragment, completed);
        if (ack_ok)
        {
            Serial.printf("[MESTRE] ACK recebido para frag=%u\n", g_current_fragment + 1);
            g_current_fragment++;
            if (completed || g_current_fragment >= g_total_fragments)
            {
                Serial.println("[MESTRE] Mensagem completa confirmada pelo Escravo.");
                g_done = true;
            }
        }
        else
        {
            Serial.printf("[MESTRE] Timeout ACK no hop=%lu, frag=%u sera reenviada no proximo salto.\n", g_hop_counter, g_current_fragment + 1);
        }

        g_hop_counter++;
    }
}

#endif // MESTRE_NODE_H
