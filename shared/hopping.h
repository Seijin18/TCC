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
 * hop-permutation (tarefa 1.1): PRNG inteiro determinístico (xorshift32)
 * usado só para embaralhar a ordem de visita dos canais ativos -- ver
 * design.md do change hop-permutation (D2). `state` é passado por
 * referência e atualizado a cada chamada (gerador com estado explícito,
 * sem variável global). xorshift32 trava em 0 para sempre se o estado
 * chegar a 0 -- por isso o estado inicial nunca vem direto da seed (ver
 * hop_prng_seed_state() abaixo).
 */
inline uint32_t hop_prng_next(uint32_t &state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

/**
 * hop-permutation (tarefa 1.1): mistura a seed de 16 bits (0-100 na
 * prática, ver mestre/src/node.h wait_for_seed_window()) com uma
 * constante não-nula (proporção áurea em 32 bits, bits altos != 0) antes
 * de usar como estado inicial do xorshift32. Sem isso: (a) seed == 0
 * daria estado inicial 0, que o xorshift32 nunca sai; (b) seeds pequenas
 * (1, 2, 3...) dariam estados quase idênticos entre si, e portanto
 * permutações quase idênticas (quase-identidade) -- ver
 * test_get_next_active_channel_seed_zero_is_not_table_order() e
 * test_get_next_active_channel_seed_one_is_not_table_order() em
 * test/test_hopping/test_hopping.cpp.
 */
inline uint32_t hop_prng_seed_state(uint16_t seed)
{
    return ((uint32_t)seed) ^ 0x9E3779B9u;
}

/**
 * hop-permutation (tarefa 1.1): permutação de Fisher-Yates das posições
 * 0..n-1, determinística a partir da seed -- fixa para uma dada seed
 * (design.md D1: não reembaralha por ciclo, senão o intervalo entre duas
 * visitas ao mesmo canal poderia estourar o orçamento de estacionamento
 * de hop-resync). `perm` precisa ter espaço para `n` elementos (no máximo
 * TABLE_SIZE). Matemática inteira pura, sem random()/esp_random() nem
 * ponto flutuante -- mesmo resultado no ESP32 e no ambiente nativo de
 * teste.
 */
inline void hop_build_permutation(uint16_t seed, uint8_t n, uint8_t perm[])
{
    uint32_t state = hop_prng_seed_state(seed);
    for (uint8_t i = 0; i < n; i++)
        perm[i] = i;
    for (uint8_t i = (uint8_t)(n - 1); i > 0; i--)
    {
        uint32_t r = hop_prng_next(state);
        uint8_t j = (uint8_t)(r % (uint32_t)(i + 1));
        uint8_t tmp = perm[i];
        perm[i] = perm[j];
        perm[j] = tmp;
    }
}

/**
 * dynamic-seed-blacklist: mesma função de f(seed, contador), mas restrita
 * aos canais habilitados por `mask` (bit i = 1 -> HOPPING_CHANNELS[i]
 * disponível). Guarda: mask == 0 (nenhum canal habilitado) nunca deveria
 * acontecer nos chamadores -- degrada para a tabela cheia em vez de causar
 * divisão por zero.
 *
 * hop-permutation (tarefa 1.1): a ordem de visita das posições ativas
 * deixou de ser `(seed + counter) % n` (deslocamento de fase só, mesma
 * ordem da tabela pra qualquer seed) e passou a ser uma permutação
 * pseudoaleatória de `0..n-1` derivada da seed (Fisher-Yates, ver
 * hop_build_permutation() acima) -- ver proposal.md/design.md do change
 * hop-permutation. Continua valendo a garantia de que cada posição ativa
 * aparece exatamente uma vez a cada `active_count` hops (hop-resync
 * depende disso pro orçamento de estacionamento, RESYNC_PARK_MAX_MS em
 * shared/protocol.h) -- só a ordem dentro do ciclo muda.
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

    uint8_t perm[TABLE_SIZE];
    hop_build_permutation(seed, active_count, perm);

    uint8_t pos = perm[counter % active_count];
    return HOPPING_CHANNELS[active_indices[pos]];
}

/** channel-quality-sensing: bits em 1 de uma máscara de 16 canais. */
inline uint8_t mask_popcount16(uint16_t mask)
{
    uint8_t n = 0;
    while (mask)
    {
        n += (uint8_t)(mask & 1u);
        mask >>= 1;
    }
    return n;
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
 *
 * `count` (adaptive-rendezvous, 2026-09-24): generalizado de `TABLE_SIZE`
 * fixo para um tamanho explícito, para a MESMA lógica poder ser reusada
 * sobre `PAIRING_CANDIDATES` (8 entradas) durante o pareamento, não só
 * sobre `HOPPING_CHANNELS` (16) durante o hopping de dados -- ver
 * `mestre/src/node.h` `note_candidate_result()`. Default `TABLE_SIZE`
 * preserva todas as chamadas existentes sem mudança.
 *
 * channel-quality-sensing: `sensor_degraded_mask` e `min_active` têm defaults
 * que preservam o comportamento antigo.
 *  - `sensor_degraded_mask` (bit i = analisador marca HOPPING_CHANNELS[i]):
 *    segundo critério de exclusão na fase 1. Na fase 2 o canal segurado pelo
 *    sensor não é reincluído e `blacklisted_since[i]` é rearmado a cada
 *    chamada, então o cooldown só conta depois que o sensor solta o canal.
 *  - `min_active` (piso de canais ativos; a fusão usa HOP_MIN_ACTIVE_CHANNELS).
 *    Abaixo do piso após a fase 1, devolve os recém-excluídos de melhor
 *    histórico (sensor não marca primeiro, depois menor `fail_count`, depois
 *    menor índice), sem gravar timestamp.
 */
inline uint16_t update_channel_mask(uint16_t mask, uint8_t fail_count[], uint32_t blacklisted_since[],
                                     uint32_t hop_counter, uint8_t fail_threshold, uint32_t recovery_cooldown_hops,
                                     uint8_t count = TABLE_SIZE, uint16_t sensor_degraded_mask = 0,
                                     uint8_t min_active = 1)
{
    uint16_t candidate = mask;
    for (uint8_t i = 0; i < count; i++)
    {
        bool sensor_bad = (sensor_degraded_mask & (1u << i)) != 0;
        if ((mask & (1u << i)) && (fail_count[i] >= fail_threshold || sensor_bad))
            candidate &= (uint16_t)~(1u << i);
    }

    if (candidate == 0 && min_active <= 1)
    {
        candidate = mask; // guarda: nunca esvaziar todos os canais habilitados
    }
    else
    {
        // channel-quality-sensing: piso de canais ativos (só com min_active > 1)
        while (min_active > 1 && mask_popcount16(candidate) < min_active)
        {
            int8_t best = -1;
            uint16_t best_key = 0;
            for (uint8_t i = 0; i < count; i++)
            {
                if (!(mask & (1u << i)) || (candidate & (1u << i)))
                    continue; // só devolve quem estava habilitado e foi excluído agora
                uint16_t key = (uint16_t)(((sensor_degraded_mask & (1u << i)) ? 256u : 0u) + fail_count[i]);
                if (best < 0 || key < best_key)
                {
                    best = (int8_t)i;
                    best_key = key;
                }
            }
            if (best < 0)
                break; // nada mais a devolver (máscara de entrada já abaixo do piso)
            candidate |= (uint16_t)(1u << best);
        }

        for (uint8_t i = 0; i < count; i++)
        {
            bool was_enabled = (mask & (1u << i)) != 0;
            bool still_enabled = (candidate & (1u << i)) != 0;
            if (was_enabled && !still_enabled)
                blacklisted_since[i] = hop_counter; // transição habilitado->excluído
        }
    }

    for (uint8_t i = 0; i < count; i++)
    {
        if (candidate & (1u << i))
            continue; // já habilitado, nada a reincluir
        if (sensor_degraded_mask & (1u << i))
        {
            // sensor ainda segura o canal: o cooldown só começa quando ele soltar
            blacklisted_since[i] = hop_counter;
            continue;
        }
        if (hop_counter - blacklisted_since[i] >= recovery_cooldown_hops)
        {
            candidate |= (uint16_t)(1u << i);
            fail_count[i] = 0;
        }
    }

    return candidate;
}

#endif // HOPPING_H
