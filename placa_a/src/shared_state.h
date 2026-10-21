#ifndef PLACA_A_SHARED_STATE_H
#define PLACA_A_SHARED_STATE_H

// Máscara de canais degradados compartilhada entre a task do mestre (core do
// loop() padrão do Arduino) e a task do analisador (core secundário), usada
// no lugar do GPIO físico do cenário de 4 placas (channel-quality-sensing,
// tarefas 3.1/3.3; antes era um bool de alerta -- spectrum-attack-classification).
// Bit i = 1 -> HOPPING_CHANNELS[i] degradado. Um único escritor
// (analisador), um único leitor (mestre), tipo primitivo (uint16_t alinhado
// cabe num load/store atômico no Xtensa) — não precisa de mutex.

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
