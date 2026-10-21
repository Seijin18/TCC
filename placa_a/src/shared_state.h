#ifndef PLACA_A_SHARED_STATE_H
#define PLACA_A_SHARED_STATE_H

// channel-quality-sensing: máscara de canais degradados (bit i = 1 ->
// HOPPING_CHANNELS[i]) entre a task do mestre e a do analisador, no lugar do
// GPIO do cenário de 4 placas. Um escritor, um leitor, uint16_t alinhado
// (load/store atômico no Xtensa) — sem mutex.

#include <Arduino.h>

static volatile uint16_t g_placaA_chq_mask = 0;

static void placaA_chq_set(uint16_t mask)
{
    g_placaA_chq_mask = mask;
}

static uint16_t placaA_chq_get()
{
    return g_placaA_chq_mask;
}

#endif // PLACA_A_SHARED_STATE_H
