// Testes unitarios de shared/resync.h -- decisoes puras de estacionamento
// (hop-resync, tarefa 1.4), sem hardware nem framework Arduino. Rodar com:
// pio test -e native_test
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

static const uint32_t PARK_MAX_MS = 3960; // (TABLE_SIZE + 2) * HOP_TIMEOUT_MS, ver shared/protocol.h

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
    return UNITY_END();
}
