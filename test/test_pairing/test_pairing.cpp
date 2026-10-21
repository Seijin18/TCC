// Testes unitarios de shared/pairing.h -- logica pura do handshake v2
// (pairing-handshake-hardening, D3/tarefa 3.3), sem hardware nem framework
// Arduino. Rodar com: pio test -e native_test
#include <unity.h>
#include "pairing.h"

void setUp(void) {}
void tearDown(void) {}

static const uint16_t NONCE_A = 0x1234;
static const uint16_t NONCE_B = 0x5678;
static const uint16_t SEED_A = 42;
static const uint16_t SEED_B = 7;

// --- pair_decide_escravo --------------------------------------------------

static void test_decide_new_session_when_not_provisional(void)
{
    // Escravo ainda escaneando (nao provisorio) -- toda sonda valida (ver
    // certa) inicia uma sessao nova, nunca REACK (nao ha sessao anterior
    // pra reconhecer).
    PairDecision d = pair_decide_escravo(false, 0, PAIRING_PROTO_VER, NONCE_A);
    TEST_ASSERT_EQUAL_INT(PAIR_ADOPT_NEW, d);
}

static void test_decide_new_session_ignores_previous_session_nonce_when_not_provisional(void)
{
    // Mesmo que `session_nonce` receba um valor de sessao anterior por
    // engano, `provisional=false` domina -- sempre ADOPT_NEW.
    PairDecision d = pair_decide_escravo(false, NONCE_A, PAIRING_PROTO_VER, NONCE_A);
    TEST_ASSERT_EQUAL_INT(PAIR_ADOPT_NEW, d);
}

static void test_decide_reack_same_nonce_while_provisional(void)
{
    // Escravo ja pareado-provisorio na sessao NONCE_A -- uma re-sonda com o
    // MESMO nonce e resposta perdida do lado do mestre -> re-ack idempotente.
    PairDecision d = pair_decide_escravo(true, NONCE_A, PAIRING_PROTO_VER, NONCE_A);
    TEST_ASSERT_EQUAL_INT(PAIR_REACK, d);
}

static void test_decide_adopt_new_session_on_different_nonce_while_provisional(void)
{
    // Escravo pareado-provisorio na sessao NONCE_A -- sonda com nonce
    // DIFERENTE (NONCE_B) significa que o mestre recomecou o pareamento;
    // adota a nova sessao.
    PairDecision d = pair_decide_escravo(true, NONCE_A, PAIRING_PROTO_VER, NONCE_B);
    TEST_ASSERT_EQUAL_INT(PAIR_ADOPT_NEW, d);
}

static void test_decide_ignore_version_mismatch_while_scanning(void)
{
    PairDecision d = pair_decide_escravo(false, 0, PAIRING_PROTO_VER + 1, NONCE_A);
    TEST_ASSERT_EQUAL_INT(PAIR_IGNORE, d);
}

static void test_decide_ignore_version_mismatch_while_provisional_same_nonce(void)
{
    // Versao errada e ignorada mesmo quando o nonce bateria com a sessao
    // corrente (re-ack) -- `ver` e checado antes de qualquer outra coisa.
    PairDecision d = pair_decide_escravo(true, NONCE_A, PAIRING_PROTO_VER + 1, NONCE_A);
    TEST_ASSERT_EQUAL_INT(PAIR_IGNORE, d);
}

static void test_decide_ignore_version_mismatch_while_provisional_different_nonce(void)
{
    PairDecision d = pair_decide_escravo(true, NONCE_A, PAIRING_PROTO_VER + 1, NONCE_B);
    TEST_ASSERT_EQUAL_INT(PAIR_IGNORE, d);
}

// --- pair_accept_ack_mestre ------------------------------------------------

static void test_accept_matching_ack(void)
{
    bool ok = pair_accept_ack_mestre(NONCE_A, SEED_A, PAIRING_PROTO_VER, NONCE_A, SEED_A);
    TEST_ASSERT_TRUE(ok);
}

static void test_reject_stale_ack_wrong_nonce(void)
{
    // Nonce de uma tentativa anterior/diferente -- ack nao pertence a esta
    // sessao (Scenario "Resposta que nao pertence a tentativa corrente").
    bool ok = pair_accept_ack_mestre(NONCE_A, SEED_A, PAIRING_PROTO_VER, NONCE_B, SEED_A);
    TEST_ASSERT_FALSE(ok);
}

static void test_reject_ack_wrong_version(void)
{
    bool ok = pair_accept_ack_mestre(NONCE_A, SEED_A, PAIRING_PROTO_VER + 1, NONCE_A, SEED_A);
    TEST_ASSERT_FALSE(ok);
}

static void test_reject_ack_divergent_seed(void)
{
    // Nonce e versao corretos, mas a seed ecoada diverge da que a sonda
    // corrente carregou -- nao aceita mesmo assim (os tres campos tem que
    // bater exatamente, ver design.md D3).
    bool ok = pair_accept_ack_mestre(NONCE_A, SEED_A, PAIRING_PROTO_VER, NONCE_A, SEED_B);
    TEST_ASSERT_FALSE(ok);
}

static void test_reject_ack_wrong_nonce_and_seed(void)
{
    bool ok = pair_accept_ack_mestre(NONCE_A, SEED_A, PAIRING_PROTO_VER, NONCE_B, SEED_B);
    TEST_ASSERT_FALSE(ok);
}

// --- half_open_should_recover ---------------------------------------------

static void test_half_open_below_threshold_does_not_recover(void)
{
    TEST_ASSERT_FALSE(half_open_should_recover(0, HALF_OPEN_SESSIONS));
    TEST_ASSERT_FALSE(half_open_should_recover(1, HALF_OPEN_SESSIONS));
    TEST_ASSERT_FALSE(half_open_should_recover(HALF_OPEN_SESSIONS - 1, HALF_OPEN_SESSIONS));
}

static void test_half_open_at_threshold_recovers(void)
{
    TEST_ASSERT_TRUE(half_open_should_recover(HALF_OPEN_SESSIONS, HALF_OPEN_SESSIONS));
}

static void test_half_open_above_threshold_recovers(void)
{
    TEST_ASSERT_TRUE(half_open_should_recover(HALF_OPEN_SESSIONS + 1, HALF_OPEN_SESSIONS));
    TEST_ASSERT_TRUE(half_open_should_recover(255, HALF_OPEN_SESSIONS));
}

static void test_half_open_boundary_with_arbitrary_threshold(void)
{
    // Fronteira independente da constante: limiar 1 dispara na primeira
    // sessao; limiar 3 so na terceira.
    TEST_ASSERT_FALSE(half_open_should_recover(0, 1));
    TEST_ASSERT_TRUE(half_open_should_recover(1, 1));
    TEST_ASSERT_FALSE(half_open_should_recover(2, 3));
    TEST_ASSERT_TRUE(half_open_should_recover(3, 3));
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_decide_new_session_when_not_provisional);
    RUN_TEST(test_decide_new_session_ignores_previous_session_nonce_when_not_provisional);
    RUN_TEST(test_decide_reack_same_nonce_while_provisional);
    RUN_TEST(test_decide_adopt_new_session_on_different_nonce_while_provisional);
    RUN_TEST(test_decide_ignore_version_mismatch_while_scanning);
    RUN_TEST(test_decide_ignore_version_mismatch_while_provisional_same_nonce);
    RUN_TEST(test_decide_ignore_version_mismatch_while_provisional_different_nonce);
    RUN_TEST(test_accept_matching_ack);
    RUN_TEST(test_reject_stale_ack_wrong_nonce);
    RUN_TEST(test_reject_ack_wrong_version);
    RUN_TEST(test_reject_ack_divergent_seed);
    RUN_TEST(test_reject_ack_wrong_nonce_and_seed);
    RUN_TEST(test_half_open_below_threshold_does_not_recover);
    RUN_TEST(test_half_open_at_threshold_recovers);
    RUN_TEST(test_half_open_above_threshold_recovers);
    RUN_TEST(test_half_open_boundary_with_arbitrary_threshold);
    return UNITY_END();
}
