#ifndef HOPPING_H
#define HOPPING_H

// Nos firmwares (ESP32/Arduino) usa os typedefs de Arduino.h; no ambiente de
// testes nativo (env:native_test, sem framework Arduino) cai para <cstdint>.
// Nenhum outro recurso de Arduino.h é usado neste header.
#ifdef ARDUINO
#include <Arduino.h>
#else
#include <cstdint>
#endif

// -------------------------------------------------------------
// FASE 1: Algoritmo de Hopping Inicial
// -------------------------------------------------------------

// Tabela fixa de 16 canais (Frequências de 2400 + CH MHz) -> Ex: Canal 10 = 2410 MHz
// Escolhemos frequências espalhadas e que tentam evitar o máximo do WiFi (canais 1, 6 e 11)
const uint8_t TABLE_SIZE = 16;
const uint8_t HOPPING_CHANNELS[TABLE_SIZE] = {
    10, 22, 34, 46, 58,
    70, 82, 94, 100, 105,
    110, 115, 120, 125, 15, 27};

// Estrutura do Beacon (Sinal de Sincronismo)
struct BeaconPacket
{
    uint32_t counter;   // Contador para saber o passo atual na sequência de saltos
    uint16_t seed;      // Semente para cálculo da sequência
    uint8_t payload[8]; // Dados úteis (pode aumentar depois)
};

// SeedPacket e os demais pacotes de protocolo (Link/Fragment/Control) foram
// movidos para shared/protocol.h (adaptive-rendezvous) -- este header fica
// só com a lógica pura de seleção de canal.

/**
 * Fase inicial: hopping sem seed.
 * O canal é determinado apenas pelo contador local de saltos.
 */
inline uint8_t get_hop_channel(uint32_t counter)
{
    return HOPPING_CHANNELS[counter % TABLE_SIZE];
}

/**
 * Função de Seed: f(Seed, Contador)
 * Retorna o canal a ser utilizado para comunicação.
 */
uint8_t get_next_channel(uint16_t seed, uint32_t counter)
{
    // Lógica simples de embaralhamento matemático usando módulo circular
    uint8_t index = (seed + counter) % TABLE_SIZE;
    return HOPPING_CHANNELS[index];
}

/**
 * dynamic-seed-blacklist: mesma função de f(seed, contador), mas restrita
 * aos canais habilitados por `mask` (bit i = 1 -> HOPPING_CHANNELS[i]
 * disponível). Guarda: mask == 0 (nenhum canal habilitado) nunca deveria
 * acontecer nos chamadores -- degrada para a tabela cheia em vez de causar
 * divisão por zero.
 */
inline uint8_t get_next_active_channel(uint16_t seed, uint32_t counter, uint16_t mask)
{
    if (mask == 0)
        mask = 0xFFFF;

    uint8_t active_indices[TABLE_SIZE];
    uint8_t active_count = 0;
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
    {
        if (mask & (1u << i))
            active_indices[active_count++] = i;
    }

    uint8_t pos = (uint8_t)((seed + counter) % active_count);
    return HOPPING_CHANNELS[active_indices[pos]];
}

#endif // HOPPING_H
