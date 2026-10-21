#ifndef PAIRING_H
#define PAIRING_H

// Nos firmwares (ESP32/Arduino) usa os typedefs de Arduino.h; no ambiente de
// testes nativo (env:native_test, sem framework Arduino) cai para <cstdint>
// -- mesmo padrao de shared/hopping.h.
#ifdef ARDUINO
#include <Arduino.h>
#else
#include <cstdint>
#endif

#include "protocol.h"

// pairing-handshake-hardening (tarefa 3.2, design.md D3): logica PURA de
// decisao do handshake v2 -- sem estado proprio, sem I/O, sem dependencia
// de Arduino/RF24 (so tipos inteiros e os pacotes de shared/protocol.h),
// testada isoladamente em env:native_test (mesmo padrao de
// shared/hopping.h). mestre/src/node.h e escravo/src/node.h guardam o
// ESTADO de verdade (nonce da sessao corrente, se o escravo esta
// pareado-provisorio, etc.) e chamam estas funcoes pra decidir o que fazer
// com cada pacote recebido -- toda a parte "e agora o que eu faco com o
// radio/telemetria" fica de fora daqui de proposito.

/**
 * Decisao do escravo ao receber uma LinkProbeV2 valida (tamanho e
 * `type == MSG_LINK_PROBE` ja conferidos pelo chamador -- so o CONTEUDO da
 * sonda e avaliado aqui):
 *
 *   PAIR_IGNORE     -- versao de protocolo (`ver`) diferente da esperada;
 *                       o escravo nao responde (evita confundir um nó com
 *                       firmware antigo/incompativel com uma sessao nova --
 *                       ver design.md D3, "ver evita...").
 *   PAIR_ADOPT_NEW  -- escravo ainda nao esta pareado-provisorio (varrendo
 *                       candidatos), OU ja esta mas a sonda traz um nonce
 *                       DIFERENTE do da sessao adotada (o mestre
 *                       recomecou o pareamento) -- em ambos os casos, toda
 *                       sonda valida com `ver` certo inicia/substitui a
 *                       sessao corrente.
 *   PAIR_REACK      -- escravo ja pareado-provisorio E a sonda traz o
 *                       MESMO nonce da sessao adotada -- resposta perdida
 *                       do lado do mestre, reenviar o ack de novo
 *                       (idempotente, ver design.md "Resposta perdida e
 *                       recuperada por re-ack").
 */
enum PairDecision : uint8_t
{
    PAIR_IGNORE = 0,
    PAIR_REACK = 1,
    PAIR_ADOPT_NEW = 2,
};

inline PairDecision pair_decide_escravo(bool provisional, uint16_t session_nonce,
                                         uint8_t probe_ver, uint16_t probe_nonce)
{
    if (probe_ver != PAIRING_PROTO_VER)
        return PAIR_IGNORE;
    if (!provisional)
        return PAIR_ADOPT_NEW;
    if (probe_nonce == session_nonce)
        return PAIR_REACK;
    return PAIR_ADOPT_NEW;
}

/**
 * Decisao do mestre ao receber uma LinkAckV2 durante a espera por resposta
 * da sonda corrente: só aceita como fechamento da tentativa se `ver`,
 * `nonce` E `seed` batem exatamente com os da sonda que o mestre mandou --
 * qualquer divergencia é um ack que não pertence a esta tentativa/sessão
 * (FIFO antiga, retransmissão de uma tentativa anterior, escravo em outra
 * sessão) e deve ser contado como `stale_ack` pelo chamador, não aceito.
 */
inline bool pair_accept_ack_mestre(uint16_t expected_nonce, uint16_t expected_seed,
                                    uint8_t ack_ver, uint16_t ack_nonce, uint16_t ack_seed)
{
    return ack_ver == PAIRING_PROTO_VER && ack_nonce == expected_nonce && ack_seed == expected_seed;
}

/**
 * half-open-handshake-recovery: decide se o escravo deve forcar a
 * recuperacao de um handshake meio-aberto -- `sessions_without_data` sessoes
 * provisorias seguidas (nonces novos) sem nenhum FragmentPacket confirmando o
 * enlace. A recuperacao dispara ao atingir (>=) o limiar
 * (HALF_OPEN_SESSIONS em shared/protocol.h). Quem guarda o contador e zera
 * ao confirmar o enlace e escravo/src/node.h.
 */
inline bool half_open_should_recover(uint8_t sessions_without_data, uint8_t threshold)
{
    return sessions_without_data >= threshold;
}

#endif // PAIRING_H
