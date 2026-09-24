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
#include "../../shared/radio_diag.h"

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

// pairing-handshake-hardening: RF24Diag (shared/radio_diag.h) em vez de
// RF24 puro -- so acrescenta um leitor publico de registrador sobre o
// mesmo objeto, nenhuma mudanca de comportamento (ver comentario lá).
static RF24Diag mestre_radio(PIN_CE, PIN_CSN);

// Achado de bancada (2026-09-24): igual ao motivo de HOP_TIMEOUT_MS/o
// delayMicroseconds(500) em mestre_loop() (ver comentário lá) -- 130us
// (o valor nominal do datasheet) não é suficiente pro PLL assentar após um
// salto grande de frequência nestes módulos específicos. Aqui isso
// enviesava measure_and_pick_candidate(): candidato 0 (primeiro avaliado,
// e o canal em que o rádio já estava desde o setup) sempre "ganhava" por
// os demais lerem atividade espúria (settling incompleto, não ruído
// real) -- confirmado em bancada, ~90% das escolhas caindo no candidato 0
// em duas sessões distintas, mesmo sob condições limpas.
static const uint32_t PAIR_MEASURE_DWELL_US = 500;

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
    guarded_println("[MESTRE] === CONFIGURAÇÃO DE SEED ===");
    guarded_printf("[MESTRE] Digite a seed desejada (0-100) e pressione ENTER (janela de %lu ms, default=%u):\n",
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
                guarded_printf("[MESTRE] Seed configurada: %u\n", g_seed);
                return;
            }
            guarded_printf("[MESTRE] Valor inválido: %d. Digite um valor entre 0 e 100.\n", value);
        }
        delay(10);
    }

    g_seed = SEED_DEFAULT;
    g_seed_initialized = true;
    guarded_printf("[MESTRE] Nenhuma seed digitada a tempo. Usando default: %u\n", g_seed);
}

// scenario-automation: Cenário A (sem hopping) -- "mode fixed" trava o
// canal de dados em TEST_FIXED_CHANNEL e desativa reseed/blacklist (o
// objetivo é medir o baseline SEM nenhuma adaptação). "mode hopping" volta
// ao comportamento normal (default).
static bool g_fixed_channel_mode = false;

// scenario-automation: Cenário B (hopping fixo, sem self-healing) vs.
// Cenário C (sistema completo, default) -- "mitigation off" desativa só a
// REAÇÃO (reseed/blacklist) sem desativar o hopping em si, ortogonal a
// g_fixed_channel_mode. Combinações válidas: fixed (A) · hopping+mitigation
// off (B) · hopping+mitigation on (C, default).
static bool g_mitigation_enabled = true;

// scenario-automation: comando "reset" força reset_to_pairing() (definida
// mais abaixo) por serial -- fronteira limpa entre cenários da bateria
// A/B/C sem precisar reiniciar a placa (zera g_channel_mask/g_channel_fail/
// g_channel_blacklisted_since, força re-pareamento).
static void reset_to_pairing();

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
            guarded_println("[MESTRE] Seed já confirmada nesta sessão; reinicie para trocar.");
            return;
        }
        int value = atoi(line + 5);
        if (value >= 0 && value <= 100)
        {
            g_seed = (uint16_t)value;
            guarded_printf("[MESTRE] Seed atualizada via comando: %u\n", g_seed);
        }
        else
        {
            guarded_printf("[MESTRE] Valor de seed inválido: %d\n", value);
        }
    }
    else if (strcmp(line, "mode fixed") == 0)
    {
        g_fixed_channel_mode = true;
        tlm_event("mestre", "mode_fixed");
        guarded_printf("[MESTRE] Modo fixo: canal travado em %u (sem hopping, sem reseed/blacklist)\n", TEST_FIXED_CHANNEL);
    }
    else if (strcmp(line, "mode hopping") == 0)
    {
        g_fixed_channel_mode = false;
        tlm_event("mestre", "mode_hopping");
        guarded_println("[MESTRE] Modo hopping normal restaurado.");
    }
    else if (strcmp(line, "mitigation off") == 0)
    {
        g_mitigation_enabled = false;
        tlm_event("mestre", "mitigation_off");
        guarded_println("[MESTRE] Mitigação (reseed/blacklist) desativada -- hopping continua normal (Cenário B).");
    }
    else if (strcmp(line, "mitigation on") == 0)
    {
        g_mitigation_enabled = true;
        tlm_event("mestre", "mitigation_on");
        guarded_println("[MESTRE] Mitigação (reseed/blacklist) reativada (Cenário C, default).");
    }
    else if (strcmp(line, "reset") == 0)
    {
        guarded_println("[MESTRE] Reset solicitado via comando -- voltando ao pareamento.");
        reset_to_pairing();
    }
    else if (strcmp(line, "regs") == 0)
    {
        // pairing-handshake-hardening (tarefa 1.2): dump de registradores
        // sob demanda, sem mudar nenhum estado -- útil pra comparar contra
        // o dump automático de boot/entrada em pareamento em bancada.
        tlm_radio_regs("mestre", mestre_radio, "cmd");
        guarded_println("[MESTRE] Registradores do radio despejados (e=regs;why=cmd).");
    }
    else
    {
        guarded_printf("[MESTRE] Comando desconhecido: %s\n", line);
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

// adaptive-rendezvous (2026-09-24): "blacklist de pareamento" -- mesma
// lógica de update_channel_mask() (blacklist-decay), aplicada aos 8
// PAIRING_CANDIDATES em vez dos 16 HOPPING_CHANNELS. g_candidate_mask usa
// só os 8 bits baixos (bit i = 1 -> PAIRING_CANDIDATES[i] disponível pra
// medir/acampar). g_pair_round é o "relógio lógico" aqui (não existe
// hop_counter antes de parear) -- incrementado a cada resultado de
// tentativa (sucesso ou falha) registrado em note_candidate_result().
static uint16_t g_candidate_mask = (1u << PAIRING_CANDIDATE_COUNT) - 1;
static uint8_t g_candidate_fail[PAIRING_CANDIDATE_COUNT] = {0};
static uint32_t g_candidate_quarantined_since[PAIRING_CANDIDATE_COUNT] = {0};
static uint32_t g_pair_round = 0;
static uint8_t g_camped_idx = 0; // índice (em PAIRING_CANDIDATES) do candidato atual, p/ penalizar/confirmar

// pairing-handshake-hardening (tarefa 1.3): contadores da janela de
// acampamento corrente -- zerados a cada novo acampamento
// (pick_new_pair_channel()), emitidos como "e=pair_stats" quando a janela
// termina (por timeout ou por sucesso) -- ver design.md H1/H4: distinguir
// "nenhum pacote chegou" (rx_total=0) de "chegou algo que não era o ack
// esperado" (rx_other>0) é o que discrimina as duas hipóteses.
static uint16_t g_pair_probes_ok = 0;
static uint16_t g_pair_probes_fail = 0;
static uint16_t g_pair_rx_total = 0;
static uint16_t g_pair_rx_link_ack = 0;
static uint16_t g_pair_rx_other = 0;
// tarefa 1.5: só imprime a linha humana na primeira falha de envio de
// sonda de CADA janela, pra não inundar a serial numa janela ruim inteira
// -- a contagem completa (probes_fail) já vai pro pair_stats de qualquer
// forma.
static bool g_pair_probe_fail_reported = false;

static void tlm_pair_stats(uint8_t ch, unsigned long window_ms)
{
    tlm_printf("mestre", "pair_stats",
               "ch=%u;probes_ok=%u;probes_fail=%u;rx_total=%u;rx_link_ack=%u;rx_other=%u;ms=%lu",
               ch, g_pair_probes_ok, g_pair_probes_fail, g_pair_rx_total, g_pair_rx_link_ack,
               g_pair_rx_other, window_ms);
}

// ---- Estado de hopping / mensagem ----
static uint16_t g_msg_id = 1;
static uint8_t g_current_fragment = 0;
static uint8_t g_total_fragments = 0;
static uint32_t g_hop_counter = 0;
static unsigned long g_hop_start_at = 0;
static uint8_t g_current_channel = PAIRING_CANDIDATES[0];
static uint32_t g_hops_since_ack = 0;

// ---- dynamic-seed-blacklist: máscara de canais ativos e evidência própria ----
static uint16_t g_channel_mask = 0xFFFF;
static uint8_t g_channel_fail[TABLE_SIZE] = {0};
// blacklist-decay: hop_counter em que cada canal foi excluído pela última
// vez -- só usado enquanto o bit correspondente está em 0 em g_channel_mask
// (ver update_channel_mask() em shared/hopping.h). Zerado em
// reset_to_pairing() junto com g_channel_fail/g_channel_mask (evita
// underflow com g_hop_counter reiniciado -- ver design.md do change).
static uint32_t g_channel_blacklisted_since[TABLE_SIZE] = {0};
static bool g_alert_active = false;
static bool g_reseed_needed = false;

// scenario-automation: throughput (fragmentos confirmados/s) por janela de
// hops -- mesmo padrão de janela de plr-metric (escravo, g_window_size).
static const uint32_t THROUGHPUT_WINDOW_HOPS = 50;
static uint32_t g_throughput_window_confirmed = 0;
static uint32_t g_throughput_window_hops = 0;
static unsigned long g_throughput_window_start_ms = 0;

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
// adaptive-rendezvous (2026-09-24): só considera candidatos habilitados em
// g_candidate_mask -- um candidato "quarentenado" (falhou repetidamente no
// handshake real, mesmo medindo quieto por RPD -- ver note_candidate_result())
// fica de fora da medição/seleção até o cooldown vencer.
static uint8_t measure_and_pick_candidate()
{
    uint8_t best_idx = 255;
    int best_activity = 999;

    for (uint8_t i = 0; i < PAIRING_CANDIDATE_COUNT; i++)
    {
        if (!(g_candidate_mask & (1u << i)))
            continue;

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

    // g_candidate_mask nunca deveria chegar a 0 (update_channel_mask() tem
    // a mesma guarda de blacklist-decay), mas se acontecer, não trava sem
    // candidato nenhum -- volta pro primeiro.
    return best_idx == 255 ? 0 : best_idx;
}

// adaptive-rendezvous (2026-09-24): registra o resultado (sucesso/falha) da
// última tentativa de pareamento no candidato atual e reavalia
// g_candidate_mask -- mesma função pura de blacklist-decay
// (update_channel_mask()), agora sobre PAIRING_CANDIDATES. Chamado nos dois
// pontos onde uma tentativa de fato termina: falha em conseguir link dentro
// da janela de camping, ou falha/sucesso no handshake de seed (ver
// mestre_loop()).
static void note_candidate_result(bool success)
{
    uint8_t idx = g_camped_idx;
    if (success)
        g_candidate_fail[idx] = 0;
    else if (g_candidate_fail[idx] < 255)
        g_candidate_fail[idx]++;

    g_pair_round++;
    uint16_t new_mask = update_channel_mask(g_candidate_mask, g_candidate_fail, g_candidate_quarantined_since,
                                             g_pair_round, PAIR_FAIL_THRESHOLD, PAIR_RECOVERY_ROUNDS,
                                             PAIRING_CANDIDATE_COUNT);
    if (new_mask != g_candidate_mask)
    {
        tlm_printf("mestre", "pair_mask", "mask=0x%02X;prev=0x%02X", new_mask, g_candidate_mask);
        g_candidate_mask = new_mask;
    }
}

static void pick_new_pair_channel()
{
    uint8_t idx = measure_and_pick_candidate();
    g_camped_idx = idx;
    g_pair_channel = PAIRING_CANDIDATES[idx];
    g_pair_camp_start = millis();
    g_camping = true;
    // tarefa 1.3: nova janela de acampamento -- zera os contadores da
    // anterior (já emitidos via tlm_pair_stats() por quem chamou isto).
    g_pair_probes_ok = 0;
    g_pair_probes_fail = 0;
    g_pair_rx_total = 0;
    g_pair_rx_link_ack = 0;
    g_pair_rx_other = 0;
    g_pair_probe_fail_reported = false;
    tlm_printf("mestre", "pair_scan", "ch=%u", g_pair_channel);
}

static bool try_establish_link()
{
    LinkPacket probe = {MSG_LINK_PROBE};

    mestre_radio.setChannel(g_pair_channel);
    mestre_radio.stopListening();
    // radio_send() em vez de write(): ver shared/protocol.h para o porque
    // (bug de retry-forever em RF24::write() nesta versao da lib quando
    // TX_DS/TX_DF nunca setam, reproduzido em bancada com o escravo fora
    // do canal na hora da sonda).
    bool sent = radio_send(mestre_radio, &probe, sizeof(probe));
    if (!sent)
    {
        // tarefa 1.5: falha de radio_send() na sonda deixa de ser
        // silenciosa -- contada em pair_stats, com linha humana só na
        // primeira ocorrência desta janela.
        g_pair_probes_fail++;
        if (!g_pair_probe_fail_reported)
        {
            g_pair_probe_fail_reported = true;
            guarded_printf("[MESTRE] Falha ao enviar sonda de pareamento em ch=%u (radio_send).\n", g_pair_channel);
        }
        mestre_radio.startListening();
        return false;
    }
    g_pair_probes_ok++;

    mestre_radio.startListening();
    unsigned long start_wait = millis();
    while (millis() - start_wait < LINK_ACK_WAIT_MS)
    {
        if (!mestre_radio.available())
            continue;

        g_pair_rx_total++;
        uint8_t size = mestre_radio.getDynamicPayloadSize();
        if (size != sizeof(LinkPacket))
        {
            uint8_t dump[32];
            mestre_radio.read(dump, size);
            g_pair_rx_other++;
            continue;
        }

        LinkPacket ack;
        mestre_radio.read(&ack, sizeof(ack));
        if (ack.type == MSG_LINK_ACK)
        {
            g_pair_rx_link_ack++;
            return true;
        }
        g_pair_rx_other++;
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
    bool sent = radio_send(mestre_radio, &seed_pkt, sizeof(seed_pkt));
    if (!sent)
    {
        guarded_println("[MESTRE] Transmissão de seed falhou.");
        mestre_radio.startListening();
        return false;
    }

    guarded_printf("[MESTRE] Seed %u transmitida. Aguardando confirmação...\n", g_seed);

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
            guarded_println("[MESTRE] Confirmação de seed recebida do escravo.");
            return true;
        }
    }

    guarded_println("[MESTRE] Timeout esperando confirmação de seed.");
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

static bool wait_fragment_ack(uint8_t expected_fragment, bool &all_complete, uint32_t timeout_ms)
{
    all_complete = false;
    unsigned long start_wait = millis();
    while (millis() - start_wait < timeout_ms)
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
// dynamic-seed-blacklist / blacklist-decay: gatilho de reseed (borda do
// alerta) e avaliação contínua de blacklist -- exclusão E reinclusão, sem
// depender do alerta estar ativo (ver design.md de blacklist-decay: o
// alerta continua sendo o único gatilho de reseed, uma ação mais
// disruptiva, mas blacklist virou uma correção contínua de baixo custo).
// -------------------------------------------------------------
static uint16_t compute_candidate_mask()
{
    return update_channel_mask(g_channel_mask, g_channel_fail, g_channel_blacklisted_since,
                                g_hop_counter, CHANNEL_FAIL_THRESHOLD, RECOVERY_COOLDOWN_HOPS);
}

static void maybe_enqueue_control()
{
    if (g_control_pending != 0)
        return;
    // scenario-automation, Cenário A: modo fixo mede o baseline SEM
    // nenhuma adaptação -- nenhum reseed nem reavaliação de blacklist
    // enquanto ativo.
    if (g_fixed_channel_mode)
        return;
    // scenario-automation, Cenário B: hopping continua normal, mas sem
    // reação a ataque -- mede o quanto o FHSS sozinho (sem detecção nem
    // blacklist) já ajuda, isolado do ganho adicional da mitigação ativa.
    if (!g_mitigation_enabled)
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

// scenario-automation: throughput = fragmentos confirmados / segundo,
// numa janela de THROUGHPUT_WINDOW_HOPS hops (mesmo padrão de janela de
// plr-metric no escravo) -- conta tanto hops de controle quanto de dados
// no tamanho da janela, mas só fragmentos realmente confirmados (ack_ok)
// no numerador.
static void note_hop_for_throughput(bool fragment_confirmed)
{
    if (g_throughput_window_hops == 0)
        g_throughput_window_start_ms = millis();

    g_throughput_window_hops++;
    if (fragment_confirmed)
        g_throughput_window_confirmed++;

    if (g_throughput_window_hops >= THROUGHPUT_WINDOW_HOPS)
    {
        unsigned long elapsed_ms = millis() - g_throughput_window_start_ms;
        float frags_per_s = elapsed_ms > 0 ? (1000.0f * (float)g_throughput_window_confirmed / (float)elapsed_ms) : 0.0f;
        tlm_printf("mestre", "throughput", "frags_per_s=%.2f;confirmed=%u;window_ms=%lu",
                   frags_per_s, g_throughput_window_confirmed, elapsed_ms);
        g_throughput_window_hops = 0;
        g_throughput_window_confirmed = 0;
    }
}

static void reset_throughput_window()
{
    g_throughput_window_hops = 0;
    g_throughput_window_confirmed = 0;
    g_throughput_window_start_ms = 0;
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
        uint16_t prev_mask = g_channel_mask;
        g_channel_mask = g_mask_apply_value;
        g_mask_apply_scheduled = false;
        // blacklist-decay: prev= permite tools/logger e plot_scenario.py
        // distinguirem visualmente exclusão (bits somem) de reinclusão
        // (bits voltam) sem precisar rastrear estado próprio.
        tlm_printf("mestre", "blacklist", "mask=0x%04X;prev=0x%04X;hop=%lu", g_channel_mask, prev_mask, g_hop_counter);
    }
}

// Retorna true se o hop atual foi consumido por um pacote de controle (em
// vez de um fragmento de dado) -- fragmento fica pausado até o controle
// resolver (confirmar ou esgotar retries). `acked_out` reporta se ESTA
// tentativa em particular foi confirmada (usado pelo chamador pra manter
// g_hops_since_ack coerente -- ver achado de bancada 2026-09-24 no
// comentário de reset_to_pairing() abaixo).
static bool send_pending_control_if_any(bool &acked_out)
{
    acked_out = false;
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
        radio_send(mestre_radio, &pkt, sizeof(pkt));
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
        radio_send(mestre_radio, &pkt, sizeof(pkt));
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

    acked_out = acked;
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
    // blacklist-decay: g_hop_counter volta a 0 acima -- sem isto,
    // hop_counter - blacklisted_since[i] usaria valores da sessão anterior
    // e faria wraparound (unsigned), tornando todos os canais "elegíveis"
    // pra reinclusão instantaneamente (ver design.md, risco de underflow).
    memset(g_channel_blacklisted_since, 0, sizeof(g_channel_blacklisted_since));
    reset_throughput_window();
    tlm_event("mestre", "loss_of_sync");
    // tarefa 1.2: dump de registradores toda vez que o mestre entra (ou
    // volta a entrar) no estado de pareamento -- compara contra o dump do
    // boot pra confirmar/descartar H4 (design.md).
    tlm_radio_regs("mestre", mestre_radio, "loss");
}

static void mestre_setup()
{
    Serial.begin(115200);
    delay(1000); // Aguarda o monitor serial abrir

    pinMode(LED_BUILTIN, OUTPUT);
    if (!g_mestre_alert_source)
        pinMode(PIN_ALERT_IN, INPUT_PULLDOWN);

    guarded_println("\n[MESTRE] Iniciando transmissor (FIXO)...");

    delay(2000);

    printf_begin();

    pinMode(PIN_CSN, OUTPUT);
    digitalWrite(PIN_CSN, HIGH);
    delay(10);

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);

    if (!mestre_radio.begin())
    {
        guarded_println(F("[ERRO MESTRE] Falha fatal! O Módulo NRF24 não respondeu no SPI. Verifique a fiação."));
    }

    mestre_radio.setPALevel(RF24_PA_LOW);
    mestre_radio.setDataRate(RF24_1MBPS);
    mestre_radio.setChannel(g_pair_channel);

    mestre_radio.enableDynamicPayloads();
    // NAO chamar enableAckPayload(): achado de bancada -- nestes módulos
    // (clones comuns baseados em SI24R1), habilitar ack payload junto com
    // envios NO_ACK (radio_send(), usado em todo write() deste projeto)
    // faz o receptor descartar o pacote silenciosamente, sem nunca disparar
    // available() -- reproduzido de forma isolada e 100% repetível com
    // Tests/AntennaCross (setAutoAck(true) sozinho: ~100% de entrega;
    // + enableAckPayload(): ~0%). writeAckPayload() nunca é chamado neste
    // protocolo (a confirmação real vem de pacotes de resposta explícitos
    // -- LinkAck/SeedAck/FragmentAck/ControlAck), então a feature nunca fez
    // falta.
    //
    // adaptive-rendezvous: enableDynamicAck() é necessário para
    // write(..., multicast=true) na sonda de pareamento (ver
    // try_establish_link) -- sem isso o bit EN_DYN_ACK do FEATURE register
    // nao é setado e o comando W_TX_PAYLOAD_NO_ACK não tem efeito.
    mestre_radio.enableDynamicAck();

    mestre_radio.setCRCLength(RF24_CRC_16);
    mestre_radio.openWritingPipe(slaveAddress);
    mestre_radio.openReadingPipe(1, masterAddress);

    mestre_radio.flush_tx();
    mestre_radio.setAutoAck(true);
    mestre_radio.stopListening();

    guarded_println("\n--- STATUS DO RÁDIO MESTRE (REGISTRADORES) ---");
    mestre_radio.printDetails();
    guarded_println("----------------------------------------------\n");

    g_total_fragments = compute_total_fragments();
    guarded_printf("[MESTRE] Rádio configurado. Total de fragmentos: %u\n", g_total_fragments);

    wait_for_seed_window();

    tlm_event("mestre", "boot");
    // tarefa 1.2: número-base de registradores no boot, pra comparar
    // contra os dumps de entrada em pareamento por perda (design.md H4).
    tlm_radio_regs("mestre", mestre_radio, "boot");
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
            guarded_println("[MESTRE] ALERTA: ataque detectado (analisador).");
            tlm_event("mestre", "alert_on");
        }
        else
        {
            guarded_println("[MESTRE] ALERTA: ataque encerrado.");
            tlm_event("mestre", "alert_off");
        }
        g_alert_active = alert_now;
    }

    if (!g_link_established)
    {
        // adaptive-rendezvous: janela de camping esgotou sem nunca
        // conseguir link neste candidato -- conta como falha antes de
        // trocar (ver note_candidate_result()). Não penaliza a
        // primeiríssima seleção (!g_camping), que não é uma falha real.
        bool camp_expired = g_camping && (millis() - g_pair_camp_start >= PAIR_CAMP_MS);
        if (camp_expired)
        {
            // tarefa 1.3: fecha a janela de acampamento que acabou de
            // esgotar -- emitir ANTES de pick_new_pair_channel() zerar os
            // contadores e trocar g_pair_channel.
            tlm_pair_stats(g_pair_channel, (unsigned long)(millis() - g_pair_camp_start));
            note_candidate_result(false);
        }
        if (!g_camping || camp_expired)
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
                guarded_printf("[MESTRE] Pareamento confirmado em ch=%u. Iniciando envio de seed...\n", g_pair_channel);
                // tarefa 1.3: pareamento também fecha a janela corrente
                // (sucesso, não timeout) -- mesmos contadores acumulados
                // desde pick_new_pair_channel().
                tlm_pair_stats(g_pair_channel, (unsigned long)(millis() - g_pair_camp_start));
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
            // adaptive-rendezvous: só agora o candidato provou que
            // suporta troca de pacote real, não só RPD quieto -- reseta
            // o histórico de falha dele.
            note_candidate_result(true);
        }
        else
        {
            // Achado de bancada 2026-09-24: um candidato pode conseguir
            // pair_ok (LINK_ACK) e mesmo assim falhar aqui repetidamente
            // -- RPD quieto não garante que um pacote de verdade
            // atravessa. Conta como falha do candidato, não só reverte
            // o link.
            note_candidate_result(false);
            g_link_established = false;
            // tarefa 1.2: volta a "entrar em pareamento" (mesmo sem passar
            // por reset_to_pairing() -- só a seed que falhou, o resto do
            // estado de sessão continua) -- dump de registradores aqui
            // também, já que é exatamente o caminho de falha do E1
            // (handshake meio-aberto) em design.md.
            tlm_radio_regs("mestre", mestre_radio, "seed_timeout");
        }
        return;
    }

    if (millis() < g_hop_start_at)
        return;

    // Hopping por evento, não por relógio periódico (ver "Correção" em
    // openspec/changes/adaptive-rendezvous/design.md): cada chamada
    // daqui pra baixo faz UMA tentativa de hop completa (bloqueante),
    // que só termina por um dos dois motivos -- ack recebido (evento de
    // sucesso) ou HOP_TIMEOUT_MS esgotado sem resposta (fallback: avança
    // pro próximo canal mesmo assim, senão o enlace travaria pra sempre
    // num canal jamado). Não há mais nenhum relógio livre (`g_last_hop_time`)
    // rodando contra o wall-clock -- o único estado que precisa concordar
    // entre mestre e escravo é `g_hop_counter`/`hop_counter` (um inteiro,
    // não um instante de tempo), e ele só muda por evento explícito dos
    // dois lados: sucesso (os dois avançam pro MESMO próximo valor,
    // porque o escravo resincroniza o dele a partir do `hop_counter` que
    // veio no pacote) ou timeout independente em cada lado (cada um
    // incrementa exatamente 1 por rodada, sem pular nem duplicar, então
    // o valor inteiro não diverge mesmo sem os dois lados terminarem a
    // rodada no mesmo instante real).
    maybe_enqueue_control();

    // scenario-automation, Cenário A: modo fixo ignora seed/mascara e usa
    // sempre o mesmo canal -- ver TEST_FIXED_CHANNEL em shared/hopping.h.
    uint8_t channel = g_fixed_channel_mode ? TEST_FIXED_CHANNEL : get_next_active_channel(g_seed, g_hop_counter, g_channel_mask);
    g_current_channel = channel;
    mestre_radio.setChannel(channel);
    // Achado de bancada: sem esta espera, o PLL do NRF24 ainda nao
    // assentou na nova frequencia quando o write() seguinte tenta
    // transmitir -- ao contrario do pareamento (que so reescreve o
    // MESMO canal a cada tentativa, nunca troca de fato), aqui o
    // canal muda em TODO hop, às vezes por >100MHz de uma vez (ex.
    // 125->15 na tabela). Os 130us do datasheet (suficientes para o
    // pareamento e para o scan do analisador) não bastaram aqui —
    // 100% de perda mesmo com o link sincronizado; 500us reduziu bem
    // a perda nestes módulos.
    delayMicroseconds(500);
    // Hipotese ainda nao testada antes desta reescrita (ver "Correção" em
    // openspec/changes/adaptive-rendezvous/design.md): RX FIFO pode reter
    // payload do canal ANTERIOR apos setChannel() -- sem o flush, um ack
    // velho parado ali poderia ser lido por wait_fragment_ack() como se
    // fosse deste hop (msg_id/fragment_index poderiam coincidir, já que o
    // mesmo fragmento é reenviado em canais sucessivos até ser confirmado).
    mestre_radio.flush_rx();

    bool control_acked = false;
    if (send_pending_control_if_any(control_acked))
    {
        // Achado de bancada (2026-09-24): faltava isto -- sem atualizar
        // g_hops_since_ack aqui, um controle que falha pra sempre (ex.
        // escravo já não está mais lá) nunca acumulava falhas o bastante
        // pra disparar o fallback de LOSS_HOPS, e o mestre ficava preso
        // reenviando a mesma proposta de blacklist/seed indefinidamente
        // em vez de voltar a parear. Espelha o tratamento de ack/miss de
        // fragmento logo abaixo.
        if (control_acked)
            g_hops_since_ack = 0;
        else
            g_hops_since_ack++;
        g_hop_counter++;
        apply_scheduled_changes();
        if (g_hops_since_ack >= LOSS_HOPS)
            reset_to_pairing();
        return;
    }

    FragmentPacket pkt;
    fill_fragment(pkt, g_current_fragment);

    // Reenvia o mesmo fragmento a cada attempt_wait_ms até um dos dois:
    // ack chega (sai do while por "break"), ou HOP_TIMEOUT_MS deste
    // canal esgota (fallback abaixo, any_sent decide se foi por falha de
    // envio ou por falta de ack).
    unsigned long hop_window_end = millis() + HOP_TIMEOUT_MS;
    // scenario-automation: latência = tempo entre a primeira tentativa de
    // envio deste fragmento neste hop e o ack recebido (round-trip de
    // wait_fragment_ack, incluindo reenvios dentro do próprio hop).
    unsigned long attempt_started_at = millis();
    const uint32_t attempt_wait_ms = 25;
    bool ack_ok = false;
    bool completed = false;
    bool any_sent = false;

    while (millis() + attempt_wait_ms < hop_window_end)
    {
        mestre_radio.stopListening();
        bool sent = radio_send(mestre_radio, &pkt, sizeof(pkt));
        mestre_radio.startListening();

        if (sent)
        {
            any_sent = true;
            ack_ok = wait_fragment_ack(g_current_fragment, completed, attempt_wait_ms);
            if (ack_ok)
                break;
        }
    }

    if (!any_sent)
    {
        guarded_printf("[MESTRE] Falha de envio no hop=%lu canal=%u frag=%u\n", g_hop_counter, g_current_channel, g_current_fragment);
        g_hops_since_ack++;
        uint8_t idx = find_channel_index(channel);
        if (g_channel_fail[idx] < 255)
            g_channel_fail[idx]++;
        g_hop_counter++;
        apply_scheduled_changes();
        note_hop_for_throughput(false);
        if (g_hops_since_ack >= LOSS_HOPS)
            reset_to_pairing();
        return;
    }

    // Prints depois de já ter tentado o ack (não antes) -- ver
    // comentário histórico: eles competem pelo mutex de telemetria
    // com a task do analisador/jammer e não devem atrasar a janela
    // de escuta do ack.

    tlm_printf("mestre", "hop", "ch=%u;n=%lu;frag=%u", channel, g_hop_counter, g_current_fragment);
    guarded_printf("[MESTRE] Hop=%lu canal=%u frag=%u/%u len=%u enviada.\n",
                  g_hop_counter, g_current_channel, g_current_fragment + 1, g_total_fragments, pkt.payload_len);

    uint8_t idx = find_channel_index(channel);
    if (ack_ok)
    {
        g_hops_since_ack = 0;
        if (g_channel_fail[idx] > 0)
            g_channel_fail[idx]--;

        tlm_printf("mestre", "ack", "ch=%u;frag=%u", channel, g_current_fragment);
        guarded_printf("[MESTRE] ACK recebido para frag=%u\n", g_current_fragment + 1);
        uint32_t latency_ms = (uint32_t)(millis() - attempt_started_at);
        tlm_printf("mestre", "latency", "ch=%u;frag=%u;ms=%lu", channel, g_current_fragment, latency_ms);
        note_hop_for_throughput(true);
        g_current_fragment++;
        if (completed || g_current_fragment >= g_total_fragments)
        {
            guarded_println("[MESTRE] Mensagem completa confirmada pelo Escravo.");
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
        guarded_printf("[MESTRE] Timeout ACK no hop=%lu, frag=%u sera reenviada no proximo salto.\n", g_hop_counter, g_current_fragment + 1);
        note_hop_for_throughput(false);
    }

    g_hop_counter++;
    apply_scheduled_changes();

    if (g_hops_since_ack >= LOSS_HOPS)
        reset_to_pairing();
}

#endif // MESTRE_NODE_H
