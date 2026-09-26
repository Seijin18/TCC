#ifndef PLACA_A_SHARED_STATE_H
#define PLACA_A_SHARED_STATE_H

// Flag de alerta compartilhada entre a task do mestre (core do loop()
// padrão do Arduino) e a task do analisador (core secundário), usada no
// lugar do GPIO físico do cenário de 4 placas
// (spectrum-attack-classification). Um único escritor (analisador), um
// único leitor (mestre), tipo primitivo (bool cabe num load/store atômico
// de 1 byte no Xtensa) — não precisa de mutex.

#include <Arduino.h>

static volatile bool g_alert_shared = false;

static void placaA_alert_set(bool active)
{
    g_alert_shared = active;
}

static bool placaA_alert_get()
{
    return g_alert_shared;
}

#endif // PLACA_A_SHARED_STATE_H
