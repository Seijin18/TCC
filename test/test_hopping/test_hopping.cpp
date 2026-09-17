// Testes unitarios de shared/hopping.h — logica pura de selecao de canal,
// sem hardware nem framework Arduino. Rodar com: pio test -e native_test
#include <unity.h>
#include "hopping.h"

void setUp(void) {}
void tearDown(void) {}

// --- get_hop_channel ---------------------------------------------------

static void test_get_hop_channel_returns_table_entries(void)
{
    for (uint32_t counter = 0; counter < TABLE_SIZE * 3; counter++)
    {
        uint8_t ch = get_hop_channel(counter);
        TEST_ASSERT_EQUAL_UINT8(HOPPING_CHANNELS[counter % TABLE_SIZE], ch);
    }
}

static void test_get_hop_channel_cycles_through_full_table(void)
{
    // Em TABLE_SIZE contadores consecutivos, cada canal da tabela deve
    // aparecer exatamente uma vez (garante que o modulo nao pula/repete).
    bool seen[TABLE_SIZE] = {false};
    for (uint32_t counter = 0; counter < TABLE_SIZE; counter++)
    {
        uint8_t ch = get_hop_channel(counter);
        for (uint8_t i = 0; i < TABLE_SIZE; i++)
        {
            if (HOPPING_CHANNELS[i] == ch)
            {
                TEST_ASSERT_FALSE_MESSAGE(seen[i], "canal repetido dentro de um ciclo completo");
                seen[i] = true;
            }
        }
    }
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
        TEST_ASSERT_TRUE_MESSAGE(seen[i], "canal da tabela nunca visitado em um ciclo completo");
}

// --- get_next_channel ----------------------------------------------------

static void test_get_next_channel_is_deterministic(void)
{
    // Mesma seed + mesmo contador -> sempre o mesmo canal (mestre e escravo
    // dependem disso para permanecerem sincronizados).
    for (uint16_t seed = 0; seed < 5; seed++)
        for (uint32_t counter = 0; counter < 20; counter++)
            TEST_ASSERT_EQUAL_UINT8(get_next_channel(seed, counter), get_next_channel(seed, counter));
}

static void test_get_next_channel_returns_table_entries(void)
{
    for (uint16_t seed = 0; seed < 10; seed++)
        for (uint32_t counter = 0; counter < TABLE_SIZE; counter++)
        {
            uint8_t ch = get_next_channel(seed, counter);
            bool found = false;
            for (uint8_t i = 0; i < TABLE_SIZE; i++)
                if (HOPPING_CHANNELS[i] == ch)
                    found = true;
            TEST_ASSERT_TRUE_MESSAGE(found, "get_next_channel retornou canal fora de HOPPING_CHANNELS");
        }
}

static void test_get_next_channel_cycles_through_full_table_per_seed(void)
{
    // Para uma seed fixa, TABLE_SIZE contadores consecutivos devem cobrir
    // a tabela inteira sem repeticao (mesma garantia de get_hop_channel,
    // agora com o deslocamento da seed).
    for (uint16_t seed = 0; seed < 5; seed++)
    {
        bool seen[TABLE_SIZE] = {false};
        for (uint32_t counter = 0; counter < TABLE_SIZE; counter++)
        {
            uint8_t ch = get_next_channel(seed, counter);
            for (uint8_t i = 0; i < TABLE_SIZE; i++)
            {
                if (HOPPING_CHANNELS[i] == ch)
                {
                    TEST_ASSERT_FALSE_MESSAGE(seen[i], "canal repetido dentro de um ciclo completo (com seed)");
                    seen[i] = true;
                }
            }
        }
        for (uint8_t i = 0; i < TABLE_SIZE; i++)
            TEST_ASSERT_TRUE_MESSAGE(seen[i], "canal nunca visitado em um ciclo completo (com seed)");
    }
}

static void test_get_next_channel_different_seeds_offset_sequence(void)
{
    // Seeds diferentes devem produzir sequencias defasadas (nao identicas)
    // para o mesmo conjunto de contadores — é o que da a cada sessao um
    // padrao de salto distinto.
    bool any_difference = false;
    for (uint32_t counter = 0; counter < TABLE_SIZE; counter++)
    {
        if (get_next_channel(0, counter) != get_next_channel(1, counter))
        {
            any_difference = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(any_difference, "seeds distintas produziram exatamente a mesma sequencia de canais");
}

// --- get_next_active_channel (dynamic-seed-blacklist) --------------------

static void test_get_next_active_channel_full_mask_matches_get_next_channel(void)
{
    // Mascara cheia (0xFFFF) deve se comportar identico a get_next_channel.
    for (uint16_t seed = 0; seed < 5; seed++)
        for (uint32_t counter = 0; counter < TABLE_SIZE * 2; counter++)
            TEST_ASSERT_EQUAL_UINT8(get_next_channel(seed, counter),
                                     get_next_active_channel(seed, counter, 0xFFFF));
}

static void test_get_next_active_channel_only_returns_enabled_channels(void)
{
    // Mascara habilita so os indices pares (0,2,4,...,14).
    uint16_t mask = 0;
    for (uint8_t i = 0; i < TABLE_SIZE; i += 2)
        mask |= (1u << i);

    for (uint32_t counter = 0; counter < TABLE_SIZE * 3; counter++)
    {
        uint8_t ch = get_next_active_channel(7, counter, mask);
        bool enabled = false;
        for (uint8_t i = 0; i < TABLE_SIZE; i += 2)
            if (HOPPING_CHANNELS[i] == ch)
                enabled = true;
        TEST_ASSERT_TRUE_MESSAGE(enabled, "get_next_active_channel retornou canal fora da mascara");
    }
}

static void test_get_next_active_channel_single_channel_mask(void)
{
    // Mascara com um unico canal habilitado deve sempre retornar esse canal.
    uint8_t only_index = 5;
    uint16_t mask = (uint16_t)(1u << only_index);

    for (uint32_t counter = 0; counter < 30; counter++)
        TEST_ASSERT_EQUAL_UINT8(HOPPING_CHANNELS[only_index], get_next_active_channel(3, counter, mask));
}

static void test_get_next_active_channel_zero_mask_falls_back_to_full_table(void)
{
    // Guarda: mascara vazia nao deve travar (divisao por zero) -- degrada
    // para a tabela cheia.
    for (uint32_t counter = 0; counter < TABLE_SIZE; counter++)
    {
        uint8_t ch = get_next_active_channel(1, counter, 0);
        bool found = false;
        for (uint8_t i = 0; i < TABLE_SIZE; i++)
            if (HOPPING_CHANNELS[i] == ch)
                found = true;
        TEST_ASSERT_TRUE_MESSAGE(found, "mascara zero nao degradou para a tabela cheia");
    }
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_get_hop_channel_returns_table_entries);
    RUN_TEST(test_get_hop_channel_cycles_through_full_table);
    RUN_TEST(test_get_next_channel_is_deterministic);
    RUN_TEST(test_get_next_channel_returns_table_entries);
    RUN_TEST(test_get_next_channel_cycles_through_full_table_per_seed);
    RUN_TEST(test_get_next_channel_different_seeds_offset_sequence);
    RUN_TEST(test_get_next_active_channel_full_mask_matches_get_next_channel);
    RUN_TEST(test_get_next_active_channel_only_returns_enabled_channels);
    RUN_TEST(test_get_next_active_channel_single_channel_mask);
    RUN_TEST(test_get_next_active_channel_zero_mask_falls_back_to_full_table);
    return UNITY_END();
}
