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

#endif // RESYNC_H
