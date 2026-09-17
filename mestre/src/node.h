#ifndef MESTRE_NODE_H
#define MESTRE_NODE_H

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <printf.h> // Necessário para o printDetails()
#include <string.h>
#include "../../shared/hopping.h"
#include "../../shared/protocol.h"
#include "../../shared/telemetry.h"

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

static const uint32_t PAIR_MEASURE_DWELL_US = 130;

// Fonte alternativa do alerta de ataque (two-board-topology): quando
// registrada (Placa A combinada, mestre+analisador na mesma placa), lê uma
// flag compartilhada em vez do GPIO físico. Default (nullptr) preserva o
// comportamento standalone via digitalRead(PIN_ALERT_IN).
//
// NOTA para o main.cpp de quem registrar isto: precisa ser chamado ANTES
// de mestre_setup(), não depois -- tanto o guard de pinMode(PIN_ALERT_IN)
// quanto o do blink de LED_BUILTIN (GPIO 2 == MISO do HSPI usado pelo nó
// secundário na placa combinada) decidem uma única vez em mestre_setup().
static bool (*g_mestre_alert_source)() = nullptr;

static void mestre_set_alert_source(bool (*source)())
{
    g_mestre_alert_source = source;
}

/**
 * Lê a seed do console serial (0-100). Janela curta e não-bloqueante: se
 * nada for digitado dentro de SEED_WAIT_WINDOW_MS, assume SEED_DEFAULT --
 * adaptive-rendezvous, para setup() nunca travar em bancada não assistida.
 */
static uint16_t g_seed = 0;
static bool g_seed_initialized = false;
static bool g_seed_confirmed = false;
static unsigned long g_seed_send_time = 0;

static void wait_for_seed_window()
{
    Serial.println("[MESTRE] === CONFIGURAÇÃO DE SEED ===");
    Serial.printf("[MESTRE] Digite a seed desejada (0-100) e pressione ENTER (janela de %lu ms, default=%u):\n",
                  SEED_WAIT_WINDOW_MS, SEED_DEFAULT);

    unsigned long start = millis();
    while (millis() - start < SEED_WAIT_WINDOW_MS)
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
                return;
            }
            Serial.printf("[MESTRE] Valor inválido: %d. Digite um valor entre 0 e 100.\n", value);
        }
        delay(10);
    }

    g_seed = SEED_DEFAULT;
    g_seed_initialized = true;
    Serial.printf("[MESTRE] Nenhuma seed digitada a tempo. Usando default: %u\n", g_seed);
}

// Comando runtime "seed <n>" -- só tem efeito antes da seed ser confirmada
// pelo escravo (adaptive-rendezvous, tarefa 4.3). Depois disso a sessão já
// está em andamento; reiniciar a placa para trocar de seed manualmente.
static char g_cmd_buf[32];
static uint8_t g_cmd_len = 0;

static void handle_mestre_command(char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;

    if (strncmp(line, "seed ", 5) == 0)
    {
        if (g_seed_confirmed)
        {
            Serial.println("[MESTRE] Seed já confirmada nesta sessão; reinicie para trocar.");
            return;
        }
        int value = atoi(line + 5);
        if (value >= 0 && value <= 100)
        {
            g_seed = (uint16_t)value;
            Serial.printf("[MESTRE] Seed atualizada via comando: %u\n", g_seed);
        }
        else
        {
            Serial.printf("[MESTRE] Valor de seed inválido: %d\n", value);
        }
    }
    else
    {
        Serial.printf("[MESTRE] Comando desconhecido: %s\n", line);
    }
}

static void process_mestre_serial()
{
    while (Serial.available())
    {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r')
        {
            if (g_cmd_len > 0)
            {
                g_cmd_buf[g_cmd_len] = '\0';
                handle_mestre_command(g_cmd_buf);
                g_cmd_len = 0;
            }
            continue;
        }
        if (g_cmd_len < sizeof(g_cmd_buf) - 1)
            g_cmd_buf[g_cmd_len++] = c;
        else
            g_cmd_len = 0;
    }
}

// ---- Estado de pareamento (adaptive-rendezvous) ----
static bool g_link_established = false;
static uint8_t g_pair_channel = PAIRING_CANDIDATES[0];
static bool g_camping = false;
static unsigned long g_pair_camp_start = 0;
static unsigned long g_last_link_try = 0;

// ---- Estado de hopping / mensagem ----
static uint16_t g_msg_id = 1;
static uint8_t g_current_fragment = 0;
static uint8_t g_total_fragments = 0;
static uint32_t g_hop_counter = 0;
static unsigned long g_last_hop_time = 0;
static unsigned long g_hop_start_at = 0;
static uint8_t g_current_channel = PAIRING_CANDIDATES[0];
static uint32_t g_hops_since_ack = 0;

// ---- dynamic-seed-blacklist: máscara de canais ativos e evidência própria ----
static uint16_t g_channel_mask = 0xFFFF;
static uint8_t g_channel_fail[TABLE_SIZE] = {0};
static bool g_alert_active = false;
static bool g_reseed_needed = false;

// Fila de controle: um pacote pendente por vez (seed tem prioridade sobre
// blacklist -- ver design.md de dynamic-seed-blacklist). 0 = nenhum.
static uint8_t g_control_pending = 0; // MSG_CTRL_SEED ou MSG_CTRL_BLACKLIST
static uint16_t g_control_seed_value = 0;
static uint16_t g_control_mask_value = 0;
static uint32_t g_control_apply_at = 0;
static uint8_t g_control_retries_left = 0;

static bool g_seed_apply_scheduled = false;
static uint16_t g_seed_apply_value = 0;
static uint32_t g_seed_apply_at = 0;

static bool g_mask_apply_scheduled = false;
static uint16_t g_mask_apply_value = 0;
static uint32_t g_mask_apply_at = 0;

static const char kFullMessage[] = "Mensagem louca enviada em partes durante os saltos FHSS para validar continuidade de comunicaaaa.";

static uint8_t find_channel_index(uint8_t ch)
{
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
        if (HOPPING_CHANNELS[i] == ch)
            return i;
    return 0;
}

// -------------------------------------------------------------
// Pareamento: mestre mede atividade e acampa no candidato mais silencioso
// -------------------------------------------------------------
static uint8_t measure_and_pick_candidate()
{
    uint8_t best_idx = 0;
    int best_activity = 999;

    for (uint8_t i = 0; i < PAIRING_CANDIDATE_COUNT; i++)
    {
        mestre_radio.setChannel(PAIRING_CANDIDATES[i]);
        mestre_radio.startListening();
        int hits = 0;
        for (uint32_t s = 0; s < PAIR_MEASURE_SAMPLES; s++)
        {
            delayMicroseconds(PAIR_MEASURE_DWELL_US);
            if (mestre_radio.testCarrier())
                hits++;
        }
        mestre_radio.stopListening();

        if (hits < best_activity)
        {
            best_activity = hits;
            best_idx = i;
        }
    }

    return best_idx;
}

static void pick_new_pair_channel()
{
    uint8_t idx = measure_and_pick_candidate();
    g_pair_channel = PAIRING_CANDIDATES[idx];
    g_pair_camp_start = millis();
    g_camping = true;
    tlm_printf("mestre", "pair_scan", "ch=%u", g_pair_channel);
}

static bool try_establish_link()
{
    LinkPacket probe = {MSG_LINK_PROBE};

    mestre_radio.setChannel(g_pair_channel);
    mestre_radio.stopListening();
    bool sent = mestre_radio.write(&probe, sizeof(probe));
    if (!sent)
    {
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
            return true;
    }

    return false;
}

static bool send_seed_to_slave()
{
    SeedPacket seed_pkt;
    seed_pkt.type = MSG_SEED_INIT;
    seed_pkt.seed = g_seed;

    mestre_radio.setChannel(g_pair_channel);
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
            Serial.println("[MESTRE] Confirmação de seed recebida do escravo.");
            return true;
        }
    }

    Serial.println("[MESTRE] Timeout esperando confirmação de seed.");
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
    pkt.hop_counter = g_hop_counter;

    size_t start = (size_t)fragment_index * FRAGMENT_DATA_SIZE;
    size_t msg_len = strlen(kFullMessage);
    size_t remaining = (start < msg_len) ? (msg_len - start) : 0;
    size_t chunk = remaining > FRAGMENT_DATA_SIZE ? FRAGMENT_DATA_SIZE : remaining;
    pkt.payload_len = (uint8_t)chunk;
    if (chunk > 0)
        memcpy(pkt.payload, kFullMessage + start, chunk);
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

static bool wait_control_ack(uint32_t expected_apply_at)
{
    unsigned long start_wait = millis();
    while (millis() - start_wait < ACK_TIMEOUT_MS)
    {
        if (!mestre_radio.available())
            continue;

        uint8_t sz = mestre_radio.getDynamicPayloadSize();
        if (sz != sizeof(ControlAck))
        {
            uint8_t dump[32];
            mestre_radio.read(dump, sz);
            continue;
        }

        ControlAck ack;
        mestre_radio.read(&ack, sizeof(ack));
        if (ack.type == MSG_CTRL_ACK && ack.apply_at_hop == expected_apply_at)
            return true;
    }
    return false;
}

// -------------------------------------------------------------
// dynamic-seed-blacklist: gatilho de reseed (borda do alerta) e avaliação
// contínua de blacklist (enquanto o alerta persistir)
// -------------------------------------------------------------
static uint16_t compute_candidate_mask()
{
    uint16_t candidate = g_channel_mask;
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
    {
        if (g_channel_fail[i] >= CHANNEL_FAIL_THRESHOLD)
            candidate &= (uint16_t)~(1u << i);
    }
    if (candidate == 0)
        return g_channel_mask; // guarda: nunca esvaziar todos os canais
    return candidate;
}

static void maybe_enqueue_control()
{
    if (g_control_pending != 0)
        return;

    if (g_reseed_needed)
    {
        g_control_pending = MSG_CTRL_SEED;
        g_control_seed_value = (uint16_t)random(0, 101);
        g_control_apply_at = g_hop_counter + CONTROL_RETRY_HOPS + CONTROL_APPLY_MARGIN;
        g_control_retries_left = CONTROL_RETRY_HOPS;
        g_reseed_needed = false;
        tlm_printf("mestre", "rekey_tx", "kind=seed;seed=%u;apply=%lu", g_control_seed_value, g_control_apply_at);
        return;
    }

    if (g_alert_active)
    {
        uint16_t candidate = compute_candidate_mask();
        if (candidate != g_channel_mask)
        {
            g_control_pending = MSG_CTRL_BLACKLIST;
            g_control_mask_value = candidate;
            g_control_apply_at = g_hop_counter + CONTROL_RETRY_HOPS + CONTROL_APPLY_MARGIN;
            g_control_retries_left = CONTROL_RETRY_HOPS;
            tlm_printf("mestre", "rekey_tx", "kind=blacklist;mask=0x%04X;apply=%lu", g_control_mask_value, g_control_apply_at);
        }
    }
}

static void apply_scheduled_changes()
{
    if (g_seed_apply_scheduled && g_hop_counter >= g_seed_apply_at)
    {
        g_seed = g_seed_apply_value;
        g_seed_apply_scheduled = false;
        tlm_printf("mestre", "rekey_apply", "kind=seed;seed=%u;hop=%lu", g_seed, g_hop_counter);
    }
    if (g_mask_apply_scheduled && g_hop_counter >= g_mask_apply_at)
    {
        g_channel_mask = g_mask_apply_value;
        g_mask_apply_scheduled = false;
        tlm_printf("mestre", "blacklist", "mask=0x%04X;hop=%lu", g_channel_mask, g_hop_counter);
    }
}

// Retorna true se o hop atual foi consumido por um pacote de controle (em
// vez de um fragmento de dado) -- fragmento fica pausado até o controle
// resolver (confirmar ou esgotar retries).
static bool send_pending_control_if_any()
{
    if (g_control_pending == 0)
        return false;

    bool acked = false;

    if (g_control_pending == MSG_CTRL_SEED)
    {
        ControlSeedPacket pkt;
        pkt.type = MSG_CTRL_SEED;
        pkt.new_seed = g_control_seed_value;
        pkt.apply_at_hop = g_control_apply_at;

        mestre_radio.stopListening();
        mestre_radio.write(&pkt, sizeof(pkt));
        mestre_radio.startListening();
        acked = wait_control_ack(g_control_apply_at);

        if (acked)
        {
            g_seed_apply_scheduled = true;
            g_seed_apply_value = g_control_seed_value;
            g_seed_apply_at = g_control_apply_at;
        }
    }
    else if (g_control_pending == MSG_CTRL_BLACKLIST)
    {
        ControlBlacklistPacket pkt;
        pkt.type = MSG_CTRL_BLACKLIST;
        pkt.mask = g_control_mask_value;
        pkt.apply_at_hop = g_control_apply_at;

        mestre_radio.stopListening();
        mestre_radio.write(&pkt, sizeof(pkt));
        mestre_radio.startListening();
        acked = wait_control_ack(g_control_apply_at);

        if (acked)
        {
            g_mask_apply_scheduled = true;
            g_mask_apply_value = g_control_mask_value;
            g_mask_apply_at = g_control_apply_at;
        }
    }

    if (acked)
    {
        tlm_printf("mestre", "rekey_ack", "kind=%s;apply=%lu",
                   g_control_pending == MSG_CTRL_SEED ? "seed" : "blacklist", g_control_apply_at);
        g_control_pending = 0;
    }
    else if (g_control_retries_left > 0)
    {
        g_control_retries_left--;
    }
    else
    {
        tlm_printf("mestre", "rekey_failed", "kind=%s", g_control_pending == MSG_CTRL_SEED ? "seed" : "blacklist");
        g_control_pending = 0;
    }

    return true;
}

static void reset_to_pairing()
{
    g_link_established = false;
    g_seed_confirmed = false;
    g_camping = false;
    g_hop_counter = 0;
    g_hops_since_ack = 0;
    g_current_fragment = 0;
    g_control_pending = 0;
    g_seed_apply_scheduled = false;
    g_mask_apply_scheduled = false;
    g_channel_mask = 0xFFFF;
    memset(g_channel_fail, 0, sizeof(g_channel_fail));
    tlm_event("mestre", "loss_of_sync");
}

static void mestre_setup()
{
    Serial.begin(115200);
    delay(1000); // Aguarda o monitor serial abrir

    pinMode(LED_BUILTIN, OUTPUT);
    if (!g_mestre_alert_source)
        pinMode(PIN_ALERT_IN, INPUT_PULLDOWN);

    Serial.println("\n[MESTRE] Iniciando transmissor (FIXO)...");

    delay(2000);

    printf_begin();

    pinMode(PIN_CSN, OUTPUT);
    digitalWrite(PIN_CSN, HIGH);
    delay(10);

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);

    if (!mestre_radio.begin())
    {
        Serial.println(F("[ERRO MESTRE] Falha fatal! O Módulo NRF24 não respondeu no SPI. Verifique a fiação."));
    }

    mestre_radio.setPALevel(RF24_PA_LOW);
    mestre_radio.setDataRate(RF24_1MBPS);
    mestre_radio.setChannel(g_pair_channel);

    mestre_radio.enableDynamicPayloads();
    mestre_radio.enableAckPayload();

    mestre_radio.setCRCLength(RF24_CRC_16);
    mestre_radio.openWritingPipe(slaveAddress);
    mestre_radio.openReadingPipe(1, masterAddress);

    mestre_radio.flush_tx();
    mestre_radio.setAutoAck(true);
    mestre_radio.stopListening();

    Serial.println("\n--- STATUS DO RÁDIO MESTRE (REGISTRADORES) ---");
    mestre_radio.printDetails();
    Serial.println("----------------------------------------------\n");

    g_total_fragments = compute_total_fragments();
    Serial.printf("[MESTRE] Rádio configurado. Total de fragmentos: %u\n", g_total_fragments);

    wait_for_seed_window();

    tlm_event("mestre", "boot");
}

static void mestre_loop()
{
    process_mestre_serial();

    // ---- BLINK NÃO-BLOQUEANTE (HEARTBEAT) ----
    // Só na placa standalone: na Placa A combinada, GPIO 2 é o MISO do
    // barramento HSPI usado pela task do analisador (two-board-topology
    // seção 7) -- a mesma condição usada para o pino de alerta indica
    // modo combinado.
    if (!g_mestre_alert_source)
    {
        static unsigned long last_blink = 0;
        if (millis() - last_blink >= 500)
        {
            last_blink = millis();
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        }
    }

    // ---- ALERTA DO ANALISADOR (spectrum-attack-classification) ----
    bool alert_now = g_mestre_alert_source ? g_mestre_alert_source() : (digitalRead(PIN_ALERT_IN) == HIGH);
    if (alert_now != g_alert_active)
    {
        if (alert_now)
        {
            g_reseed_needed = true;
            Serial.println("[MESTRE] ALERTA: ataque detectado (analisador).");
            tlm_event("mestre", "alert_on");
        }
        else
        {
            Serial.println("[MESTRE] ALERTA: ataque encerrado.");
            tlm_event("mestre", "alert_off");
        }
        g_alert_active = alert_now;
    }

    if (!g_link_established)
    {
        if (!g_camping || millis() - g_pair_camp_start >= PAIR_CAMP_MS)
            pick_new_pair_channel();

        if (millis() - g_last_link_try >= LINK_RETRY_MS)
        {
            g_last_link_try = millis();
            if (try_establish_link())
            {
                g_link_established = true;
                g_hop_counter = 0;
                g_hops_since_ack = 0;
                g_hop_start_at = millis() + HOP_START_DELAY_MS;
                g_last_hop_time = millis();
                Serial.printf("[MESTRE] Pareamento confirmado em ch=%u. Iniciando envio de seed...\n", g_pair_channel);
                tlm_printf("mestre", "pair_ok", "ch=%u", g_pair_channel);
            }
        }
        return;
    }

    if (!g_seed_confirmed)
    {
        if (send_seed_to_slave())
        {
            g_seed_confirmed = true;
            g_hop_start_at = millis() + HOP_START_DELAY_MS;
            tlm_printf("mestre", "seed", "seed=%u", g_seed);
        }
        else
        {
            g_link_established = false;
        }
        return;
    }

    if (millis() < g_hop_start_at)
        return;

    if (millis() - g_last_hop_time >= HOP_INTERVAL_MS)
    {
        g_last_hop_time = millis();

        maybe_enqueue_control();

        uint8_t channel = get_next_active_channel(g_seed, g_hop_counter, g_channel_mask);
        g_current_channel = channel;
        mestre_radio.setChannel(channel);

        if (send_pending_control_if_any())
        {
            g_hop_counter++;
            apply_scheduled_changes();
            return;
        }

        FragmentPacket pkt;
        fill_fragment(pkt, g_current_fragment);

        mestre_radio.stopListening();
        bool sent = mestre_radio.write(&pkt, sizeof(pkt));
        if (!sent)
        {
            Serial.printf("[MESTRE] Falha de envio no hop=%lu canal=%u frag=%u\n", g_hop_counter, g_current_channel, g_current_fragment);
            mestre_radio.startListening();
            g_hops_since_ack++;
            uint8_t idx = find_channel_index(channel);
            if (g_channel_fail[idx] < 255)
                g_channel_fail[idx]++;
            g_hop_counter++;
            apply_scheduled_changes();
            if (g_hops_since_ack >= LOSS_HOPS)
                reset_to_pairing();
            return;
        }

        tlm_printf("mestre", "hop", "ch=%u;n=%lu;frag=%u", channel, g_hop_counter, g_current_fragment);
        Serial.printf("[MESTRE] Hop=%lu canal=%u frag=%u/%u len=%u enviada.\n",
                      g_hop_counter, g_current_channel, g_current_fragment + 1, g_total_fragments, pkt.payload_len);

        mestre_radio.startListening();
        bool completed = false;
        bool ack_ok = wait_fragment_ack(g_current_fragment, completed);

        uint8_t idx = find_channel_index(channel);
        if (ack_ok)
        {
            g_hops_since_ack = 0;
            if (g_channel_fail[idx] > 0)
                g_channel_fail[idx]--;

            tlm_printf("mestre", "ack", "ch=%u;frag=%u", channel, g_current_fragment);
            Serial.printf("[MESTRE] ACK recebido para frag=%u\n", g_current_fragment + 1);
            g_current_fragment++;
            if (completed || g_current_fragment >= g_total_fragments)
            {
                Serial.println("[MESTRE] Mensagem completa confirmada pelo Escravo.");
                tlm_event("mestre", "msg_done");
                g_msg_id++;
                g_current_fragment = 0;
            }
        }
        else
        {
            g_hops_since_ack++;
            if (g_channel_fail[idx] < 255)
                g_channel_fail[idx]++;
            tlm_printf("mestre", "miss", "ch=%u;frag=%u", channel, g_current_fragment);
            Serial.printf("[MESTRE] Timeout ACK no hop=%lu, frag=%u sera reenviada no proximo salto.\n", g_hop_counter, g_current_fragment + 1);
        }

        g_hop_counter++;
        apply_scheduled_changes();

        if (g_hops_since_ack >= LOSS_HOPS)
            reset_to_pairing();
    }
}

#endif // MESTRE_NODE_H
