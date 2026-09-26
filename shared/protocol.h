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

// hop-resync (tarefa 1.1): RESYNC_PARK_MAX_MS abaixo depende de TABLE_SIZE
// (shared/hopping.h) -- incluido aqui (em vez de exigir que todo consumidor
// de protocol.h inclua hopping.h antes) porque hopping.h nao depende de
// nada deste header (sem risco de inclusao circular) e nem todo consumidor
// atual segue essa ordem (ex.: shared/pairing.h inclui só protocol.h).
#include "hopping.h"

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
// (LinkAckV2/FragmentAck/ControlAck) -- nunca dependemos do ACK de
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
    // MSG_SEED_INIT/MSG_SEED_ACK (5, 6) existiram no handshake de 4
    // mensagens do adaptive-rendezvous -- removidos pelo
    // pairing-handshake-hardening (D3, tarefa 3.1): a seed agora viaja
    // dentro da propria sonda/ack (LinkProbeV2/LinkAckV2), sem uma troca
    // separada. Valores deixados vagos de proposito (nao reaproveitados),
    // pra um dump antigo de rede/log nao ser reinterpretado por engano.
    // dynamic-seed-blacklist: pacotes de controle, enviados no lugar do
    // proximo fragmento quando ha uma renegociacao pendente (fila de
    // prioridade simples -- ver design.md daquele change).
    MSG_CTRL_SEED = 7,
    MSG_CTRL_BLACKLIST = 8,
    MSG_CTRL_ACK = 9
};

// pairing-handshake-hardening (D3, tarefa 3.1): versao do protocolo de
// pareamento, transportada em LinkProbeV2/LinkAckV2 -- um no gravado com o
// handshake antigo (pre-nonce) ou uma versao futura incompativel nao deve
// ser confundido com esta: `ver` diferente => ignora e loga
// `pair_ver_mismatch` (ver shared/pairing.h pair_decide_escravo()).
static const uint8_t PAIRING_PROTO_VER = 2;

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

// pairing-handshake-hardening (D4, tarefa 4.1): valores recalibrados a
// partir do numero-base de bancada (tasks.md 1.8/2.5) -- o handshake v2
// (D3) manda a sonda a cada LINK_RETRY_MS e só precisa aguardar o ack por
// alguns ms (LINK_ACK_WAIT_MS), não mais o round-trip completo de uma
// troca de seed separada. Antes desta calibragem, LINK_ACK_WAIT_MS=120
// (>LINK_RETRY_MS=300) so atrasava a proxima sonda sem necessidade.
static const uint32_t LINK_RETRY_MS = 100;     // mestre: intervalo entre MSG_LINK_PROBE
static const uint32_t LINK_ACK_WAIT_MS = 30;   // mestre: espera por MSG_LINK_ACK apos probe
static const uint32_t PAIR_CAMP_MS = 4500;     // mestre: tempo acampado num candidato antes de rodar pro proximo
// escravo: tempo escutando cada candidato. DEVE ser maior que
// LINK_RETRY_MS -- por pigeonhole, qualquer janela de escuta mais larga
// que o intervalo entre sondas do mestre garante pegar pelo menos uma
// sonda, não importa a fase relativa entre os dois relogios livres
// (millis() de cada placa comeca em instante diferente e não se
// realinha). Com dwell < intervalo de sonda (250 < 300, valor anterior do
// adaptive-rendezvous), a janela de escuta podia sistematicamente cair
// sempre entre duas sondas consecutivas e nunca convergir -- reproduzido
// em bancada.
//
// pairing-handshake-hardening (D4): duas invariantes de COBERTURA TEMPORAL
// do encontro, checadas em compilação (spec "Cobertura temporal garantida
// do encontro") -- uma mudança futura de constante que quebre qualquer uma
// delas não compila, em vez de silenciosamente parar de convergir (foi
// exatamente o que aconteceu quando o dwell do adaptive-rendezvous subiu de
// 250 para 400 ms sem reavaliar o intervalo de sonda):
//   1) PAIR_DWELL_MS >= 2 * LINK_RETRY_MS -- cada janela de escuta do
//      escravo contém pelo menos DUAS sondas do mestre (folga sobre o
//      pigeonhole de 1 sonda acima, cobrindo a perda isolada de uma delas
//      sem esperar a volta completa da varredura).
//   2) PAIR_CAMP_MS >= 2 * PAIRING_CANDIDATE_COUNT * PAIR_DWELL_MS -- cada
//      acampamento do mestre dura pelo menos DOIS ciclos completos de
//      varredura do escravo pelos 8 candidatos (E6, design.md: um
//      acampamento mais curto que um ciclo de varredura pode terminar sem o
//      escravo sequer ter passado pelo canal).
static const uint32_t PAIR_DWELL_MS = 250;
static const uint32_t PAIR_MEASURE_SAMPLES = 20; // mestre: amostras de testCarrier() por candidato

static_assert(PAIR_DWELL_MS >= 2 * LINK_RETRY_MS,
              "PAIR_DWELL_MS precisa ser >= 2x LINK_RETRY_MS (cobertura de pelo menos 2 sondas por janela de escuta)");
static_assert(PAIR_CAMP_MS >= 2 * (uint32_t)PAIRING_CANDIDATE_COUNT * PAIR_DWELL_MS,
              "PAIR_CAMP_MS precisa ser >= 2x PAIRING_CANDIDATE_COUNT x PAIR_DWELL_MS (pelo menos 2 ciclos completos de varredura por acampamento)");

// adaptive-rendezvous (2026-09-24): pareamento ganha o mesmo mecanismo de
// evitar-canal-ruim de blacklist-decay, aplicado aos PAIRING_CANDIDATES em
// vez de HOPPING_CHANNELS -- ver mestre/src/node.h note_candidate_result().
// Achado de bancada: um candidato pode medir "quieto" por RPD
// (measure_and_pick_candidate) e mesmo assim falhar repetidamente na troca
// de pacotes real (handshake) -- a métrica de energia não substitui a
// evidência de sucesso/falha real do protocolo. Limiar mais baixo que
// CHANNEL_FAIL_THRESHOLD porque cada falha aqui já consumiu um
// PAIR_CAMP_MS inteiro (ou uma tentativa de handshake que não confirmou
// dado -- ver D5/note_candidate_result() em mestre/src/node.h) -- não vale
// a pena esperar tantas quanto no hopping de dados, onde cada falha é só
// 1 hop.
static const uint8_t PAIR_FAIL_THRESHOLD = 3;
// Em "rounds" de seleção de candidato (não hops, que ainda não existem
// antes de parear) -- ver g_pair_round em mestre/src/node.h.
static const uint32_t PAIR_RECOVERY_ROUNDS = 12;

static const uint32_t HOP_START_DELAY_MS = 900;
// Hopping por evento (nao mais por relogio periodico fixo): o salto avanca a
// cada TENTATIVA de transmissao, desacoplado do ACK (hop-ack-decoupling).
// O mestre manda o fragmento corrente UMA vez no canal corrente e espera o
// ack por ATTEMPT_TIMEOUT_MS; o escravo escuta o canal corrente por
// ATTEMPT_TIMEOUT_MS. Um dos dois acontece primeiro -- confirmacao (ack
// recebido / fragmento recebido e ackado) ou este timeout esgotar (avanca
// pro proximo canal de qualquer jeito, e o fragmento nao-ackado e reenviado
// no proximo canal, onde o escravo ja esta). Ver mestre_loop()/escravo_loop().
//
// hop-ack-decoupling: a cadencia de um MISS cai de ~220 ms (uma janela
// inteira de reenvio no mesmo canal, comportamento anterior) para
// ATTEMPT_TIMEOUT_MS (uma tentativa curta). Referencia: Bluetooth salta a
// cada slot/evento independente do ACK (lockstep) e a confiabilidade fica
// num layer separado (ARQN+SEQN). Aqui o SEQN e o (msg_id, fragment_index)
// deduplicado por received_bitmap no escravo.
static const uint32_t ATTEMPT_TIMEOUT_MS = 30;
static const uint32_t ACK_TIMEOUT_MS = 120;
static const uint32_t SEED_WAIT_WINDOW_MS = 3000; // seed nao-bloqueante: janela na serial antes do default
// pairing-handshake-hardening (D3, tarefa 3.5): com o handshake v2 a seed
// nao viaja mais separada (vai na propria LinkProbeV2/LinkAckV2 -- ver
// shared/pairing.h), entao este timeout deixou de ser "espera pela seed" e
// virou "espera pela CONFIRMACAO do enlace": o escravo entra
// pareado-provisorio ao responder uma sonda valida (ver
// PAIR_ESCRAVO_ADOPT_NEW/REACK em pairing.h) e só sai desse estado quando
// (a) o primeiro FragmentPacket valido chega depois do inicio do salto
// (confirma o enlace, pair_confirm) ou (b) este timeout esgota contado a
// partir do hop_start_at corrente (que e rearmado a cada re-ack recebido
// enquanto ainda provisorio) -- nesse caso volta a escanear. Mesmo
// raciocinio de DEADLOCK do achado original permanece valido: sem este
// timeout, um escravo que respondeu mas cujo ack nunca chegou ao mestre
// (ou cujo mestre desistiu do candidato) ficaria preso pra sempre
// esperando dado que nunca vai chegar.
static const uint32_t SEED_WAIT_TIMEOUT_MS = 3000;
static const uint16_t SEED_DEFAULT = 42;

// Perda de sincronismo: depois de LOSS_HOPS hops sem trafego/ACK valido, os
// dois lados voltam ao estado de pareamento (mesma maquina de estados serve
// o primeiro encontro e a recuperacao apos falha).
// hop-ack-decoupling: re-derivado de 25 (@ ate 220 ms/hop = ~5,5 s) para
// preservar o mesmo ORCAMENTO DE TEMPO na nova cadencia (~30 ms/hop):
// 5,5 s / 0,030 s ~= 183 -> 180.
static const uint32_t LOSS_HOPS = 180;

// hop-resync (tarefa 1.1, design.md D1/D2): defasagem de 1 canal entre
// mestre e escravo (ack perdido logo apos um fragmento recebido -- ver
// proposal.md "Mecanismo") nao se corrige sozinha enquanto nenhum pacote
// chega em nenhum dos dois lados. Depois de RESYNC_MISS_HOPS timeouts de
// hop CONSECUTIVOS sem nada valido, o escravo para de saltar por conta
// propria e estaciona no ultimo canal em que recebeu algo valido -- ver
// escravo_advance_hop()/escravo_loop() em escravo/src/node.h. 2 hops (440ms)
// e o minimo que ainda deixa uma falha ISOLADA (recupera sozinha no proximo
// hop, ver design.md "Mecanismo": caso "fragmento perdido") se resolver
// antes de estacionar.
// hop-ack-decoupling (design.md item 4): com o lockstep por-tentativa, um
// miss isolado nao desalinha mais (os dois lados avancam ATTEMPT_TIMEOUT_MS
// juntos e o hop_counter no pacote corrige quando algo passa), entao o
// estacionamento vira ULTIMO RECURSO: elevado de 6 para 45 -- so dispara
// apos muitos hops secos CONSECUTIVOS (desalinhamento genuino e sustentado,
// nao um canal jammed pontual).
static const uint8_t RESYNC_MISS_HOPS = 45;
// D2: estacionamento dura no maximo (TABLE_SIZE + 2) hops do mestre --
// TABLE_SIZE cobre um ciclo completo do round-robin sobre a mascara de
// canais ativa (o mestre passa pelo canal estacionado em no maximo
// active_count <= TABLE_SIZE hops, ver get_next_active_channel() em
// shared/hopping.h), +2 de folga. hop-ack-decoupling: um ciclo completo
// agora custa ATTEMPT_TIMEOUT_MS por hop, nao HOP_TIMEOUT_MS.
static const uint32_t RESYNC_PARK_MAX_MS = (uint32_t)(TABLE_SIZE + 2) * ATTEMPT_TIMEOUT_MS;

// D2: orcamento verificado em compilacao -- a ressincronizacao completa
// (hops ate estacionar + tempo estacionado) tem que terminar antes do
// mestre desistir por LOSS_HOPS, senao o mestre re-pareia enquanto o
// escravo ainda estaria tentando se recuperar sozinho. hop-ack-decoupling:
// reexpresso em ATTEMPT_TIMEOUT_MS (a cadencia unica dos dois lados agora).
static_assert(RESYNC_MISS_HOPS * ATTEMPT_TIMEOUT_MS + RESYNC_PARK_MAX_MS < LOSS_HOPS * ATTEMPT_TIMEOUT_MS,
              "orcamento de ressincronizacao (RESYNC_MISS_HOPS + estacionamento) precisa terminar antes do mestre declarar LOSS_HOPS");

// dynamic-seed-blacklist
// jamming-mitigation-hardening: baixado de 5 para 3 -- com a atribuição de
// falha agora restrita a perdas isoladas (ATTRIB_MAX_HOPS_SINCE_ACK em
// mestre/src/node.h), quase só o canal realmente jammed acumula falha, então
// um limiar menor bane o atacante mais rápido (antes do enlace colapsar) sem
// o risco antigo de banir canais inocentes por rajada de dessincronismo.
// hop-ack-decoupling: MANTIDO em 3 (nao re-derivado). Ao contrario das
// constantes de TEMPO acima, este limiar conta VISITAS ao canal ruim -- com
// round-robin sobre ~16 canais, o canal jammed e visitado ~1x a cada 16
// hops independentemente da cadencia, entao o numero de visitas ate banir e
// o mesmo; so o TEMPO ate la cai ~7x (bane mais rapido, efeito desejado).
static const uint8_t CHANNEL_FAIL_THRESHOLD = 3; // ban rapido; escravo (suspect_idx) da a evidencia limpa, attribution gate evita inocentes
// blacklist-decay: hops que um canal precisa passar excluído antes de ser
// reincluído às cegas (ver shared/hopping.h update_channel_mask() e
// design.md "Cooldown por hop_counter"). Ordem de grandeza inicial, a
// calibrar em bancada (openspec/changes/blacklist-decay/tasks.md 4.2) --
// bem maior que CHANNEL_FAIL_THRESHOLD para não flappar.
// hop-ack-decoupling: re-derivado de 200 (@ ate 220 ms/hop ~= 44 s) para
// preservar o mesmo TEMPO de cooldown na nova cadencia (~30 ms/hop):
// ~44 s / 0,030 s ~= 1466 -> 1500. Sem isto, um canal jammed banido seria
// reincluido as cegas ~7x mais rapido (churn de re-teste).
static const uint32_t RECOVERY_COOLDOWN_HOPS = 1500;
static const uint8_t CONTROL_RETRY_HOPS = 45;    // hops repetindo o controle (re-derivado de 6 @220ms p/ ~1,3s @30ms)
static const uint8_t CONTROL_APPLY_MARGIN = 15;  // folga entre confirmacao e troca (re-derivado de 2 @220ms p/ ~450ms @30ms)

// pairing-handshake-hardening (D3, tarefa 3.1): handshake de pareamento v2
// -- substitui o LinkPacket de 1 byte (sem identificacao de sessao) e as
// mensagens separadas de seed (SeedPacket/SeedAckPacket, removidas do
// enum acima) por 2 mensagens que carregam nonce de sessao E seed juntos.
// `ver` (PAIRING_PROTO_VER) protege contra confundir um no com firmware
// antigo; `nonce` identifica a tentativa (novo a cada acampamento do
// mestre, ver esp_random() em mestre/src/node.h); `seed` viaja na propria
// sonda/ack em vez de uma troca a parte -- a decisao de aceitar/re-ackar/
// adotar nova sessao/ignorar e logica pura em shared/pairing.h.
struct __attribute__((packed)) LinkProbeV2
{
    uint8_t type; // MSG_LINK_PROBE
    uint8_t ver;
    uint16_t nonce;
    uint16_t seed;
    // jamming-mitigation-hardening: a blacklist persiste através do
    // re-pareamento (o mestre não zera a máscara num reset causado por
    // jamming). Para o escravo re-pareado saltar no MESMO conjunto de
    // canais, a máscara viaja na própria sonda -- sem isto, o mestre
    // saltaria em 15 canais e o escravo em 16, dessincronizando.
    uint16_t mask;
};

struct __attribute__((packed)) LinkAckV2
{
    uint8_t type; // MSG_LINK_ACK
    uint8_t ver;
    uint16_t nonce;
    uint16_t seed;
    uint16_t mask; // eco da máscara da sonda (confirmação)
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
    // jamming-mitigation-hardening: o ESCRAVO é quem tem evidência limpa de
    // qual canal está jammed -- ele nunca recebe nesse canal enquanto recebe
    // normalmente nos outros. O mestre, vendo só os próprios misses, não
    // distingue "canal jammed" de "escravo dessincronizado". Então o escravo
    // reporta aqui o índice (posição em HOPPING_CHANNELS) do canal que
    // suspeita jammed (0xFF = nenhum), e o mestre usa isso como evidência
    // autoritativa para banir o canal certo, sem contaminar os inocentes.
    uint8_t suspect_idx;
};

static const uint8_t SUSPECT_NONE = 0xFF;

// dynamic-seed-blacklist: mesmo tamanho (11 bytes, desde hop-resync tarefa
// 1.2a -- antes 7) que ControlBlacklistPacket -- os dois so aparecem quando
// ha controle pendente (nunca simultaneos), e o campo `type` (primeiro byte
// dos dois) desambigua no recebedor.
//
// hop-resync (tarefa 1.2a, design.md D3a): ganham `hop_counter` -- enquanto
// ha controle pendente, o mestre manda o pacote de controle NO LUGAR do
// fragmento em todo hop ate ser ackado (send_pending_control_if_any() em
// mestre/src/node.h), entao um escravo estacionado (ver RESYNC_MISS_HOPS
// acima) pode nunca ver um FragmentPacket -- sem este campo nao teria de
// onde adotar o contador de hop do mestre pra sair do estacionamento.
struct __attribute__((packed)) ControlSeedPacket
{
    uint8_t type; // MSG_CTRL_SEED
    uint16_t new_seed;
    uint32_t apply_at_hop;
    uint32_t hop_counter; // hop-resync (tarefa 1.2a): hop corrente do mestre no envio
};

struct __attribute__((packed)) ControlBlacklistPacket
{
    uint8_t type; // MSG_CTRL_BLACKLIST
    uint16_t mask;
    uint32_t apply_at_hop;
    uint32_t hop_counter; // hop-resync (tarefa 1.2a): hop corrente do mestre no envio
};

struct __attribute__((packed)) ControlAck
{
    uint8_t type; // MSG_CTRL_ACK
    uint32_t apply_at_hop;
};

static_assert(sizeof(LinkProbeV2) <= 32, "LinkProbeV2 excede 32 bytes do NRF24");
static_assert(sizeof(LinkAckV2) <= 32, "LinkAckV2 excede 32 bytes do NRF24");
static_assert(sizeof(FragmentPacket) <= 32, "FragmentPacket excede 32 bytes do NRF24");
static_assert(sizeof(FragmentAck) <= 32, "FragmentAck excede 32 bytes do NRF24");
static_assert(sizeof(ControlSeedPacket) <= 32, "ControlSeedPacket excede 32 bytes do NRF24");
static_assert(sizeof(ControlBlacklistPacket) <= 32, "ControlBlacklistPacket excede 32 bytes do NRF24");
static_assert(sizeof(ControlAck) <= 32, "ControlAck excede 32 bytes do NRF24");

#endif // PROTOCOL_H
