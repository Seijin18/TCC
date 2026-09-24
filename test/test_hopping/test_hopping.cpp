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

// --- update_channel_mask (blacklist-decay) --------------------------------

static const uint8_t FAIL_THRESHOLD = 5;
static const uint32_t COOLDOWN = 200;

static void reset_state(uint8_t fail_count[], uint32_t blacklisted_since[])
{
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
    {
        fail_count[i] = 0;
        blacklisted_since[i] = 0;
    }
}

static void test_update_channel_mask_basic_exclusion(void)
{
    uint8_t fail_count[TABLE_SIZE];
    uint32_t blacklisted_since[TABLE_SIZE];
    reset_state(fail_count, blacklisted_since);
    fail_count[3] = FAIL_THRESHOLD;

    uint16_t result = update_channel_mask(0xFFFF, fail_count, blacklisted_since, 1000, FAIL_THRESHOLD, COOLDOWN);

    TEST_ASSERT_FALSE_MESSAGE(result & (1u << 3), "canal com falha no limiar deveria ser excluido");
    TEST_ASSERT_EQUAL_UINT32(1000, blacklisted_since[3]);
}

static void test_update_channel_mask_no_reinclusion_before_cooldown(void)
{
    uint8_t fail_count[TABLE_SIZE];
    uint32_t blacklisted_since[TABLE_SIZE];
    reset_state(fail_count, blacklisted_since);
    fail_count[3] = FAIL_THRESHOLD;

    uint16_t excluded = update_channel_mask(0xFFFF, fail_count, blacklisted_since, 1000, FAIL_THRESHOLD, COOLDOWN);
    // ainda dentro do cooldown -- fail_count[3] permanece >= limiar, entao a
    // chamada tambem nao deveria "empurrar" blacklisted_since pra frente.
    uint16_t result = update_channel_mask(excluded, fail_count, blacklisted_since, 1000 + COOLDOWN - 1, FAIL_THRESHOLD, COOLDOWN);

    TEST_ASSERT_FALSE_MESSAGE(result & (1u << 3), "canal nao deveria ser reincluido antes do cooldown vencer");
    TEST_ASSERT_EQUAL_UINT32(1000, blacklisted_since[3]);
}

static void test_update_channel_mask_reinclusion_after_cooldown(void)
{
    uint8_t fail_count[TABLE_SIZE];
    uint32_t blacklisted_since[TABLE_SIZE];
    reset_state(fail_count, blacklisted_since);
    fail_count[3] = FAIL_THRESHOLD;

    uint16_t excluded = update_channel_mask(0xFFFF, fail_count, blacklisted_since, 1000, FAIL_THRESHOLD, COOLDOWN);
    TEST_ASSERT_FALSE(excluded & (1u << 3));

    uint16_t result = update_channel_mask(excluded, fail_count, blacklisted_since, 1000 + COOLDOWN, FAIL_THRESHOLD, COOLDOWN);

    TEST_ASSERT_TRUE_MESSAGE(result & (1u << 3), "canal deveria ser reincluido apos o cooldown vencer");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, fail_count[3], "fail_count deveria ser zerado na reinclusao");
}

static void test_update_channel_mask_transition_writes_timestamp_once(void)
{
    uint8_t fail_count[TABLE_SIZE];
    uint32_t blacklisted_since[TABLE_SIZE];
    reset_state(fail_count, blacklisted_since);
    fail_count[3] = FAIL_THRESHOLD;

    uint16_t mask = update_channel_mask(0xFFFF, fail_count, blacklisted_since, 1000, FAIL_THRESHOLD, COOLDOWN);
    TEST_ASSERT_EQUAL_UINT32(1000, blacklisted_since[3]);

    // canal ja excluido, ainda com falha no limiar -- chamadas repetidas nao
    // podem reescrever blacklisted_since (senao o cooldown nunca vence).
    mask = update_channel_mask(mask, fail_count, blacklisted_since, 1050, FAIL_THRESHOLD, COOLDOWN);
    mask = update_channel_mask(mask, fail_count, blacklisted_since, 1100, FAIL_THRESHOLD, COOLDOWN);

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1000, blacklisted_since[3], "timestamp nao deveria ser reescrito enquanto ja excluido");
}

static void test_update_channel_mask_guard_never_empties_mask(void)
{
    uint8_t fail_count[TABLE_SIZE];
    uint32_t blacklisted_since[TABLE_SIZE];
    reset_state(fail_count, blacklisted_since);
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
        fail_count[i] = FAIL_THRESHOLD; // todos os canais habilitados falhando ao mesmo tempo

    uint16_t result = update_channel_mask(0xFFFF, fail_count, blacklisted_since, 1000, FAIL_THRESHOLD, COOLDOWN);

    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0xFFFF, result, "guarda deveria manter a mascara anterior em vez de esvaziar tudo");
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, blacklisted_since[i], "guarda nao deveria gravar timestamp de exclusao que nao ocorreu");
}

static void test_update_channel_mask_flapping_cycle(void)
{
    uint8_t fail_count[TABLE_SIZE];
    uint32_t blacklisted_since[TABLE_SIZE];
    reset_state(fail_count, blacklisted_since);
    fail_count[3] = FAIL_THRESHOLD;

    // 1) exclui
    uint16_t mask = update_channel_mask(0xFFFF, fail_count, blacklisted_since, 0, FAIL_THRESHOLD, COOLDOWN);
    TEST_ASSERT_FALSE(mask & (1u << 3));

    // 2) cooldown cumprido -> reinclui, fail_count zerado
    mask = update_channel_mask(mask, fail_count, blacklisted_since, COOLDOWN, FAIL_THRESHOLD, COOLDOWN);
    TEST_ASSERT_TRUE(mask & (1u << 3));
    TEST_ASSERT_EQUAL_UINT8(0, fail_count[3]);

    // 3) nova sequencia de falhas -> exclui de novo, com timestamp novo
    fail_count[3] = FAIL_THRESHOLD;
    mask = update_channel_mask(mask, fail_count, blacklisted_since, COOLDOWN + 10, FAIL_THRESHOLD, COOLDOWN);
    TEST_ASSERT_FALSE_MESSAGE(mask & (1u << 3), "canal deveria ser excluido de novo apos nova sequencia de falhas");
    TEST_ASSERT_EQUAL_UINT32(COOLDOWN + 10, blacklisted_since[3]);

    // 4) antes do segundo cooldown vencer, continua excluido
    mask = update_channel_mask(mask, fail_count, blacklisted_since, COOLDOWN + 10 + COOLDOWN - 1, FAIL_THRESHOLD, COOLDOWN);
    TEST_ASSERT_FALSE(mask & (1u << 3));

    // 5) segundo cooldown vencido -> reinclui de novo, sem estado inconsistente
    mask = update_channel_mask(mask, fail_count, blacklisted_since, COOLDOWN + 10 + COOLDOWN, FAIL_THRESHOLD, COOLDOWN);
    TEST_ASSERT_TRUE_MESSAGE(mask & (1u << 3), "canal deveria ser reincluido de novo apos o segundo cooldown");
}

// adaptive-rendezvous (2026-09-24): update_channel_mask() generalizado com
// `count` explicito para ser reusado sobre PAIRING_CANDIDATES (8 entradas),
// nao só HOPPING_CHANNELS (16) -- estes dois testes confirmam que um
// `count` menor que TABLE_SIZE funciona igual (exclusão/reinclusão) e que
// bits fora de `count` nunca são tocados (não fazem parte do universo
// considerado pela função com esse `count`).

static void test_update_channel_mask_smaller_count_basic_exclusion(void)
{
    const uint8_t CANDIDATE_COUNT = 8;
    uint8_t fail_count[CANDIDATE_COUNT];
    uint32_t blacklisted_since[CANDIDATE_COUNT];
    for (uint8_t i = 0; i < CANDIDATE_COUNT; i++)
    {
        fail_count[i] = 0;
        blacklisted_since[i] = 0;
    }
    fail_count[5] = FAIL_THRESHOLD;

    uint16_t mask = update_channel_mask(0xFF, fail_count, blacklisted_since, 1000, FAIL_THRESHOLD, COOLDOWN, CANDIDATE_COUNT);

    TEST_ASSERT_FALSE_MESSAGE(mask & (1u << 5), "candidato com falha no limiar deveria ser excluido mesmo com count < TABLE_SIZE");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0xDF, mask, "bits fora de count nao deveriam ser afetados");
}

static void test_update_channel_mask_smaller_count_guard_and_reinclusion(void)
{
    const uint8_t CANDIDATE_COUNT = 8;
    uint8_t fail_count[CANDIDATE_COUNT];
    uint32_t blacklisted_since[CANDIDATE_COUNT];
    for (uint8_t i = 0; i < CANDIDATE_COUNT; i++)
    {
        fail_count[i] = FAIL_THRESHOLD; // todos os 8 candidatos falhando ao mesmo tempo
        blacklisted_since[i] = 0;
    }

    uint16_t mask = update_channel_mask(0xFF, fail_count, blacklisted_since, 1000, FAIL_THRESHOLD, COOLDOWN, CANDIDATE_COUNT);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0xFF, mask, "guarda deveria manter todos os 8 candidatos em vez de esvaziar tudo");

    // exclui só o candidato 2, confirma reinclusao apos cooldown com count=8
    for (uint8_t i = 0; i < CANDIDATE_COUNT; i++)
        fail_count[i] = 0;
    fail_count[2] = FAIL_THRESHOLD;
    mask = update_channel_mask(0xFF, fail_count, blacklisted_since, 1000, FAIL_THRESHOLD, COOLDOWN, CANDIDATE_COUNT);
    TEST_ASSERT_FALSE(mask & (1u << 2));

    mask = update_channel_mask(mask, fail_count, blacklisted_since, 1000 + COOLDOWN, FAIL_THRESHOLD, COOLDOWN, CANDIDATE_COUNT);
    TEST_ASSERT_TRUE_MESSAGE(mask & (1u << 2), "candidato deveria ser reincluido apos o cooldown, com count < TABLE_SIZE");
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
    RUN_TEST(test_update_channel_mask_basic_exclusion);
    RUN_TEST(test_update_channel_mask_no_reinclusion_before_cooldown);
    RUN_TEST(test_update_channel_mask_reinclusion_after_cooldown);
    RUN_TEST(test_update_channel_mask_transition_writes_timestamp_once);
    RUN_TEST(test_update_channel_mask_guard_never_empties_mask);
    RUN_TEST(test_update_channel_mask_flapping_cycle);
    RUN_TEST(test_update_channel_mask_smaller_count_basic_exclusion);
    RUN_TEST(test_update_channel_mask_smaller_count_guard_and_reinclusion);
    return UNITY_END();
}
