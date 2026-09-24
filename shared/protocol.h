#ifndef PROTOCOL_H
#define PROTOCOL_H

// adaptive-rendezvous: extrai o que antes estava duplicado em
// mestre/src/node.h e escravo/src/node.h (MessageType, tamanhos de pacote,
// enderecos) para uma unica fonte -- pre-requisito para os pacotes de
// controle de dynamic-seed-blacklist nao divergirem entre os dois lados.

#ifdef ARDUINO
#include <Arduino.h>
#include <RF24.h>
#else
#include <cstdint>
#include <cstddef>
typedef uint8_t byte;
#endif

static const byte masterAddress[6] = "MSTR1";
static const byte slaveAddress[6] = "SLV01";

#ifdef ARDUINO
// adaptive-rendezvous: substituto seguro para RF24::write(). Essa versao
// da lib (RF24 1.6.2) tem um bug real em write(buf,len,multicast): o laco
// "do { startFastWrite(...); } while (errHandler(&doRecovery) < 0);"
// nunca sai, porque errHandler() testa "if (doRecovery)" (o PONTEIRO,
// sempre nao-nulo) em vez de "if (*doRecovery)" (o valor) -- toda vez que
// o timeout interno de 95ms dispara ele retorna -1 de novo e o do-while
// tenta de novo, para sempre, se TX_DS/TX_DF nunca setarem (peer fora do
// alcance, canal errado, jammer ativo). Reproduzido em bancada: mestre
// travava permanentemente na primeira sonda de pareamento sem escravo
// presente no canal.
//
// writeFast()+txStandBy(timeout) usam um caminho de erro DIFERENTE e
// genuinamente limitado (bound explicito em txStandBy, sem a mesma
// checagem de ponteiro sempre-verdadeira). multicast=true (NO_ACK)
// sempre, porque a confirmacao real de qualquer troca deste protocolo já
// vem de um pacote de resposta explicito lido separadamente
// (LinkAck/SeedAck/FragmentAck/ControlAck) -- nunca dependemos do ACK de
// hardware do NRF24 pra saber se algo funcionou, então write() em si só
// precisa terminar (nao travar), nao confirmar entrega.
static const uint32_t RADIO_WRITE_TIMEOUT_MS = 50;

static inline bool radio_send(RF24 &radio, const void *buf, uint8_t len)
{
    radio.flush_tx(); // garante FIFO limpo -- um payload preso de uma
                       // tentativa anterior travaria writeFast() em
                       // TX_FULL para sempre (nunca reenviado).
    bool queued = radio.writeFast(buf, len, true);
    if (!queued)
        return false;
    return radio.txStandBy(RADIO_WRITE_TIMEOUT_MS, true);
}
#endif // ARDUINO

enum MessageType : uint8_t
{
    MSG_LINK_PROBE = 1,
    MSG_LINK_ACK = 2,
    MSG_FRAGMENT = 3,
    MSG_FRAGMENT_ACK = 4,
    MSG_SEED_INIT = 5,
    MSG_SEED_ACK = 6,
    // dynamic-seed-blacklist: pacotes de controle, enviados no lugar do
    // proximo fragmento quando ha uma renegociacao pendente (fila de
    // prioridade simples -- ver design.md daquele change).
    MSG_CTRL_SEED = 7,
    MSG_CTRL_BLACKLIST = 8,
    MSG_CTRL_ACK = 9
};

static const uint8_t FRAGMENT_DATA_SIZE = 20;

// -------------------------------------------------------------
// Pareamento (adaptive-rendezvous)
// -------------------------------------------------------------
// 8 candidatos; a maioria fora da faixa 2-80 varrida pelo jammer sweep
// (jammer/src/node.h), para sempre sobrar onde parear mesmo sob ataque de
// banda larga. 76 mantido como primeiro candidato (compatibilidade com o
// comportamento anterior a este change).
static const uint8_t PAIRING_CANDIDATES[8] = {76, 90, 96, 101, 108, 114, 119, 124};
static const uint8_t PAIRING_CANDIDATE_COUNT = 8;

static const uint32_t LINK_RETRY_MS = 300;     // mestre: intervalo entre MSG_LINK_PROBE
static const uint32_t LINK_ACK_WAIT_MS = 120;  // mestre: espera por MSG_LINK_ACK apos probe
static const uint32_t PAIR_CAMP_MS = 2000;     // mestre: tempo acampado num candidato antes de rodar pro proximo
// escravo: tempo escutando cada candidato. DEVE ser maior que
// LINK_RETRY_MS -- por pigeonhole, qualquer janela de escuta mais larga
// que o intervalo entre sondas do mestre garante pegar pelo menos uma
// sonda, não importa a fase relativa entre os dois relogios livres
// (millis() de cada placa comeca em instante diferente e não se
// realinha). Com dwell < intervalo de sonda (250 < 300, valor anterior),
// a janela de escuta podia sistematicamente cair sempre entre duas
// sondas consecutivas e nunca convergir -- reproduzido em bancada.
static const uint32_t PAIR_DWELL_MS = 400;
static const uint32_t PAIR_MEASURE_SAMPLES = 20; // mestre: amostras de testCarrier() por candidato

static const uint32_t HOP_START_DELAY_MS = 900;
// Hopping por evento (nao mais por relogio periodico fixo): mestre
// reenvia o fragmento corrente e escravo escuta no canal corrente ate
// UM dos dois acontecer primeiro -- confirmacao (ack recebido / fragmento
// recebido e ackado) ou este timeout esgotar (fallback: avanca pro
// proximo canal mesmo sem confirmacao, pra o enlace nao travar
// indefinidamente num canal jamado). Ver mestre_loop()/escravo_loop().
static const uint32_t HOP_TIMEOUT_MS = 220;
static const uint32_t ACK_TIMEOUT_MS = 120;
static const uint32_t SEED_ACK_WAIT_MS = 500;
static const uint32_t SEED_WAIT_WINDOW_MS = 3000; // seed nao-bloqueante: janela na serial antes do default
static const uint16_t SEED_DEFAULT = 42;

// Perda de sincronismo: depois de LOSS_HOPS hops sem trafego/ACK valido, os
// dois lados voltam ao estado de pareamento (mesma maquina de estados serve
// o primeiro encontro e a recuperacao apos falha).
static const uint32_t LOSS_HOPS = 25;

// dynamic-seed-blacklist
static const uint8_t CHANNEL_FAIL_THRESHOLD = 5; // g_channel_fail >= isso -> candidato a blacklist
// blacklist-decay: hops que um canal precisa passar excluído antes de ser
// reincluído às cegas (ver shared/hopping.h update_channel_mask() e
// design.md "Cooldown por hop_counter"). Ordem de grandeza inicial, a
// calibrar em bancada (openspec/changes/blacklist-decay/tasks.md 4.2) --
// bem maior que CHANNEL_FAIL_THRESHOLD para não flappar.
static const uint32_t RECOVERY_COOLDOWN_HOPS = 200;
static const uint8_t CONTROL_RETRY_HOPS = 6;     // hops repetindo o pacote de controle ate confirmar
static const uint8_t CONTROL_APPLY_MARGIN = 2;   // folga de hops entre confirmacao e troca efetiva

struct __attribute__((packed)) LinkPacket
{
    uint8_t type;
};

struct __attribute__((packed)) SeedPacket
{
    uint8_t type; // MSG_SEED_INIT
    uint16_t seed;
};

struct __attribute__((packed)) SeedAckPacket
{
    uint8_t type; // MSG_SEED_ACK
    uint16_t seed;
};

struct __attribute__((packed)) FragmentPacket
{
    uint8_t type;
    uint16_t msg_id;
    uint8_t fragment_index;
    uint8_t total_fragments;
    uint8_t payload_len;
    char payload[FRAGMENT_DATA_SIZE];
    uint32_t hop_counter; // adaptive-rendezvous: resync do contador de hop
};

struct __attribute__((packed)) FragmentAck
{
    uint8_t type;
    uint16_t msg_id;
    uint8_t fragment_index;
    uint8_t complete;
};

// dynamic-seed-blacklist: mesmo tamanho (7 bytes) que ControlBlacklistPacket
// -- os dois so aparecem quando ha controle pendente (nunca simultaneos), e
// o campo `type` (primeiro byte dos dois) desambigua no recebedor.
struct __attribute__((packed)) ControlSeedPacket
{
    uint8_t type; // MSG_CTRL_SEED
    uint16_t new_seed;
    uint32_t apply_at_hop;
};

struct __attribute__((packed)) ControlBlacklistPacket
{
    uint8_t type; // MSG_CTRL_BLACKLIST
    uint16_t mask;
    uint32_t apply_at_hop;
};

struct __attribute__((packed)) ControlAck
{
    uint8_t type; // MSG_CTRL_ACK
    uint32_t apply_at_hop;
};

static_assert(sizeof(LinkPacket) <= 32, "LinkPacket excede 32 bytes do NRF24");
static_assert(sizeof(SeedPacket) <= 32, "SeedPacket excede 32 bytes do NRF24");
static_assert(sizeof(SeedAckPacket) <= 32, "SeedAckPacket excede 32 bytes do NRF24");
static_assert(sizeof(FragmentPacket) <= 32, "FragmentPacket excede 32 bytes do NRF24");
static_assert(sizeof(FragmentAck) <= 32, "FragmentAck excede 32 bytes do NRF24");
static_assert(sizeof(ControlSeedPacket) <= 32, "ControlSeedPacket excede 32 bytes do NRF24");
static_assert(sizeof(ControlBlacklistPacket) <= 32, "ControlBlacklistPacket excede 32 bytes do NRF24");
static_assert(sizeof(ControlAck) <= 32, "ControlAck excede 32 bytes do NRF24");

#endif // PROTOCOL_H
