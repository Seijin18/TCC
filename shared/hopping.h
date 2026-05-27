#ifndef HOPPING_H
#define HOPPING_H

#include <Arduino.h>

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

// Estrutura para transmissão da Seed inicial
struct SeedPacket
{
    uint16_t seed;      // Semente para cálculo da sequência (0-100)
};

static_assert(sizeof(SeedPacket) <= 32, "SeedPacket excede 32 bytes do NRF24");

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

#endif // HOPPING_H
