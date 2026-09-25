#ifndef RESYNC_H
#define RESYNC_H

// Nos firmwares (ESP32/Arduino) usa os typedefs de Arduino.h; no ambiente de
// testes nativo (env:native_test, sem framework Arduino) cai para <cstdint>
// -- mesmo padrao de shared/hopping.h e shared/pairing.h.
#ifdef ARDUINO
#include <Arduino.h>
#else
#include <cstdint>
#endif

// hop-resync (tarefa 1.5): resync_pick_park_channel() usa TABLE_SIZE como
// tamanho padrao de tabela (mesmo padrao de update_channel_mask() em
// shared/hopping.h) -- incluido aqui sem risco de ciclo, mesmo raciocinio
// de shared/protocol.h (hopping.h nao depende de nada deste header).
#include "hopping.h"

// hop-resync (tarefa 1.4, design.md D1/D2): as duas decisoes de
// estacionamento sao comparacoes puras sobre contadores que o chamador ja
// mantem -- extraidas pra native_test cobrir a fronteira exata (limiar-1
// nao decide, limiar decide) sem hardware. O RESTO da logica (trocar de
// canal, contabilizar PLR/g_hops_since_rx, mandar telemetria) fica em
// escravo/src/node.h de proposito -- tem efeito colateral (radio, millis(),
// tlm_printf) e nao ganha nada em virar funcao pura.

/**
 * D1: o escravo deve entrar em PARKED quando o numero de timeouts de hop
 * CONSECUTIVOS sem nenhuma recepcao valida atinge RESYNC_MISS_HOPS (ver
 * shared/protocol.h). `>=` em vez de `==` -- nao "desarma" se o chamador
 * deixar o contador subir alem do limiar por algum motivo.
 */
inline bool resync_should_park(uint8_t consecutive_miss_hops, uint8_t resync_miss_hops)
{
    return consecutive_miss_hops >= resync_miss_hops;
}

/**
 * D2: o estacionamento esgotou quando o tempo decorrido desde a entrada em
 * PARKED atinge RESYNC_PARK_MAX_MS (ver shared/protocol.h) -- dali em diante
 * o chamador segue o caminho de perda ja existente (reset_to_scanning()).
 */
inline bool resync_park_expired(uint32_t elapsed_ms, uint32_t park_max_ms)
{
    return elapsed_ms >= park_max_ms;
}

/**
 * hop-resync (tarefa 1.5, achado de bancada 2026-09-24): D1 estacionava
 * sempre em `g_last_good_channel` (último canal com recepção válida) -- mas
 * no caso "ack perdido" (proposal.md "Mecanismo") essa última recepção
 * válida acontece EXATAMENTE no canal ruim cujo ack se perdeu (é a causa da
 * própria defasagem), então estacionar ali de novo repete a mesma falha.
 * 1ª tentativa de soak (tasks.md 2.1): 11 `resync_park`, 0 `resync_ok`, 9 de
 * 11 em ch 34 (colado no Wi-Fi) -- toda tentativa expirou.
 *
 * Escolhe, dentre os canais habilitados por `mask`, a posição da tabela
 * (`HOPPING_CHANNELS`) com melhor "qualidade" de recepção recente, a partir
 * dos contadores `ok`/`miss` por posição que o chamador mantém (ver
 * g_resync_channel_ok/g_resync_channel_miss em escravo/src/node.h, com
 * decaimento periódico -- histórico velho não deveria pesar pra sempre).
 *
 * score = ok - 2×miss -- penaliza miss duas vezes mais do que premia ok, pra
 * um canal com muita recepção bruta mas jamado com frequência (o caso do
 * achado acima) perder pra um canal mais quieto mesmo com menos ok. Só
 * canais habilitados com `ok[i] > 0` são elegíveis -- sem nenhuma recepção
 * válida registrada não há evidência de que o canal funciona, e um score
 * "bom" por pura ausência de dado (0 - 2×0 = 0) não deveria competir com um
 * canal comprovado. Empate de score: menor `miss` desempata primeiro;
 * empate total (score E miss iguais): menor índice vence, de graça, porque
 * a busca é em ordem crescente de `i` e só substitui o candidato quando acha
 * algo estritamente melhor (nunca troca por um empate completo -- ver o
 * `if` abaixo). Nenhum canal elegível ⇒ `fallback_idx` (o `g_last_good_channel`
 * de antes desta tarefa, convertido pro índice da tabela pelo chamador).
 */
inline uint8_t resync_pick_park_channel(const uint8_t ok[], const uint8_t miss[],
                                         uint16_t mask, uint8_t fallback_idx,
                                         uint8_t table_size = TABLE_SIZE)
{
    bool have_candidate = false;
    uint8_t best_idx = fallback_idx;
    int16_t best_score = 0;
    uint8_t best_miss = 0;

    for (uint8_t i = 0; i < table_size; i++)
    {
        if (!(mask & (1u << i)))
            continue; // canal desabilitado -- nunca elegivel
        if (ok[i] == 0)
            continue; // nenhuma recepcao valida registrada -- sem evidencia

        int16_t score = (int16_t)ok[i] - 2 * (int16_t)miss[i];

        if (!have_candidate || score > best_score || (score == best_score && miss[i] < best_miss))
        {
            have_candidate = true;
            best_idx = i;
            best_score = score;
            best_miss = miss[i];
        }
    }

    return have_candidate ? best_idx : fallback_idx;
}

#endif // RESYNC_H
