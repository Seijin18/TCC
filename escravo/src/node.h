#ifndef ESCRAVO_NODE_H
#define ESCRAVO_NODE_H

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <printf.h>
#include <string.h>
#include "../../shared/hopping.h"
#include "../../shared/protocol.h"
#include "../../shared/pairing.h"
#include "../../shared/resync.h"
#include "../../shared/telemetry.h"
#include "../../shared/radio_diag.h"

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

// pairing-handshake-hardening: RF24Diag (shared/radio_diag.h) em vez de
// RF24 puro -- so acrescenta um leitor publico de registrador sobre o
// mesmo objeto, nenhuma mudanca de comportamento (ver comentario lá).
static RF24Diag escravo_radio(PIN_CE, PIN_CSN);

static const uint8_t MAX_FRAGMENTS = 16;
static const uint16_t MAX_MESSAGE_SIZE = FRAGMENT_DATA_SIZE * MAX_FRAGMENTS;

static char assembled_message[MAX_MESSAGE_SIZE + 1];
static bool received_bitmap[MAX_FRAGMENTS];
static uint8_t expected_total_fragments = 0;
static uint16_t current_msg_id = 0;
static uint8_t received_count = 0;
static bool completion_logged = false;

// ---- Estado de pareamento (pairing-handshake-hardening, D3) ----
// Substitui os bools link_established/g_seed_initialized do
// adaptive-rendezvous por uma maquina de estados explicita -- o handshake
// v2 (2 mensagens, sessao por nonce) tem uma fase a mais que um bool não
// descreve bem (pareado-provisorio, que pode durar varias sondas
// re-ackadas antes de confirmar dado de verdade):
//
//   SCANNING    -- varrendo PAIRING_CANDIDATES, nenhuma sessao adotada.
//   PROVISIONAL -- adotou {nonce, seed} de uma sonda valida, respondeu
//                  LinkAckV2, e esta esperando: (a) enquanto ainda nao
//                  chegou a hop_start_at, permanece no MESMO canal de
//                  pareamento respondendo re-sondas idempotentemente
//                  (mesmo nonce -> re-ack + rearma hop_start_at; nonce
//                  diferente -> adota nova sessao); (b) depois de
//                  hop_start_at, já está no loop de hopping (como
//                  CONFIRMED), só que ainda sem ter recebido nenhum
//                  FragmentPacket valido -- SEED_WAIT_TIMEOUT_MS contado
//                  a partir de hop_start_at e o prazo pra isso acontecer.
//   CONFIRMED   -- primeiro FragmentPacket valido recebido depois do
//                  inicio do salto -- enlace confirmado (notify_ready(true)
//                  já disparado); dali em diante e o loop de hopping normal,
//                  com LOSS_HOPS/reset_to_scanning() de sempre.
enum EscravoLinkState : uint8_t
{
    ESCRAVO_SCANNING = 0,
    ESCRAVO_PROVISIONAL = 1,
    ESCRAVO_CONFIRMED = 2,
};
static EscravoLinkState g_link_state = ESCRAVO_SCANNING;
static uint8_t g_pair_idx = 0;
static unsigned long g_pair_dwell_start = 0;
// Nonce da sessao de pareamento adotada (da ultima LinkProbeV2 valida
// aceita) -- usado por pair_decide_escravo() (shared/pairing.h) pra
// distinguir re-sonda da MESMA tentativa (re-ack) de uma sonda de uma
// tentativa NOVA do mestre (adota outra sessao).
static uint16_t g_session_nonce = 0;
// pairing-handshake-hardening (tarefa 3.7): comando de teste "drop_next_ack"
// -- descarta (nao transmite) o proximo LinkAckV2 que seria enviado, em vez
// de enviar, pra validar deterministicamente o caminho de re-ack por perda
// (tasks.md 5.2). Consumido (voltando a false) no primeiro ack que
// encontrar, adotado ou nao.
static bool g_drop_next_ack = false;

// pairing-handshake-hardening (tarefa 1.4): contadores do ciclo de
// varredura corrente (um ciclo = PAIRING_CANDIDATE_COUNT trocas de dwell),
// zerados a cada ciclo fechado -- ver escravo_loop(). Mesmo raciocínio do
// mestre (design.md H1/H4): rx_total=0 com o mestre relatando probes_ok>0
// no mesmo intervalo aponta pra rádio surdo (H4); rx_other>0 aponta pra
// pacote de formato/tipo inesperado chegando (H1, FIFO velha).
static uint8_t g_pair_cycle_dwells = 0;
static uint16_t g_pair_rx_total = 0;
static uint16_t g_pair_rx_probe = 0;
static uint16_t g_pair_rx_other = 0;
static uint16_t g_pair_acks_ok = 0;
static uint16_t g_pair_acks_fail = 0;
// pairing-handshake-hardening (tarefa 2.3): quantas trocas de dwell deste
// ciclo encontraram a FIFO de RX não-vazia antes do flush_rx() -- mede H1
// (design.md): FIFO velha atravessando a troca de canal de pareamento.
static uint16_t g_pair_flushed = 0;
// tarefa 1.5: linha humana só na primeira falha de LINK_ACK de cada ciclo.
static bool g_pair_ack_fail_reported = false;

static void reset_pair_cycle_counters()
{
    g_pair_cycle_dwells = 0;
    g_pair_rx_total = 0;
    g_pair_rx_probe = 0;
    g_pair_rx_other = 0;
    g_pair_acks_ok = 0;
    g_pair_acks_fail = 0;
    g_pair_flushed = 0;
    g_pair_ack_fail_reported = false;
}

// ---- Estado de hopping ----
static uint32_t hop_counter = 0;
// Hopping por evento: instante em que o escravo começou a escutar em
// current_channel -- não um relógio periódico. Ver escravo_advance_hop().
static unsigned long channel_deadline_start = 0;
static unsigned long hop_start_at = 0;
static uint8_t current_channel = PAIRING_CANDIDATES[0];
static uint32_t g_hops_since_rx = 0;

// ---- hop-resync: estacionamento (design.md D1/D2) ----
// Canal do ultimo FragmentPacket ou pacote de controle valido recebido --
// candidato a canal pra estacionar se a defasagem persistir (D1). So e lido
// depois de RESYNC_MISS_HOPS timeouts consecutivos, o que so acontece depois
// de pelo menos uma recepcao valida ja ter confirmado o enlace -- o valor
// inicial (primeiro candidato de pareamento) nunca chega a ser usado de
// verdade.
static uint8_t g_last_good_channel = PAIRING_CANDIDATES[0];
// Timeouts de hop CONSECUTIVOS sem nenhuma recepcao valida (fragmento ou
// controle) -- zerado por qualquer sucesso, em qualquer estado (parado ou
// nao). Ver resync_should_park() em shared/resync.h.
static uint8_t g_resync_miss_count = 0;
static bool g_resync_parked = false;
static unsigned long g_resync_park_start = 0;
// hop_counter do proprio escravo no instante em que entrou em PARKED --
// vira "hop_before" do evento resync_ok (a diferenca pro hop_after adotado
// mostra se o escravo estava adiantado ou atrasado, ver proposal.md).
static uint32_t g_resync_hop_before = 0;

// hop-resync (tarefa 1.5, achado de bancada 2026-09-24): qualidade de
// recepcao recente POR POSICAO na tabela (HOPPING_CHANNELS), nao por canal
// mais recente -- a 1a versao (D1) estacionava sempre em
// g_last_good_channel, mas no caso "ack perdido" esse "ultimo canal bom" E'
// justamente o canal ruim cujo ack se perdeu (a causa da propria
// defasagem): soak de bancada (tasks.md 2.1) deu 9 de 11 estacionamentos em
// ch 34 (colado no Wi-Fi) e 0 resync_ok, todos expirando. Contadores
// saturantes (nunca passam de 0xFF) com decaimento simples a cada
// RESYNC_QUALITY_DECAY_HOPS hops REAIS (nunca durante hops "virtuais" do
// estacionamento, ver escravo_advance_hop()) -- historico recente pesa mais
// que antigo (o Wi-Fi ambiente muda de canal com o tempo) sem precisar de
// float/EWMA de verdade. Indexados por posicao na tabela (nao pelo valor do
// canal) pra bater com o `mask`/`fallback_idx` que resync_pick_park_channel()
// (shared/resync.h) espera.
static uint8_t g_resync_channel_ok[TABLE_SIZE] = {0};
static uint8_t g_resync_channel_miss[TABLE_SIZE] = {0};
// hop-ack-decoupling: re-derivado de 64 para 450 (~7x) para preservar a
// janela de tempo de "historico recente" na cadencia de ~30 ms/hop. Como
// 450 > 255, o contador precisa ser uint16_t (era uint8_t e saturaria antes
// de alcancar o limiar).
static uint16_t g_resync_quality_hops = 0;
static const uint16_t RESYNC_QUALITY_DECAY_HOPS = 450;

// ---- Seed ----
// pairing-handshake-hardening: g_seed agora é adotado diretamente do campo
// `seed` da LinkProbeV2 aceita (ver g_link_state acima) -- o antigo
// g_seed_initialized (gate de "aguardando SeedPacket") foi removido junto
// com a troca de seed separada (D3, design.md).
static uint16_t g_seed = 0;

// ---- dynamic-seed-blacklist: máscara de canais ativos ----
static uint16_t g_channel_mask = 0xFFFF;
static bool g_seed_apply_scheduled = false;
static uint16_t g_seed_apply_value = 0;
static uint32_t g_seed_apply_at = 0;
static bool g_mask_apply_scheduled = false;
static uint16_t g_mask_apply_value = 0;
static uint32_t g_mask_apply_at = 0;

// scenario-automation: Cenário A (sem hopping) -- espelha "mode
// fixed|hopping" do mestre (mestre/src/node.h). Comando serial próprio,
// não pacote de protocolo: é configuração de bancada pelo operador (igual
// ao "mode sweep|targeted" do jammer), não uma decisão em runtime que
// precise ser negociada entre os dois lados.
static bool g_fixed_channel_mode = false;

// ---- Métricas de PLR (Packet Loss Rate) ----
// hop-ack-decoupling: re-derivado de 50 para 350 (~7x), par com
// THROUGHPUT_WINDOW_HOPS do mestre -- mesma janela de tempo de medicao na
// cadencia de ~30 ms/hop.
static const uint32_t g_window_size = 350; // hops por janela (~10,5s a ~30ms/hop sob jamming)
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

// pairing-handshake-hardening (tarefa 2.1): sequência de configuração do
// rádio extraída de escravo_setup() (D2, design.md) -- fonte única de
// verdade: setup() e toda (re)entrada no estado de escaneamento chamam
// exatamente este código, na mesma ordem. Ordem de registrador importa
// nestes clones de NRF24 -- dois bugs reais já vieram de configuração de
// feature/ack fora de ordem (ver comentário do enableAckPayload() abaixo),
// então isto só MOVE o que já existia em escravo_setup(), sem reordenar.
// Não inclui Serial/SPI.begin()/radio.begin()/pinMode/printDetails/boot --
// isso continua só em escravo_setup() (não faz sentido repetir a cada
// reentrada em escaneamento).
static void radio_reinit()
{
    // D2: power-cycle lógico -- força o rádio pra um estado conhecido
    // antes de reconfigurar. powerUp() (RF24 1.6.2, RF24.cpp) já espera
    // RF24_POWERUP_DELAY (5000us) internamente antes de retornar -- dentro
    // da janela de 1.5-5ms do datasheet (Tpd2stby) -- então nenhuma espera
    // adicional é necessária aqui.
    escravo_radio.powerDown();
    escravo_radio.powerUp();

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
    // D2: limpa os bits de IRQ de STATUS explicitamente antes de voltar a
    // escutar -- startListening() logo abaixo já faz isso sozinho (RF24
    // 1.6.2, RF24.cpp escreve STATUS antes de erguer CE), mas deixamos
    // explícito aqui pra não depender silenciosamente desse efeito
    // colateral se a lib mudar.
    escravo_radio.clearStatusFlags();
    escravo_radio.startListening();

    escravo_radio.flush_rx();
    escravo_radio.flush_tx();
}

static void reset_to_scanning()
{
    g_link_state = ESCRAVO_SCANNING;
    hop_counter = 0;
    g_hops_since_rx = 0;
    g_seed_apply_scheduled = false;
    g_mask_apply_scheduled = false;
    // jamming-mitigation-hardening: NÃO zerar a máscara aqui -- de qualquer
    // forma a LinkProbeV2 do mestre re-pareado traz a máscara persistida e o
    // escravo a adota; manter evita uma janela de discordância. (Se o mestre
    // for reiniciado limpo, sua sonda virá com 0xFFFF e o escravo adota.)
    // hop-resync: qualquer estacionamento em curso nao faz mais sentido
    // depois de voltar a escanear (o proximo estacionamento so pode
    // acontecer depois de um novo enlace confirmado).
    g_resync_parked = false;
    g_resync_miss_count = 0;
    reset_pair_cycle_counters();
    notify_ready(false);
    tlm_event("escravo", "loss_of_sync");
    // tarefa 1.2: dump de registradores toda vez que o escravo volta a
    // escanear -- compara contra o dump do boot pra confirmar/descartar
    // H4 (design.md).
    // pairing-handshake-hardening (tarefa 2.2): dump ANTES do
    // radio_reinit() (why=loss_pre) -- mesmo racional do lado do mestre
    // (ver reset_to_pairing() em mestre/src/node.h): sem isto o reinit
    // apagaria o estado de rádio que motivou o reset antes de registrá-lo.
    tlm_radio_regs("escravo", escravo_radio, "loss_pre");
    radio_reinit();
    tlm_radio_regs("escravo", escravo_radio, "loss");
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

    // pairing-handshake-hardening (tarefa 2.1): configuração do rádio
    // extraída para radio_reinit() (definida acima) -- setup() e toda
    // (re)entrada em escaneamento usam exatamente a mesma sequência, na
    // mesma ordem (fonte única de verdade, D2 em design.md).
    radio_reinit();

    guarded_println("\n--- STATUS DO RÁDIO ESCRAVO (REGISTRADORES) ---");
    escravo_radio.printDetails();
    guarded_println("-----------------------------------------------\n");

    g_pair_dwell_start = millis();
    guarded_printf("[ESCRAVO] Varrendo %u candidatos de pareamento...\n", PAIRING_CANDIDATE_COUNT);
    tlm_event("escravo", "boot");
    // tarefa 1.2: número-base de registradores no boot, pra comparar
    // contra os dumps de entrada em pareamento por perda (design.md H4).
    tlm_radio_regs("escravo", escravo_radio, "boot");
}

// Hopping por evento: chamado sempre que um "round" de escuta em
// current_channel termina -- por sucesso (fragmento/controle recebido e
// ackado, com `g_hop_received_flag`/`hop_counter` já atualizados pelo
// chamador) ou por ATTEMPT_TIMEOUT_MS esgotado sem nada válido (fallback).
// Nos dois casos o efeito é o mesmo: contabiliza PLR desta rodada, avança
// hop_counter em exatamente 1, escolhe e configura o próximo canal, e
// reinicia o deadline de escuta dele.
// scenario-automation: comando serial "mode fixed|hopping", espelhando
// handle_mestre_command() em mestre/src/node.h -- ver comentário em
// g_fixed_channel_mode acima sobre por que é comando local, não pacote.
static char g_escravo_cmd_buf[32];
static uint8_t g_escravo_cmd_len = 0;

// pairing-handshake-hardening: pedidos pendentes gravados por
// handle_escravo_command() para serem executados de fato dentro de
// escravo_loop() (ver process_escravo_pending_requests() e comentário na
// declaração de handle_escravo_command() logo abaixo) -- nunca inline
// dentro do próprio handle_escravo_command(). Na Placa B combinada,
// handle_escravo_command() roda na task do jammer (core 0) via
// jammer_set_unknown_cmd_sink(), enquanto escravo_loop() roda em loop()
// (core 1, Arduino): reset_to_scanning()/radio_reinit() e tlm_radio_regs()
// fazem leitura/escrita SPI sobre escravo_radio, e "mode fixed|hopping"
// mexe em g_fixed_channel_mode -- executar isso inline correria com
// escravo_loop() (barramento SPI e estado do escravo lidos/escritos por
// duas tasks ao mesmo tempo). Escrita de um único byte volatile é atômica
// no ESP32, então o padrão "produtor escreve, consumidor lê-e-limpa"
// dispensa mutex; quando mais de um campo precisa ficar consistente (valor
// do modo + flag), o valor é escrito ANTES da flag que o publica.
static volatile bool g_escravo_pending_reset = false;
static volatile bool g_escravo_pending_regs = false;
static volatile bool g_escravo_pending_mode_change = false;
static volatile bool g_escravo_pending_mode_value = false; // true=fixed, false=hopping
// pairing-handshake-hardening (tarefa 3.7): comando de teste "drop_next_ack"
// -- mesmo padrão de pedido pendente das flags acima.
static volatile bool g_escravo_pending_drop_next_ack = false;

// Retorna true se o comando foi reconhecido -- usado tanto por
// process_escravo_serial() (escravo standalone, lê a própria Serial)
// quanto como sink de jammer_set_unknown_cmd_sink() na placa combinada
// (ver comentário em g_fixed_channel_mode acima e em jammer/src/node.h:
// só o jammer lê Serial fisicamente ali, pra não correr com este leitor).
//
// pairing-handshake-hardening: só GRAVA o pedido (flags acima) e retorna --
// a ação de fato (reset_to_scanning(), tlm_radio_regs(), troca de modo)
// acontece em process_escravo_pending_requests(), chamada do início de
// escravo_loop(), sempre na task do escravo. Ver comentário nas
// declarações das flags acima sobre por quê.
static bool handle_escravo_command(const char *raw_line)
{
    const char *line = raw_line;
    while (*line == ' ' || *line == '\t')
        line++;

    if (strcmp(line, "mode fixed") == 0)
    {
        g_escravo_pending_mode_value = true;
        g_escravo_pending_mode_change = true;
        return true;
    }
    if (strcmp(line, "mode hopping") == 0)
    {
        g_escravo_pending_mode_value = false;
        g_escravo_pending_mode_change = true;
        return true;
    }
    if (strcmp(line, "reset") == 0)
    {
        g_escravo_pending_reset = true;
        return true;
    }
    if (strcmp(line, "regs") == 0)
    {
        // pairing-handshake-hardening (tarefa 1.2): dump de registradores
        // sob demanda, sem mudar nenhum estado. Na placa combinada
        // (placa_b), chega aqui via jammer_set_unknown_cmd_sink() -- ver
        // comentário acima em handle_escravo_command().
        g_escravo_pending_regs = true;
        return true;
    }
    if (strcmp(line, "drop_next_ack") == 0)
    {
        // pairing-handshake-hardening (tarefa 3.7): arma o descarte do
        // próximo LinkAckV2 (validação determinística de 5.2 -- re-ack por
        // perda de resposta).
        g_escravo_pending_drop_next_ack = true;
        return true;
    }
    return false;
}

// pairing-handshake-hardening: consome as flags gravadas por
// handle_escravo_command() (possivelmente de outra task -- ver comentário
// acima) e executa a ação de verdade aqui, sempre dentro da task do
// escravo. Chamada do início de escravo_loop(), logo após
// process_escravo_serial().
static void process_escravo_pending_requests()
{
    if (g_escravo_pending_mode_change)
    {
        g_escravo_pending_mode_change = false;
        g_fixed_channel_mode = g_escravo_pending_mode_value;
        if (g_fixed_channel_mode)
        {
            tlm_event("escravo", "mode_fixed");
            guarded_printf("[ESCRAVO] Modo fixo: canal travado em %u\n", TEST_FIXED_CHANNEL);
        }
        else
        {
            tlm_event("escravo", "mode_hopping");
            guarded_println("[ESCRAVO] Modo hopping normal restaurado.");
        }
    }
    if (g_escravo_pending_reset)
    {
        g_escravo_pending_reset = false;
        guarded_println("[ESCRAVO] Reset solicitado via comando -- voltando ao escaneamento.");
        // jamming-mitigation-hardening: reset LIMPO por comando zera a máscara
        // (o mestre re-pareado mandará 0xFFFF na sonda de qualquer forma, mas
        // manter consistente evita janela de discordância). Reset por jamming
        // (reset_to_scanning direto) preserva.
        g_channel_mask = 0xFFFF;
        reset_to_scanning();
    }
    if (g_escravo_pending_drop_next_ack)
    {
        g_escravo_pending_drop_next_ack = false;
        g_drop_next_ack = true;
        guarded_println("[ESCRAVO] drop_next_ack armado -- proximo LinkAckV2 sera descartado.");
    }
    if (g_escravo_pending_regs)
    {
        g_escravo_pending_regs = false;
        tlm_radio_regs("escravo", escravo_radio, "cmd");
        guarded_println("[ESCRAVO] Registradores do radio despejados (e=regs;why=cmd).");
    }
}

static void process_escravo_serial()
{
    // Na placa combinada (g_escravo_ready_sink registrado por
    // placa_b/src/main.cpp), o jammer é o único a ler Serial fisicamente
    // e repassa comandos não reconhecidos pra handle_escravo_command()
    // via jammer_set_unknown_cmd_sink() -- ler aqui também correria com
    // aquela leitura na mesma UART. Standalone (sem sink), o escravo lê
    // normalmente.
    if (g_escravo_ready_sink)
        return;

    while (Serial.available())
    {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r')
        {
            if (g_escravo_cmd_len > 0)
            {
                g_escravo_cmd_buf[g_escravo_cmd_len] = '\0';
                if (!handle_escravo_command(g_escravo_cmd_buf))
                    guarded_printf("[ESCRAVO] Comando desconhecido: %s\n", g_escravo_cmd_buf);
                g_escravo_cmd_len = 0;
            }
            continue;
        }
        if (g_escravo_cmd_len < sizeof(g_escravo_cmd_buf) - 1)
            g_escravo_cmd_buf[g_escravo_cmd_len++] = c;
        else
            g_escravo_cmd_len = 0;
    }
}

// hop-resync (tarefa 1.5): posicao de `channel` em HOPPING_CHANNELS -- usada
// pra indexar g_resync_channel_ok/miss por posicao na tabela (nao pelo valor
// do canal). HOPPING_CHANNELS nao tem valores repetidos, entao a busca acha
// exatamente um indice pra qualquer canal que ja veio dali
// (current_channel/g_last_good_channel sempre vieram de
// get_next_active_channel() ou de HOPPING_CHANNELS diretamente).
static uint8_t channel_table_index(uint8_t channel)
{
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
    {
        if (HOPPING_CHANNELS[i] == channel)
            return i;
    }
    return 0; // nao deveria acontecer -- guarda defensiva
}

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

    // hop-resync (tarefa 1.5): atualiza a qualidade de recepcao do canal QUE
    // ACABOU DE TERMINAR (current_channel, antes de trocar mais abaixo) --
    // ok se recebeu algo valido, miss se estourou por timeout. So conta
    // hops REAIS -- esta funcao nunca roda pros hops "virtuais" do
    // estacionamento (ver ramo ESTACIONADO em escravo_loop()), entao a
    // amostra reflete de verdade o comportamento do canal, nao o tempo que
    // o escravo passou parado nele.
    uint8_t quality_idx = channel_table_index(current_channel);
    if (g_hop_received_flag)
    {
        if (g_resync_channel_ok[quality_idx] < 0xFF)
            g_resync_channel_ok[quality_idx]++;
    }
    else
    {
        if (g_resync_channel_miss[quality_idx] < 0xFF)
            g_resync_channel_miss[quality_idx]++;
    }
    g_resync_quality_hops++;
    if (g_resync_quality_hops >= RESYNC_QUALITY_DECAY_HOPS)
    {
        // Decaimento simples e deterministico: divide os dois contadores de
        // TODOS os canais por 2 a cada RESYNC_QUALITY_DECAY_HOPS hops reais
        // -- historico recente pesa mais que antigo, sem precisar de
        // float/EWMA de verdade (ver comentario na declaracao dos arrays).
        for (uint8_t i = 0; i < TABLE_SIZE; i++)
        {
            g_resync_channel_ok[i] >>= 1;
            g_resync_channel_miss[i] >>= 1;
        }
        g_resync_quality_hops = 0;
    }

    g_hop_received_flag = false;

    if (g_window_expected >= g_window_size)
    {
        log_plr_window();
        g_window_expected = 0;
        g_window_received = 0;
    }

    hop_counter++;
    // scenario-automation, Cenário A: espelha o modo fixo do mestre (ver
    // mestre/src/node.h) -- mesmo TEST_FIXED_CHANNEL literal dos dois
    // lados, sem depender de seed/mascara.
    current_channel = g_fixed_channel_mode ? TEST_FIXED_CHANNEL : get_next_active_channel(g_seed, hop_counter, g_channel_mask);
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

// jamming-mitigation-hardening: canal (posição na tabela) que o escravo
// suspeita jammed -- muitos misses recentes E nenhum sucesso recente, entre os
// canais ainda ativos na máscara. É a evidência LIMPA que o mestre não tem
// (o mestre confunde jamming com dessincronismo). Reportado no FragmentAck.
static const uint8_t SUSPECT_MISS_MIN = 2;
static uint8_t escravo_suspect_channel()
{
    int best = -1;
    uint8_t best_miss = SUSPECT_MISS_MIN;
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
    {
        if ((g_channel_mask & (1u << i)) == 0)
            continue; // já banido -- não re-reportar
        if (g_resync_channel_ok[i] == 0 && g_resync_channel_miss[i] >= best_miss)
        {
            best_miss = g_resync_channel_miss[i];
            best = i;
        }
    }
    return best < 0 ? SUSPECT_NONE : (uint8_t)best;
}

// pairing-handshake-hardening (D3, tarefa 3.5): monta e envia o LinkAckV2
// que ecoa {nonce, seed} da sonda recebida -- usado tanto na primeira sonda
// válida (SCANNING -> PROVISIONAL) quanto no re-ack idempotente (mesmo
// nonce, ainda PROVISIONAL). H6 (achado de bancada, tasks.md 1.8): o ack
// sai daqui SEM nenhum print/telemetria no meio -- só depois de chamar isto
// o chamador pode logar (guarded_printf/tlm_printf competem pelo mutex de
// telemetria com o CSV do analisador na placa combinada e podem atrasar o
// suficiente pra perder a janela de espera do mestre).
static bool escravo_send_ack_for(const LinkProbeV2 &probe, bool &dropped_for_test)
{
    dropped_for_test = false;

    // tarefa 3.7: comando de teste "drop_next_ack" -- descarta em vez de
    // transmitir, consumido de uma vez só.
    if (g_drop_next_ack)
    {
        g_drop_next_ack = false;
        dropped_for_test = true;
        return false;
    }

    LinkAckV2 ack = {MSG_LINK_ACK, PAIRING_PROTO_VER, probe.nonce, probe.seed, probe.mask};
    escravo_radio.stopListening();
    // radio_send() em vez de write(): ver shared/protocol.h (bug de
    // retry-forever em RF24::write() nesta versao da lib) -- o mestre pode
    // nao estar pronto pra auto-ack no exato instante, e a confirmacao real
    // vem do LinkAckV2 que o mestre le, nao do ack de hardware.
    bool sent = radio_send(escravo_radio, &ack, sizeof(ack));
    escravo_radio.startListening();
    return sent;
}

static void escravo_loop()
{
    process_escravo_serial();
    process_escravo_pending_requests();

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

    if (g_link_state == ESCRAVO_SCANNING)
    {
        if (millis() - g_pair_dwell_start >= PAIR_DWELL_MS)
        {
            g_pair_idx = (uint8_t)((g_pair_idx + 1) % PAIRING_CANDIDATE_COUNT);
            g_pair_dwell_start = millis();
            escravo_radio.setChannel(PAIRING_CANDIDATES[g_pair_idx]);
            // pairing-handshake-hardening (tarefa 2.3): descarta qualquer
            // pacote que tenha caído na FIFO enquanto escutava o canal
            // anterior, antes de escutar o novo (D2, design.md) -- mesmo
            // racional do lado do mestre (ver pick_new_pair_channel() em
            // mestre/src/node.h); conta se havia algo pra descartar (mede
            // H1: FIFO velha atravessando troca de canal).
            if (escravo_radio.available())
                g_pair_flushed++;
            escravo_radio.flush_rx();
            tlm_printf("escravo", "pair_scan", "ch=%u", PAIRING_CANDIDATES[g_pair_idx]);

            // tarefa 1.4: um ciclo completo de varredura = passar pelos
            // PAIRING_CANDIDATE_COUNT candidatos uma vez -- emite e zera os
            // contadores a cada ciclo fechado.
            g_pair_cycle_dwells++;
            if (g_pair_cycle_dwells >= PAIRING_CANDIDATE_COUNT)
            {
                tlm_printf("escravo", "pair_stats",
                           "rx_total=%u;rx_probe=%u;rx_other=%u;acks_ok=%u;acks_fail=%u;flushed=%u",
                           g_pair_rx_total, g_pair_rx_probe, g_pair_rx_other, g_pair_acks_ok, g_pair_acks_fail,
                           g_pair_flushed);
                reset_pair_cycle_counters();
            }
        }

        uint8_t pipe;
        if (escravo_radio.available(&pipe))
        {
            g_pair_rx_total++;
            uint8_t size = escravo_radio.getDynamicPayloadSize();
            if (size == sizeof(LinkProbeV2))
            {
                LinkProbeV2 probe;
                escravo_radio.read(&probe, sizeof(probe));
                if (probe.type == MSG_LINK_PROBE)
                {
                    g_pair_rx_probe++;
                    // provisional=false: escravo ainda escaneando -- toda
                    // sonda válida (ver PAIRING_PROTO_VER) inicia uma sessão
                    // nova (pair_decide_escravo() nunca devolve REACK aqui,
                    // ver shared/pairing.h).
                    PairDecision decision = pair_decide_escravo(false, 0, probe.ver, probe.nonce);
                    if (decision == PAIR_IGNORE)
                    {
                        // D3: versão de protocolo diferente -- não responde,
                        // só loga (ver spec "Handshake identificado por
                        // sessão").
                        tlm_printf("escravo", "pair_ver_mismatch", "ver=%u;esperado=%u", probe.ver, PAIRING_PROTO_VER);
                    }
                    else
                    {
                        g_session_nonce = probe.nonce;
                        g_seed = probe.seed;
                        g_channel_mask = probe.mask; // jamming-mitigation-hardening: adota a blacklist persistida

                        bool dropped_for_test = false;
                        bool sent = escravo_send_ack_for(probe, dropped_for_test);

                        g_link_state = ESCRAVO_PROVISIONAL;
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

                        if (!sent)
                        {
                            g_pair_acks_fail++;
                            if (dropped_for_test)
                                guarded_printf("[ESCRAVO] drop_next_ack: LinkAckV2 descartado de proposito (nonce=%u).\n", probe.nonce);
                            else if (!g_pair_ack_fail_reported)
                            {
                                g_pair_ack_fail_reported = true;
                                guarded_printf("[ESCRAVO] Falha ao enviar LinkAckV2 em ch=%u (radio_send).\n", PAIRING_CANDIDATES[g_pair_idx]);
                            }
                        }
                        else
                        {
                            g_pair_acks_ok++;
                        }

                        guarded_printf("[ESCRAVO] Pareamento confirmado em ch=%u nonce=%u. Aguardando dado...\n",
                                      PAIRING_CANDIDATES[g_pair_idx], probe.nonce);
                        tlm_printf("escravo", "pair_ok", "ch=%u;nonce=%u", PAIRING_CANDIDATES[g_pair_idx], probe.nonce);
                    }
                }
                else
                {
                    // Mesmo tamanho de LinkProbeV2, mas tipo diferente de
                    // MSG_LINK_PROBE -- pacote "de outro assunto" (H1).
                    g_pair_rx_other++;
                }
            }
            else
            {
                uint8_t dump[32];
                escravo_radio.read(dump, size);
                g_pair_rx_other++;
            }
        }

        return;
    }

    // D3 (design.md, "Resposta perdida é recuperada por re-ack"): enquanto
    // pareado-provisório e AINDA no canal de pareamento (não chegou
    // hop_start_at), o escravo só espera por re-sondas -- nenhum
    // FragmentPacket é esperado nesta janela (o mestre só começa a saltar
    // depois de aceitar o ack, e só aí agenda o próprio hop_start_at).
    if (g_link_state == ESCRAVO_PROVISIONAL && millis() < hop_start_at)
    {
        uint8_t pipe;
        if (escravo_radio.available(&pipe))
        {
            uint8_t size = escravo_radio.getDynamicPayloadSize();
            if (size == sizeof(LinkProbeV2))
            {
                LinkProbeV2 probe;
                escravo_radio.read(&probe, sizeof(probe));
                if (probe.type == MSG_LINK_PROBE)
                {
                    PairDecision decision = pair_decide_escravo(true, g_session_nonce, probe.ver, probe.nonce);
                    if (decision == PAIR_IGNORE)
                    {
                        tlm_printf("escravo", "pair_ver_mismatch", "ver=%u;esperado=%u", probe.ver, PAIRING_PROTO_VER);
                    }
                    else
                    {
                        bool is_new_session = (decision == PAIR_ADOPT_NEW);
                        g_session_nonce = probe.nonce;
                        g_seed = probe.seed;
                        g_channel_mask = probe.mask; // jamming-mitigation-hardening: adota a blacklist persistida

                        bool dropped_for_test = false;
                        bool sent = escravo_send_ack_for(probe, dropped_for_test);

                        // Riscos, design.md: "o último ack entregue define o
                        // alinhamento" -- rearma hop_start_at a cada sonda
                        // válida respondida (mesma sessão ou nova), pra o
                        // mestre e o escravo começarem o salto alinhados
                        // mesmo que um ack anterior tenha se perdido.
                        hop_start_at = millis() + HOP_START_DELAY_MS;
                        channel_deadline_start = hop_start_at;
                        hop_counter = 0;
                        g_hops_since_rx = 0;

                        if (!sent)
                        {
                            if (dropped_for_test)
                                guarded_printf("[ESCRAVO] drop_next_ack: LinkAckV2 descartado de proposito (nonce=%u).\n", probe.nonce);
                            else if (!g_pair_ack_fail_reported)
                            {
                                g_pair_ack_fail_reported = true;
                                guarded_printf("[ESCRAVO] Falha ao enviar LinkAckV2 em ch=%u (radio_send).\n", PAIRING_CANDIDATES[g_pair_idx]);
                            }
                        }

                        tlm_printf("escravo", is_new_session ? "pair_ok" : "pair_reack",
                                   "ch=%u;nonce=%u", PAIRING_CANDIDATES[g_pair_idx], probe.nonce);
                    }
                }
                else
                {
                    uint8_t dump[32];
                    escravo_radio.read(dump, size);
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

    // A partir daqui, hop_start_at já passou (ver os dois `return` acima) --
    // g_link_state é PROVISIONAL (aguardando o primeiro FragmentPacket
    // válido pra confirmar) ou CONFIRMED (hopping normal).
    if (g_link_state == ESCRAVO_PROVISIONAL && (millis() - hop_start_at) >= SEED_WAIT_TIMEOUT_MS)
    {
        // D3 (design.md, "Enlace provisório sem dados volta à varredura"):
        // nenhum FragmentPacket válido chegou a tempo depois do início do
        // salto -- mesmo caminho de volta a escanear de sempre (radio_reinit()
        // incluso, D2).
        guarded_println("[ESCRAVO] Nenhum fragmento valido a tempo -- voltando a escanear.");
        tlm_event("escravo", "pair_timeout");
        g_link_state = ESCRAVO_SCANNING;
        g_pair_dwell_start = millis();
        reset_pair_cycle_counters();
        // tarefa 1.2: dump de registradores neste caminho também --
        // achado de bancada E1 (handshake meio-aberto) em design.md
        // passa exatamente por aqui do lado do escravo.
        // pairing-handshake-hardening (tarefa 2.2): dump pré-reinit
        // (why=pair_timeout_pre) antes do radio_reinit() -- mesmo
        // racional do reset_to_scanning() acima.
        tlm_radio_regs("escravo", escravo_radio, "pair_timeout_pre");
        radio_reinit();
        tlm_radio_regs("escravo", escravo_radio, "pair_timeout");
        return;
    }

    // Hopping por evento, não por relógio periódico (ver "Correção" em
    // openspec/changes/adaptive-rendezvous/design.md e mestre_loop() em
    // mestre/src/node.h): current_channel só muda quando (a) um
    // fragmento/pacote de controle válido é recebido e ackado -- evento
    // de sucesso, ver as chamadas a escravo_advance_hop() dentro do bloco
    // de recepção abaixo, que resincronizam hop_counter a partir do valor
    // recebido do mestre ANTES de avançar -- ou (b) ATTEMPT_TIMEOUT_MS esgota
    // sem nada válido chegar neste canal (fallback logo abaixo: avança
    // mesmo assim, senão o escravo ficaria preso pra sempre esperando num
    // canal jamado).
    //
    // hop-resync (tarefa 1.2, design.md D1/D2): o timeout se divide em dois
    // casos -- ESTACIONADO (g_resync_parked) só contabiliza um hop "virtual"
    // por tempo, sem trocar de canal nem chamar escravo_advance_hop(); NÃO
    // ESTACIONADO é o caminho de sempre, contando timeouts consecutivos pra
    // decidir se deve estacionar.
    //
    // hop-resync (tarefa 1.8, bug de bancada): `timed_out` é calculado UMA
    // vez só e testado nos três ramos abaixo (`timed_out && g_resync_parked`,
    // depois `timed_out` sozinho, depois o `else` final de recepção) -- e
    // não mais um `if (g_resync_parked) { if (timed_out) {...} } else if
    // (timed_out) {...} else {...}` aninhado. Nessa versão antiga, com
    // g_resync_parked==true e timed_out==false (o caso comum, a cada
    // iteração até o timeout estourar), nenhum dos dois `if` internos
    // disparava e o `else` de recepção (available()/read, mais abaixo)
    // JAMAIS rodava -- um escravo estacionado nunca lia o rádio (achado de
    // bancada: 0 resync_ok em dezenas de estacionamentos, mesmo com os dois
    // rádios reinicializados). O comentário antigo aqui já afirmava
    // (incorretamente) que o `else` de recepção rodava nos dois casos; com
    // `timed_out` único isso finalmente é verdade.
    // hop-ack-decoupling: janela de escuta por hop = ESCRAVO_LISTEN_MS (~60 ms),
    // MAIOR que a janela do mestre (ATTEMPT_TIMEOUT_MS ~30 ms) de proposito --
    // absorve o lead de fase do mestre (telemetria pesada apos o ack) sem
    // atrasar o caminho de sucesso, que avanca por EVENTO (no RX valido, adota
    // o hop_counter do pacote e o received_bitmap deduplica retransmissoes,
    // reancorando a fase). Ver comentario de ESCRAVO_LISTEN_MS em protocol.h.
    bool timed_out = (millis() - channel_deadline_start >= ESCRAVO_LISTEN_MS);
    if (timed_out && g_resync_parked)
    {
        // D2: nada válido chegou nesta janela -- conta 1 hop "virtual"
        // pra PLR/g_hops_since_rx continuarem honestos, mas SEM chamar
        // escravo_advance_hop() (que trocaria de canal): o escravo
        // continua parado em current_channel (o canal escolhido por
        // qualidade na entrada do estacionamento, tarefa 1.5 -- não é
        // sempre g_last_good_channel) até um pacote válido chegar ou o
        // orçamento esgotar.
        g_window_expected++;
        g_session_expected++;
        g_hops_since_rx++;
        if (g_window_expected >= g_window_size)
        {
            log_plr_window();
            g_window_expected = 0;
            g_window_received = 0;
        }
        channel_deadline_start = millis();

        if (resync_park_expired((uint32_t)(millis() - g_resync_park_start), RESYNC_PARK_MAX_MS) ||
            g_hops_since_rx >= LOSS_HOPS)
        {
            // Orçamento de estacionamento esgotado sem nada válido --
            // mesmo caminho de perda de sempre (o static_assert do
            // orçamento em shared/protocol.h garante que
            // RESYNC_PARK_MAX_MS sempre vence antes de LOSS_HOPS; a
            // checagem de g_hops_since_rx fica por simetria/defesa).
            reset_to_scanning();
            return;
        }
    }
    else if (timed_out)
    {
        escravo_advance_hop();
        g_resync_miss_count++;
        if (resync_should_park(g_resync_miss_count, RESYNC_MISS_HOPS))
        {
            // D1: RESYNC_MISS_HOPS timeouts consecutivos sem nada válido --
            // para de avançar por conta própria e estaciona, escutando
            // continuamente até o mestre passar por ali de novo
            // (round-robin sobre a máscara ativa, no máximo active_count
            // hops, ver get_next_active_channel() em shared/hopping.h) ou o
            // orçamento (D2) esgotar.
            g_resync_parked = true;
            g_resync_hop_before = hop_counter;
            g_resync_park_start = millis();

            // hop-resync (tarefa 1.5, achado de bancada): NÃO estaciona mais
            // cegamente em g_last_good_channel -- no caso "ack perdido"
            // (proposal.md "Mecanismo"), a ÚLTIMA recepção válida acontece
            // exatamente no canal ruim cujo ack se perdeu (é a causa da
            // própria defasagem), então repetir esse canal repete a falha
            // (soak de bancada, tasks.md 2.1: 9 de 11 estacionamentos em ch
            // 34, colado no Wi-Fi, 0 resync_ok). Escolhe em vez disso, entre
            // os canais habilitados, o de melhor qualidade de recepção
            // recente (g_resync_channel_ok/miss, ver shared/resync.h) --
            // cai de volta em g_last_good_channel só se nenhum canal
            // habilitado tiver recepção alguma registrada.
            uint8_t fallback_idx = channel_table_index(g_last_good_channel);
            uint8_t park_idx = resync_pick_park_channel(g_resync_channel_ok, g_resync_channel_miss,
                                                         g_channel_mask, fallback_idx);
            // Mesma fórmula de score de resync_pick_park_channel() --
            // recalculada aqui só pra telemetria (a função devolve só o
            // índice escolhido).
            int16_t park_score = (int16_t)g_resync_channel_ok[park_idx] - 2 * (int16_t)g_resync_channel_miss[park_idx];

            current_channel = HOPPING_CHANNELS[park_idx];

            // hop-resync (tarefa 1.8): entrada no estacionamento -- mesmo
            // settle/flush de escravo_advance_hop() após setChannel() (RX
            // FIFO pode reter payload do canal anterior -- ver comentário
            // lá). Sem startListening() extra: o rádio já está escutando
            // (escravo_advance_hop() faz o mesmo, sem reafirmar CE/PRIM_RX).
            // As variantes "listen"/"reinit" testadas aqui (tasks.md 1.6)
            // eram bancada em cima de um diagnóstico errado -- a causa real
            // da falta de resync_ok era o bug de 1.8 (escravo estacionado
            // nunca lia o rádio, ver comentário em `timed_out` acima); com
            // ele corrigido, nenhuma das duas variantes fez falta (soak:
            // 49/49 resync_ok).
            escravo_radio.setChannel(current_channel);
            delayMicroseconds(500);
            escravo_radio.flush_rx();

            channel_deadline_start = millis();
            tlm_printf("escravo", "resync_park", "ch=%u;hop=%lu;score=%d",
                       current_channel, hop_counter, park_score);
        }
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

                        // hop-resync (tarefa 1.2): qualquer recepção válida
                        // zera a contagem de timeouts consecutivos e marca
                        // este canal como o "último bom" -- candidato a
                        // estacionar se a defasagem persistir de novo. Se o
                        // escravo estava estacionado, sai do estacionamento
                        // aqui (o resync_ok é logado mais abaixo, DEPOIS do
                        // ack -- ver comentário do achado de bancada logo
                        // abaixo).
                        g_resync_miss_count = 0;
                        g_last_good_channel = current_channel;
                        bool frag_was_parked = g_resync_parked;
                        g_resync_parked = false;

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
                        ack.suspect_idx = escravo_suspect_channel(); // jamming-mitigation-hardening

                        escravo_radio.stopListening();
                        radio_send(escravo_radio, &ack, sizeof(ack));
                        escravo_radio.startListening();

                        // Evento de sucesso: avança pro próximo canal já
                        // agora, sem esperar timeout nenhum -- ver
                        // escravo_advance_hop().
                        escravo_advance_hop();

                        // D3 (design.md, "primeiro FragmentPacket válido
                        // confirma o enlace"): só agora, com dado de
                        // verdade recebido (não só o handshake), o enlace
                        // sai de pareado-provisório -- notify_ready(true)
                        // (libera o jammer combinado) só dispara aqui, nunca
                        // antes.
                        bool just_confirmed = (g_link_state == ESCRAVO_PROVISIONAL);
                        if (just_confirmed)
                        {
                            g_link_state = ESCRAVO_CONFIRMED;
                            reset_plr_counters();
                        }

                        // Prints adiados pra depois do ack (ver comentário
                        // acima) -- não afetam o timing crítico daqui em diante.
                        if (just_confirmed)
                        {
                            tlm_printf("escravo", "pair_confirm", "ch=%u", current_channel);
                            guarded_printf("[ESCRAVO] Enlace confirmado (primeiro fragmento recebido) em ch=%u.\n", current_channel);
                            notify_ready(true);
                        }
                        if (frag_was_parked)
                        {
                            // hop-resync (tarefa 1.3): loga DEPOIS do ack (H6,
                            // ver tasks.md de pairing-handshake-hardening
                            // 1.8) -- hop_before é o contador do próprio
                            // escravo no instante em que estacionou,
                            // hop_after é o que acabou de ser adotado deste
                            // fragmento (a diferença mostra se o escravo
                            // estava adiantado ou atrasado, ver proposal.md).
                            tlm_printf("escravo", "resync_ok", "ch=%u;hop_before=%lu;hop_after=%lu;ms=%lu",
                                       g_last_good_channel, g_resync_hop_before, pkt.hop_counter,
                                       (unsigned long)(millis() - g_resync_park_start));
                        }
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

                        // hop-resync (tarefa 1.2a, design.md D3a): adota o
                        // contador de hop do mestre também a partir de
                        // pacote de controle -- mesma correção de deriva que
                        // o FragmentPacket já faz (ver ramo MSG_FRAGMENT
                        // acima). Necessário porque, enquanto há controle
                        // pendente, o mestre manda o pacote de controle NO
                        // LUGAR do fragmento em todo hop até ser ackado (ver
                        // send_pending_control_if_any() em mestre/src/node.h)
                        // -- um escravo estacionado pode nunca ver um
                        // FragmentPacket, então sem isto não teria de onde
                        // sair do estacionamento.
                        hop_counter = pkt.hop_counter;
                        g_resync_miss_count = 0;
                        g_last_good_channel = current_channel;
                        bool seed_was_parked = g_resync_parked;
                        g_resync_parked = false;

                        g_seed_apply_scheduled = true;
                        g_seed_apply_value = pkt.new_seed;
                        g_seed_apply_at = pkt.apply_at_hop;

                        ControlAck ack = {MSG_CTRL_ACK, pkt.apply_at_hop};
                        escravo_radio.stopListening();
                        radio_send(escravo_radio, &ack, sizeof(ack));
                        escravo_radio.startListening();
                        tlm_printf("escravo", "rekey_ack", "kind=seed;seed=%u;apply=%lu", pkt.new_seed, pkt.apply_at_hop);
                        // Achado de bancada (2026-09-24): faltava esta linha
                        // -- sem ela, escravo_advance_hop() logo abaixo conta
                        // este hop como PERDA (g_hop_received_flag continua
                        // false), mesmo o pacote de controle tendo sido
                        // recebido e ackado com sucesso. Sob rajada de
                        // controle (vários MUDAR_SEED/BLACKLIST_UPDATE em
                        // sequência, exatamente o que acontece sob jamming
                        // real ou reavaliação contínua de blacklist-decay),
                        // isso inflava g_hops_since_rx até LOSS_HOPS e
                        // derrubava um enlace que na verdade estava
                        // funcionando -- provável causa raiz do "link nunca
                        // se recupera" investigado em dynamic-seed-blacklist
                        // tasks.md 4.4.
                        g_hop_received_flag = true;
                        // Mesmo caminho de evento-sucesso do fragmento: o
                        // mestre também avança o dele imediatamente após
                        // enviar um pacote de controle (ver mestre_loop()).
                        escravo_advance_hop();
                        if (seed_was_parked)
                        {
                            // hop-resync (tarefa 1.3): loga DEPOIS do ack,
                            // mesma regra do ramo MSG_FRAGMENT acima (H6).
                            tlm_printf("escravo", "resync_ok", "ch=%u;hop_before=%lu;hop_after=%lu;ms=%lu",
                                       g_last_good_channel, g_resync_hop_before, pkt.hop_counter,
                                       (unsigned long)(millis() - g_resync_park_start));
                        }
                    }
                    else if (ctrl_type == MSG_CTRL_BLACKLIST)
                    {
                        ControlBlacklistPacket pkt;
                        memcpy(&pkt, raw, sizeof(pkt));

                        // hop-resync (tarefa 1.2a): mesma adoção do
                        // hop_counter do mestre que o ramo MSG_CTRL_SEED
                        // acima -- ver comentário lá.
                        hop_counter = pkt.hop_counter;
                        g_resync_miss_count = 0;
                        g_last_good_channel = current_channel;
                        bool blacklist_was_parked = g_resync_parked;
                        g_resync_parked = false;

                        g_mask_apply_scheduled = true;
                        g_mask_apply_value = pkt.mask;
                        g_mask_apply_at = pkt.apply_at_hop;

                        ControlAck ack = {MSG_CTRL_ACK, pkt.apply_at_hop};
                        escravo_radio.stopListening();
                        radio_send(escravo_radio, &ack, sizeof(ack));
                        escravo_radio.startListening();
                        tlm_printf("escravo", "rekey_ack", "kind=blacklist;mask=0x%04X;apply=%lu", pkt.mask, pkt.apply_at_hop);
                        // Mesmo achado do ramo MSG_CTRL_SEED acima.
                        g_hop_received_flag = true;
                        escravo_advance_hop();
                        if (blacklist_was_parked)
                        {
                            // hop-resync (tarefa 1.3): loga DEPOIS do ack
                            // (H6), mesma regra dos ramos acima.
                            tlm_printf("escravo", "resync_ok", "ch=%u;hop_before=%lu;hop_after=%lu;ms=%lu",
                                       g_last_good_channel, g_resync_hop_before, pkt.hop_counter,
                                       (unsigned long)(millis() - g_resync_park_start));
                        }
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
