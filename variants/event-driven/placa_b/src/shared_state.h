#ifndef PLACA_B_SHARED_STATE_H
#define PLACA_B_SHARED_STATE_H

// Flag de prontidão compartilhada entre a task do escravo (core do loop()
// padrão do Arduino) e a task do jammer (core secundário): o jammer só
// começa a emitir depois que o escravo confirma que o handshake com o
// mestre terminou (seed recebida, hopping prestes a começar) — sem isso,
// o jammer entra em operação junto com o boot e o canal fixo de
// pareamento (76), dentro da faixa varrida pelo jammer (2–80), nunca fica
// livre por tempo suficiente pro mestre/escravo completarem o handshake
// inicial. Mesmo raciocínio de single-writer/single-reader do
// placa_a/src/shared_state.h (alerta do analisador): não precisa de mutex.

#include <Arduino.h>

static volatile bool g_ready_shared = false;

static void placaB_ready_set(bool ready)
{
    g_ready_shared = ready;
}

static bool placaB_ready_get()
{
    return g_ready_shared;
}

#endif // PLACA_B_SHARED_STATE_H
