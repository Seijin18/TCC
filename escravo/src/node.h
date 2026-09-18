#ifndef ESCRAVO_NODE_H
#define ESCRAVO_NODE_H

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <printf.h>
#include <string.h>
#include "../../shared/hopping.h"
#include "../../shared/protocol.h"
#include "../../shared/telemetry.h"

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

#define PIN_CE 22
#define PIN_CSN 21

#define PIN_SCK 18
#define PIN_MISO 19
#define PIN_MOSI 23

static RF24 escravo_radio(PIN_CE, PIN_CSN);

static const uint8_t MAX_FRAGMENTS = 16;
static const uint16_t MAX_MESSAGE_SIZE = FRAGMENT_DATA_SIZE * MAX_FRAGMENTS;

static char assembled_message[MAX_MESSAGE_SIZE + 1];
static bool received_bitmap[MAX_FRAGMENTS];
static uint8_t expected_total_fragments = 0;
static uint16_t current_msg_id = 0;
static uint8_t received_count = 0;
static bool completion_logged = false;

// ---- Estado de pareamento (adaptive-rendezvous) ----
static bool link_established = false;
static uint8_t g_pair_idx = 0;
static unsigned long g_pair_dwell_start = 0;

// ---- Estado de hopping ----
static uint32_t hop_counter = 0;
// Hopping por evento: instante em que o escravo começou a escutar em
// current_channel -- não um relógio periódico. Ver escravo_advance_hop().
static unsigned long channel_deadline_start = 0;
static unsigned long hop_start_at = 0;
static uint8_t current_channel = PAIRING_CANDIDATES[0];
static uint32_t g_hops_since_rx = 0;

// ---- Seed ----
static uint16_t g_seed = 0;
static bool g_seed_initialized = false;

// ---- dynamic-seed-blacklist: máscara de canais ativos ----
static uint16_t g_channel_mask = 0xFFFF;
static bool g_seed_apply_scheduled = false;
static uint16_t g_seed_apply_value = 0;
static uint32_t g_seed_apply_at = 0;
static bool g_mask_apply_scheduled = false;
static uint16_t g_mask_apply_value = 0;
static uint32_t g_mask_apply_at = 0;

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
// mestre_set_alert_source() do spectrum-attack-classification. Sem fonte
// registrada (nós standalone), não tem efeito nenhum.
//
// NOTA para o main.cpp de quem registrar isto: precisa ser chamado ANTES
// de escravo_setup() -- o guard do blink de LED_BUILTIN (GPIO 2 == MISO do
// HSPI usado pelo jammer na Placa B combinada) decide uma única vez em
// escravo_setup().
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

    guarded_printf("PLR;t_ms=%lu;window=%.1f;session=%.1f;recv=%u;exp=%u\n",
                  millis(), plr_window, plr_session, g_window_received, g_window_expected);
    tlm_printf("escravo", "plr", "window=%.1f;session=%.1f;recv=%u;exp=%u",
               plr_window, plr_session, g_window_received, g_window_expected);
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

static void apply_scheduled_changes()
{
    if (g_seed_apply_scheduled && hop_counter >= g_seed_apply_at)
    {
        g_seed = g_seed_apply_value;
        g_seed_apply_scheduled = false;
        tlm_printf("escravo", "rekey_apply", "kind=seed;seed=%u;hop=%lu", g_seed, hop_counter);
    }
    if (g_mask_apply_scheduled && hop_counter >= g_mask_apply_at)
    {
        g_channel_mask = g_mask_apply_value;
        g_mask_apply_scheduled = false;
        tlm_printf("escravo", "blacklist", "mask=0x%04X;hop=%lu", g_channel_mask, hop_counter);
    }
}

static void reset_to_scanning()
{
    link_established = false;
    g_seed_initialized = false;
    hop_counter = 0;
    g_hops_since_rx = 0;
    g_seed_apply_scheduled = false;
    g_mask_apply_scheduled = false;
    g_channel_mask = 0xFFFF;
    notify_ready(false);
    tlm_event("escravo", "loss_of_sync");
}

static void escravo_setup()
{
    Serial.begin(115200);
    delay(1000); // Aguarda o monitor serial abrir

    // Blink de heartbeat só na placa standalone: na Placa B combinada,
    // GPIO 2 é o MISO do barramento HSPI usado pela task do jammer
    // (two-board-topology seção 7).
    if (!g_escravo_ready_sink)
        pinMode(LED_BUILTIN, OUTPUT);

    guarded_println("\n[ESCRAVO] Iniciando receptor (FIXO)...");

    printf_begin();

    delay(2000);

    pinMode(PIN_CSN, OUTPUT);
    digitalWrite(PIN_CSN, HIGH);
    delay(10);

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);

    if (!escravo_radio.begin())
    {
        guarded_println(F("[ERRO ESCRAVO] Falha fatal! O Módulo NRF24 não respondeu no SPI. Verifique a fiação."));
    }

    escravo_radio.setPALevel(RF24_PA_LOW);
    escravo_radio.setDataRate(RF24_1MBPS);
    escravo_radio.setChannel(PAIRING_CANDIDATES[g_pair_idx]);

    escravo_radio.enableDynamicPayloads();
    // NAO chamar enableAckPayload(): achado de bancada -- ver comentario
    // em mestre/src/node.h (mesmo ponto). Com ack payload habilitado, o
    // receptor descarta silenciosamente pacotes enviados como NO_ACK
    // (radio_send(), usado em todo write() deste projeto) -- reproduzido
    // isolado e 100% repetível com Tests/AntennaCross.
    //
    // adaptive-rendezvous: necessario para radio_send()/writeFast(...,
    // multicast=true) -- ver shared/protocol.h.
    escravo_radio.enableDynamicAck();

    escravo_radio.setCRCLength(RF24_CRC_16);
    escravo_radio.closeReadingPipe(0);
    escravo_radio.flush_rx();
    escravo_radio.setAutoAck(true);

    escravo_radio.openReadingPipe(1, slaveAddress);
    escravo_radio.openWritingPipe(masterAddress);
    escravo_radio.startListening();

    escravo_radio.flush_rx();
    escravo_radio.flush_tx();

    guarded_println("\n--- STATUS DO RÁDIO ESCRAVO (REGISTRADORES) ---");
    escravo_radio.printDetails();
    guarded_println("-----------------------------------------------\n");

    g_pair_dwell_start = millis();
    guarded_printf("[ESCRAVO] Varrendo %u candidatos de pareamento...\n", PAIRING_CANDIDATE_COUNT);
    tlm_event("escravo", "boot");
}

// Hopping por evento: chamado sempre que um "round" de escuta em
// current_channel termina -- por sucesso (fragmento/controle recebido e
// ackado, com `g_hop_received_flag`/`hop_counter` já atualizados pelo
// chamador) ou por HOP_TIMEOUT_MS esgotado sem nada válido (fallback).
// Nos dois casos o efeito é o mesmo: contabiliza PLR desta rodada, avança
// hop_counter em exatamente 1, escolhe e configura o próximo canal, e
// reinicia o deadline de escuta dele.
static void escravo_advance_hop()
{
    g_window_expected++;
    g_session_expected++;
    if (g_hop_received_flag)
    {
        g_window_received++;
        g_session_received++;
        g_hops_since_rx = 0;
    }
    else
    {
        g_hops_since_rx++;
    }
    g_hop_received_flag = false;

    if (g_window_expected >= g_window_size)
    {
        log_plr_window();
        g_window_expected = 0;
        g_window_received = 0;
    }

    hop_counter++;
    current_channel = get_next_active_channel(g_seed, hop_counter, g_channel_mask);
    escravo_radio.setChannel(current_channel);
    // Achado de bancada: mesmo motivo do lado do mestre (ver
    // mestre/src/node.h) -- 500us em vez dos 130us do datasheet.
    delayMicroseconds(500);
    // Hipotese ainda nao testada antes desta reescrita (ver "Correção" em
    // design.md): RX FIFO pode reter payload do canal ANTERIOR apos
    // setChannel() -- sem o flush, esse pacote velho seria lido como se
    // tivesse chegado no canal novo. Agora que o hop e disparado por
    // evento (podendo trocar de canal bem mais rapido que o antigo tick
    // fixo de 220ms em caso de sucesso), essa janela de sobreposicao fica
    // mais provavel de importar.
    escravo_radio.flush_rx();
    channel_deadline_start = millis();
    apply_scheduled_changes();
}

static void escravo_loop()
{
    // ---- BLINK NÃO-BLOQUEANTE (HEARTBEAT) ----
    if (!g_escravo_ready_sink)
    {
        static unsigned long last_blink = 0;
        if (millis() - last_blink >= 500)
        {
            last_blink = millis();
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        }
    }

    if (!link_established)
    {
        if (millis() - g_pair_dwell_start >= PAIR_DWELL_MS)
        {
            g_pair_idx = (uint8_t)((g_pair_idx + 1) % PAIRING_CANDIDATE_COUNT);
            g_pair_dwell_start = millis();
            escravo_radio.setChannel(PAIRING_CANDIDATES[g_pair_idx]);
            tlm_printf("escravo", "pair_scan", "ch=%u", PAIRING_CANDIDATES[g_pair_idx]);
        }

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
                    // radio_send() em vez de write(): ver shared/protocol.h
                    // (bug de retry-forever em RF24::write() nesta versao
                    // da lib) -- o mestre pode nao estar pronto pra
                    // auto-ack no exato instante, e a confirmacao real vem
                    // do MSG_LINK_ACK que o mestre le, nao do ack de
                    // hardware.
                    bool wrote = radio_send(escravo_radio, &ack, sizeof(ack));
                    escravo_radio.startListening();

                    if (wrote)
                    {
                        link_established = true;
                        hop_counter = 0;
                        g_hops_since_rx = 0;
                        hop_start_at = millis() + HOP_START_DELAY_MS;
                        // Hopping por evento: o "relogio" da primeira janela
                        // de escuta comeca no instante em que o hopping
                        // realmente vai comecar (hop_start_at), nao agora --
                        // mesmo motivo do antigo hop_clock_primed (ver git
                        // history), senao a checagem de timeout already
                        // veria ~900ms (HOP_START_DELAY_MS) decorridos na
                        // primeira vez que passar do gate abaixo e avançaria
                        // de canal antes mesmo de escutar current_channel.
                        channel_deadline_start = hop_start_at;
                        guarded_printf("[ESCRAVO] Pareamento confirmado em ch=%u. Aguardando seed...\n", PAIRING_CANDIDATES[g_pair_idx]);
                        tlm_printf("escravo", "pair_ok", "ch=%u", PAIRING_CANDIDATES[g_pair_idx]);
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

    if (!g_seed_initialized)
    {
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

                guarded_printf("[ESCRAVO] Seed recebida: %u\n", g_seed);
                tlm_printf("escravo", "seed", "seed=%u", g_seed);

                SeedAckPacket seed_ack;
                seed_ack.type = MSG_SEED_ACK;
                seed_ack.seed = g_seed;
                escravo_radio.stopListening();
                bool wrote = radio_send(escravo_radio, &seed_ack, sizeof(seed_ack));
                escravo_radio.startListening();

                if (wrote)
                {
                    guarded_println("[ESCRAVO] Confirmação de seed enviada.");
                    hop_start_at = millis() + HOP_START_DELAY_MS;
                    channel_deadline_start = hop_start_at;
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

    // Hopping por evento, não por relógio periódico (ver "Correção" em
    // openspec/changes/adaptive-rendezvous/design.md e mestre_loop() em
    // mestre/src/node.h): current_channel só muda quando (a) um
    // fragmento/pacote de controle válido é recebido e ackado -- evento
    // de sucesso, ver as chamadas a escravo_advance_hop() dentro do bloco
    // de recepção abaixo, que resincronizam hop_counter a partir do valor
    // recebido do mestre ANTES de avançar -- ou (b) HOP_TIMEOUT_MS esgota
    // sem nada válido chegar neste canal (fallback logo abaixo: avança
    // mesmo assim, senão o escravo ficaria preso pra sempre esperando num
    // canal jamado).
    //
    // O `else` entre o timeout e o available() evita chamar
    // escravo_advance_hop() duas vezes na mesma iteração (uma pelo
    // timeout, outra por um pacote que apareceu logo em seguida) -- o que
    // avançaria hop_counter em 2 em vez de 1 e dessincronizaria os dois
    // lados. Um pacote que chegue bem no instante do timeout só é visto
    // na iteração seguinte (delay de loop(), irrelevante no ESP32).
    if (millis() - channel_deadline_start >= HOP_TIMEOUT_MS)
    {
        escravo_advance_hop();
        if (g_hops_since_rx >= LOSS_HOPS)
        {
            reset_to_scanning();
            return;
        }
    }
    else
    {
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
                        // adaptive-rendezvous: adota o contador de hop do
                        // mestre em vez de manter contagem própria --
                        // corrige qualquer deriva entre os dois lados.
                        // escravo_advance_hop() logo abaixo incrementa a
                        // partir DESTE valor, então o próximo canal
                        // escolhido é exatamente o mesmo que o mestre vai
                        // calcular ao avançar o dele.
                        hop_counter = pkt.hop_counter;

                        // Achado de bancada: nenhum guarded_printf/tlm_printf
                        // roda antes do ACK ser enviado (só depois, mais
                        // abaixo) -- eles competem pelo mesmo mutex de
                        // telemetria que a task do jammer também usa, e um
                        // print antes do ack podia atrasar o suficiente pra
                        // mestre já ter desistido de esperar quando o ack
                        // finalmente saía. Só o estado (reset de mensagem,
                        // bitmap, contagem) é atualizado aqui.
                        bool is_new_message = (current_msg_id != pkt.msg_id);
                        if (is_new_message)
                            reset_message_state(pkt.msg_id, pkt.total_fragments);

                        bool fragment_stored = false;
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
                                fragment_stored = true;
                            }
                        }

                        bool complete = is_message_complete();

                        FragmentAck ack;
                        ack.type = MSG_FRAGMENT_ACK;
                        ack.msg_id = pkt.msg_id;
                        ack.fragment_index = pkt.fragment_index;
                        ack.complete = complete ? 1 : 0;

                        escravo_radio.stopListening();
                        radio_send(escravo_radio, &ack, sizeof(ack));
                        escravo_radio.startListening();

                        // Evento de sucesso: avança pro próximo canal já
                        // agora, sem esperar timeout nenhum -- ver
                        // escravo_advance_hop().
                        escravo_advance_hop();

                        // Prints adiados pra depois do ack (ver comentário
                        // acima) -- não afetam o timing crítico daqui em diante.
                        if (is_new_message)
                            guarded_printf("[ESCRAVO] Nova mensagem msg_id=%u com %u fragmentos\n", pkt.msg_id, pkt.total_fragments);
                        if (fragment_stored)
                            guarded_printf("[ESCRAVO] Canal=%u frag=%u/%u recebida (len=%u)\n",
                                          current_channel, pkt.fragment_index + 1, expected_total_fragments, pkt.payload_len);

                        if (complete && !completion_logged)
                        {
                            assembled_message[MAX_MESSAGE_SIZE] = '\0';
                            guarded_printf("[ESCRAVO] Mensagem completa reconstruida: %s\n", assembled_message);
                            tlm_event("escravo", "msg_done");
                            completion_logged = true;
                        }
                    }
                    else
                    {
                        guarded_println("[ESCRAVO] Pacote ignorado por formato invalido.");
                    }
                }
                else if (dt_size == sizeof(ControlSeedPacket)) // == sizeof(ControlBlacklistPacket)
                {
                    uint8_t raw[sizeof(ControlSeedPacket)];
                    escravo_radio.read(raw, dt_size);
                    uint8_t ctrl_type = raw[0];

                    if (ctrl_type == MSG_CTRL_SEED)
                    {
                        ControlSeedPacket pkt;
                        memcpy(&pkt, raw, sizeof(pkt));
                        g_seed_apply_scheduled = true;
                        g_seed_apply_value = pkt.new_seed;
                        g_seed_apply_at = pkt.apply_at_hop;

                        ControlAck ack = {MSG_CTRL_ACK, pkt.apply_at_hop};
                        escravo_radio.stopListening();
                        radio_send(escravo_radio, &ack, sizeof(ack));
                        escravo_radio.startListening();
                        tlm_printf("escravo", "rekey_ack", "kind=seed;seed=%u;apply=%lu", pkt.new_seed, pkt.apply_at_hop);
                        // Mesmo caminho de evento-sucesso do fragmento: o
                        // mestre também avança o dele imediatamente após
                        // enviar um pacote de controle (ver mestre_loop()).
                        escravo_advance_hop();
                    }
                    else if (ctrl_type == MSG_CTRL_BLACKLIST)
                    {
                        ControlBlacklistPacket pkt;
                        memcpy(&pkt, raw, sizeof(pkt));
                        g_mask_apply_scheduled = true;
                        g_mask_apply_value = pkt.mask;
                        g_mask_apply_at = pkt.apply_at_hop;

                        ControlAck ack = {MSG_CTRL_ACK, pkt.apply_at_hop};
                        escravo_radio.stopListening();
                        radio_send(escravo_radio, &ack, sizeof(ack));
                        escravo_radio.startListening();
                        tlm_printf("escravo", "rekey_ack", "kind=blacklist;mask=0x%04X;apply=%lu", pkt.mask, pkt.apply_at_hop);
                        escravo_advance_hop();
                    }
                    else
                    {
                        guarded_println("[ESCRAVO] Pacote de controle com tipo desconhecido, descartado.");
                    }
                }
                else
                {
                    uint8_t dump[32];
                    escravo_radio.read(dump, dt_size);
                    guarded_printf("[ESCRAVO] Payload inesperado recebido (%d bytes), descartado.\n", dt_size);
                }
            }
            else
            {
                uint32_t dump_buffer;
                escravo_radio.read(&dump_buffer, sizeof(dump_buffer));
            }
        }
    }
}

#endif // ESCRAVO_NODE_H
