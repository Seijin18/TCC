// Testes unitarios de shared/resync.h -- decisoes puras de estacionamento
// (hop-resync, tarefas 1.4 e 1.5), sem hardware nem framework Arduino. Rodar
// com: pio test -e native_test
#include <unity.h>
#include "resync.h"

void setUp(void) {}
void tearDown(void) {}

// --- resync_should_park (D1) ----------------------------------------------

static void test_should_park_false_below_threshold(void)
{
    TEST_ASSERT_FALSE(resync_should_park(0, 2));
    TEST_ASSERT_FALSE(resync_should_park(1, 2));
}

static void test_should_park_true_at_threshold(void)
{
    TEST_ASSERT_TRUE(resync_should_park(2, 2));
}

static void test_should_park_true_above_threshold(void)
{
    // Nunca deveria acontecer na pratica (o chamador estaciona assim que o
    // limiar e atingido, sem deixar o contador subir mais) -- mas a funcao
    // nao deve "desarmar" se isso ocorrer por algum motivo.
    TEST_ASSERT_TRUE(resync_should_park(5, 2));
}

static void test_should_park_respects_different_threshold(void)
{
    TEST_ASSERT_FALSE(resync_should_park(2, 3));
    TEST_ASSERT_TRUE(resync_should_park(3, 3));
}

// --- resync_park_expired (D2) ----------------------------------------------

static const uint32_t PARK_MAX_MS = 540; // (TABLE_SIZE + 2) * ATTEMPT_TIMEOUT_MS, ver shared/protocol.h (hop-ack-decoupling)

static void test_park_not_expired_before_limit(void)
{
    TEST_ASSERT_FALSE(resync_park_expired(PARK_MAX_MS - 1, PARK_MAX_MS));
    TEST_ASSERT_FALSE(resync_park_expired(0, PARK_MAX_MS));
}

static void test_park_expired_at_limit(void)
{
    TEST_ASSERT_TRUE(resync_park_expired(PARK_MAX_MS, PARK_MAX_MS));
}

static void test_park_expired_after_limit(void)
{
    TEST_ASSERT_TRUE(resync_park_expired(PARK_MAX_MS + 100000, PARK_MAX_MS));
}

// --- resync_pick_park_channel (tarefa 1.5) ---------------------------------

static const uint8_t QTABLE = 4; // tabela pequena so pra estes testes

static void reset_quality(uint8_t ok[], uint8_t miss[], uint8_t n)
{
    for (uint8_t i = 0; i < n; i++)
    {
        ok[i] = 0;
        miss[i] = 0;
    }
}

static void test_pick_park_channel_noisy_channel_with_more_oks_still_loses(void)
{
    // Achado de bancada (design.md "Implementado"): canal 2 recebeu MAIS
    // fragmentos brutos que o canal 1, mas tambem falhou muito mais (jamado
    // por Wi-Fi) -- score = ok - 2*miss precisa preferir o canal mais
    // "quieto" mesmo com menos recepcoes totais.
    uint8_t ok[QTABLE], miss[QTABLE];
    reset_quality(ok, miss, QTABLE);
    ok[1] = 5;
    miss[1] = 0; // score 5
    ok[2] = 10;
    miss[2] = 8; // score 10 - 16 = -6 -- muito jamado, apesar de mais ok

    uint8_t picked = resync_pick_park_channel(ok, miss, 0xFFFF, 0, QTABLE);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, picked, "canal ruidoso com mais ok nao deveria vencer o mais quieto");
}

static void test_pick_park_channel_masked_out_never_picked(void)
{
    uint8_t ok[QTABLE], miss[QTABLE];
    reset_quality(ok, miss, QTABLE);
    ok[2] = 20;
    miss[2] = 0; // melhor canal de todos, mas desabilitado pela mascara
    ok[1] = 3;
    miss[1] = 0;

    uint16_t mask = (uint16_t)(0xFFFF & ~(1u << 2)); // tudo habilitado, exceto o indice 2
    uint8_t picked = resync_pick_park_channel(ok, miss, mask, 0, QTABLE);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, picked, "canal fora da mascara nunca deveria ser escolhido");
}

static void test_pick_park_channel_none_eligible_falls_back(void)
{
    uint8_t ok[QTABLE], miss[QTABLE];
    reset_quality(ok, miss, QTABLE); // nenhum canal com ok>0

    uint8_t picked = resync_pick_park_channel(ok, miss, 0xFFFF, 3, QTABLE);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, picked, "sem candidato elegivel deveria cair no fallback_idx");
}

static void test_pick_park_channel_only_ok_greater_than_zero_eligible(void)
{
    uint8_t ok[QTABLE], miss[QTABLE];
    reset_quality(ok, miss, QTABLE);
    miss[0] = 5; // so tem miss, nunca recebeu nada -- nao elegivel
    ok[3] = 1;   // unico com alguma recepcao valida registrada

    uint8_t picked = resync_pick_park_channel(ok, miss, 0xFFFF, 2, QTABLE);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, picked, "canal so com miss (ok=0) nao deveria ser elegivel");
}

static void test_pick_park_channel_ties_prefer_lower_miss(void)
{
    uint8_t ok[QTABLE], miss[QTABLE];
    reset_quality(ok, miss, QTABLE);
    // Mesmo score (4-2*1=2 e 2-2*0=2), miss menor desempata.
    ok[0] = 4;
    miss[0] = 1;
    ok[1] = 2;
    miss[1] = 0;

    uint8_t picked = resync_pick_park_channel(ok, miss, 0xFFFF, 0, QTABLE);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, picked, "empate de score deveria desempatar pelo menor miss");
}

static void test_pick_park_channel_full_tie_prefers_lower_index(void)
{
    uint8_t ok[QTABLE], miss[QTABLE];
    reset_quality(ok, miss, QTABLE);
    ok[2] = 3;
    miss[2] = 1;
    ok[3] = 3;
    miss[3] = 1; // score e miss identicos ao canal 2

    uint8_t picked = resync_pick_park_channel(ok, miss, 0xFFFF, 0, QTABLE);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, picked, "empate total (score e miss) deveria preferir o menor indice");
}

static void test_pick_park_channel_default_table_size_matches_table_size(void)
{
    // Sem passar table_size explicito, o default precisa ser TABLE_SIZE
    // (shared/hopping.h) -- confirma que o parametro default nao regrediu.
    uint8_t ok[TABLE_SIZE], miss[TABLE_SIZE];
    reset_quality(ok, miss, TABLE_SIZE);
    ok[TABLE_SIZE - 1] = 1;

    uint8_t picked = resync_pick_park_channel(ok, miss, 0xFFFF, 0);
    TEST_ASSERT_EQUAL_UINT8(TABLE_SIZE - 1, picked);
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_should_park_false_below_threshold);
    RUN_TEST(test_should_park_true_at_threshold);
    RUN_TEST(test_should_park_true_above_threshold);
    RUN_TEST(test_should_park_respects_different_threshold);
    RUN_TEST(test_park_not_expired_before_limit);
    RUN_TEST(test_park_expired_at_limit);
    RUN_TEST(test_park_expired_after_limit);
    RUN_TEST(test_pick_park_channel_noisy_channel_with_more_oks_still_loses);
    RUN_TEST(test_pick_park_channel_masked_out_never_picked);
    RUN_TEST(test_pick_park_channel_none_eligible_falls_back);
    RUN_TEST(test_pick_park_channel_only_ok_greater_than_zero_eligible);
    RUN_TEST(test_pick_park_channel_ties_prefer_lower_miss);
    RUN_TEST(test_pick_park_channel_full_tie_prefers_lower_index);
    RUN_TEST(test_pick_park_channel_default_table_size_matches_table_size);
    return UNITY_END();
}
