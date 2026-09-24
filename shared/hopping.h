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

// scenario-automation: canal único usado pelo modo de teste "sem hopping"
// (Cenário A do guia -- baseline sem nenhuma adaptação, pra comparar com
// hopping fixo/adaptativo). Literal e compartilhado entre mestre/escravo
// (em vez de negociado por pacote) porque o modo é configuração de bancada
// pelo operador, não uma decisão em runtime que precise de sincronização.
const uint8_t TEST_FIXED_CHANNEL = HOPPING_CHANNELS[0];

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

/**
 * blacklist-decay: decide a próxima máscara de canais habilitados a partir
 * da atual, em dois sentidos -- excluir canais com falha severa e
 * persistente (comportamento já existente de `compute_candidate_mask()`) e
 * reincluir canais antes excluídos cujo cooldown (em hops, não em millis()
 * -- ver design.md "Cooldown por hop_counter") já venceu. Lógica pura, sem
 * estado próprio: opera sobre os arrays que o chamador mantém
 * (`fail_count`/`blacklisted_since`, ambos tamanho TABLE_SIZE), do mesmo
 * jeito que `get_next_active_channel` já faz com `mask`.
 *
 * Fase 1 (exclusão, com a guarda de "nunca esvaziar todos os canais" já
 * existente): só grava `blacklisted_since[i]` na transição
 * habilitado->excluído -- uma chamada repetida com o canal já excluído (ou
 * revertido pela guarda) nunca empurra o timestamp pra frente, senão o
 * cooldown nunca venceria (ver design.md, risco de "empurrar" o timestamp).
 *
 * Fase 2 (reinclusão, cega -- sem sondagem ativa, ver design.md
 * "Reinclusão é cega"): qualquer canal ainda desabilitado depois da fase 1
 * cujo `hop_counter - blacklisted_since[i] >= recovery_cooldown_hops`
 * volta à máscara, com `fail_count[i]` zerado (dá a ele o mesmo número de
 * falhas frescas antes de ser excluído de novo, em vez de já nascer no
 * limiar).
 */
inline uint16_t update_channel_mask(uint16_t mask, uint8_t fail_count[], uint32_t blacklisted_since[],
                                     uint32_t hop_counter, uint8_t fail_threshold, uint32_t recovery_cooldown_hops)
{
    uint16_t candidate = mask;
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
    {
        if ((mask & (1u << i)) && fail_count[i] >= fail_threshold)
            candidate &= (uint16_t)~(1u << i);
    }

    if (candidate == 0)
    {
        candidate = mask; // guarda: nunca esvaziar todos os canais habilitados
    }
    else
    {
        for (uint8_t i = 0; i < TABLE_SIZE; i++)
        {
            bool was_enabled = (mask & (1u << i)) != 0;
            bool still_enabled = (candidate & (1u << i)) != 0;
            if (was_enabled && !still_enabled)
                blacklisted_since[i] = hop_counter; // transição habilitado->excluído
        }
    }

    for (uint8_t i = 0; i < TABLE_SIZE; i++)
    {
        if (candidate & (1u << i))
            continue; // já habilitado, nada a reincluir
        if (hop_counter - blacklisted_since[i] >= recovery_cooldown_hops)
        {
            candidate |= (uint16_t)(1u << i);
            fail_count[i] = 0;
        }
    }

    return candidate;
}

#endif // HOPPING_H
