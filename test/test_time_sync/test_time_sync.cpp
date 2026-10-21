// Testes unitarios de shared/time_sync.h -- servo de fase de slot
// (slot-phase-servo, tarefa 1.2), sem hardware nem framework Arduino. Rodar
// com: pio test -e native_test
#include <unity.h>
#include <cmath>
#include <cstdlib>
#include "time_sync.h"

void setUp(void) {}
void tearDown(void) {}

// PRNG deterministico (LCG) -- testes reprodutiveis, sem depender de rand().
static uint32_t g_rng = 12345;
static uint32_t rng_next(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}
// uniforme inteiro em [-amp, +amp]
static int32_t rng_uniform(int32_t amp)
{
    return (int32_t)(rng_next() % (uint32_t)(2 * amp + 1)) - amp;
}

// Simula o escravo: erro "verdadeiro" (ms, real) = offset inicial + deriva -
// soma dos passos ja aplicados a ancora. A amostra entregue ao servo e esse
// erro arredondado a 1 ms (a resolucao de millis()) mais ruido opcional.
struct SimResult
{
    double max_abs_err;    // maior |erro verdadeiro - erro inicial| depois da captura do alvo
    uint32_t steps;        // passos != 0
    int32_t net_adj;       // soma dos passos
    uint32_t min_gap_ms;   // menor intervalo entre dois passos
    int max_step_abs;      // maior |passo|
    bool disabled;
};

static SimResult simulate(double ppm, uint32_t duration_ms, int32_t noise_amp, double offset0)
{
    PhaseServo s;
    phase_servo_reset(s);
    SimResult r = {0, 0, 0, 0xFFFFFFFFu, 0, false};
    const uint32_t dt = 10; // ~100 fragmentos/s
    double applied = 0;
    uint32_t last_step_t = 0;
    bool had_step = false;
    for (uint32_t t = 0; t <= duration_ms; t += dt)
    {
        double e_true = offset0 + ppm * 1e-6 * (double)t - applied;
        int32_t e = (int32_t)std::lround(e_true) + (noise_amp ? rng_uniform(noise_amp) : 0);
        int8_t adj = phase_servo_update(s, e, t);
        if (adj != 0)
        {
            r.steps++;
            r.net_adj += adj;
            applied += adj;
            int a = adj < 0 ? -adj : adj;
            if (a > r.max_step_abs)
                r.max_step_abs = a;
            if (had_step && (t - last_step_t) < r.min_gap_ms)
                r.min_gap_ms = t - last_step_t;
            last_step_t = t;
            had_step = true;
        }
        if (phase_servo_target_ready(s))
        {
            double err = std::fabs((offset0 + ppm * 1e-6 * (double)t - applied) - offset0);
            if (err > r.max_abs_err)
                r.max_abs_err = err;
        }
    }
    r.disabled = !s.enabled;
    return r;
}

static const uint32_t THIRTY_MIN_MS = 30u * 60u * 1000u;

// --- deriva constante ------------------------------------------------------

static void test_drift_plus_50ppm_30min_bounded(void)
{
    // 50 ppm = 0,05 ms/s => 90 ms em 30 min => ~90 passos de 1 ms.
    SimResult r = simulate(+50.0, THIRTY_MIN_MS, 0, 7.0);
    TEST_ASSERT_FALSE(r.disabled);
    TEST_ASSERT_TRUE(r.max_abs_err <= 3.0);
    TEST_ASSERT_INT_WITHIN(5, 90, (int)r.steps);
    // todos os passos no sentido que compensa a deriva
    TEST_ASSERT_INT_WITHIN(5, 90, r.net_adj);
}

static void test_drift_minus_50ppm_30min_bounded(void)
{
    SimResult r = simulate(-50.0, THIRTY_MIN_MS, 0, -4.0);
    TEST_ASSERT_FALSE(r.disabled);
    TEST_ASSERT_TRUE(r.max_abs_err <= 3.0);
    TEST_ASSERT_INT_WITHIN(5, 90, (int)r.steps);
    TEST_ASSERT_INT_WITHIN(5, -90, r.net_adj);
}

static void test_drift_78ppm_with_noise_bounded(void)
{
    // deriva maxima medida em bancada (~78 ppm) com ruido de +-1 ms
    // (quantizacao/jitter tipicos): ainda limitado.
    SimResult r = simulate(-78.0, THIRTY_MIN_MS, 1, 3.0);
    TEST_ASSERT_FALSE(r.disabled);
    TEST_ASSERT_TRUE(r.max_abs_err <= 3.0);
    // 0,078 ms/s * 1800 s = ~140 ms
    TEST_ASSERT_INT_WITHIN(15, -140, r.net_adj);
}

// --- passo e taxa limitados ------------------------------------------------

static void test_step_never_over_1ms_and_rate_limited(void)
{
    // deriva exagerada (5000 ppm = 5 ms/s, no limite do servo) para forcar
    // passos o mais seguido possivel.
    SimResult r = simulate(+5000.0, 20000, 0, 0.0);
    TEST_ASSERT_TRUE(r.steps > 0);
    TEST_ASSERT_EQUAL_INT(1, r.max_step_abs);
    TEST_ASSERT_TRUE(r.min_gap_ms >= PHASE_MIN_STEP_INTERVAL_MS);
}

static void test_step_rate_limit_direct(void)
{
    // erro constante grande (dentro da guarda): o servo deve devolver passos
    // espacados de >= 200 ms, nunca dois seguidos.
    PhaseServo s;
    phase_servo_reset(s);
    for (uint32_t i = 0; i < PHASE_TARGET_SAMPLES; i++)
        TEST_ASSERT_EQUAL_INT(0, phase_servo_update(s, 0, i * 10));
    int steps = 0;
    uint32_t last = 0;
    for (uint32_t t = 100; t < 2000; t += 10)
    {
        int8_t adj = phase_servo_update(s, 6, t);
        if (adj != 0)
        {
            if (steps > 0)
                TEST_ASSERT_TRUE(t - last >= PHASE_MIN_STEP_INTERVAL_MS);
            TEST_ASSERT_EQUAL_INT(1, adj);
            last = t;
            steps++;
        }
    }
    TEST_ASSERT_TRUE(steps >= 2);
}

// --- captura do alvo -------------------------------------------------------

static void test_no_correction_before_target_captured(void)
{
    PhaseServo s;
    phase_servo_reset(s);
    for (uint32_t i = 0; i < PHASE_TARGET_SAMPLES; i++)
    {
        TEST_ASSERT_FALSE(phase_servo_target_ready(s));
        // erro grande e crescente: ainda assim nenhuma correcao
        TEST_ASSERT_EQUAL_INT(0, phase_servo_update(s, 3 + (int32_t)i, 1000 + i * 10));
    }
    TEST_ASSERT_TRUE(phase_servo_target_ready(s));
}

static void test_constant_latency_offset_absorbed_by_target(void)
{
    // latencia fixa de 7 ms (e constante = 7): vira o alvo, nenhuma correcao.
    SimResult r = simulate(0.0, 60000, 0, 7.0);
    TEST_ASSERT_EQUAL_UINT32(0, r.steps);
    TEST_ASSERT_EQUAL_INT(0, r.net_adj);
}

// --- ruido -----------------------------------------------------------------

static void test_uniform_noise_2ms_no_runaway(void)
{
    // ruido uniforme +-2 ms (bem pior que a quantizacao real de 1 ms) em torno
    // de fase fixa, por 30 min, varias sementes: a ancora nao pode andar. Com
    // sigma do EMA ~0,37 ms contra o limiar de 1 ms, o servo pode oscilar
    // +-1 ms em torno da posicao certa (passo espurio desfeito pelo seguinte),
    // mas a deriva liquida fica ~0, o excesso limitado e ele nao se desliga.
    for (uint32_t seed = 1; seed <= 4; seed++)
    {
        g_rng = seed * 77;
        SimResult r = simulate(0.0, THIRTY_MIN_MS, 2, 0.0);
        TEST_ASSERT_FALSE(r.disabled);
        TEST_ASSERT_TRUE(r.max_abs_err <= 3.0);
        TEST_ASSERT_INT_WITHIN(3, 0, r.net_adj);
        TEST_ASSERT_EQUAL_INT(1, r.steps ? r.max_step_abs : 1);
    }
}

static void test_noise_1ms_almost_no_corrections(void)
{
    // ruido de +-1 ms (a quantizacao de millis()): praticamente nenhuma
    // correcao espuria em 30 min (limiar 1 ms = ~3 sigma do EMA).
    for (uint32_t seed = 1; seed <= 4; seed++)
    {
        g_rng = seed * 77;
        SimResult r = simulate(0.0, THIRTY_MIN_MS, 1, 0.0);
        TEST_ASSERT_FALSE(r.disabled);
        TEST_ASSERT_TRUE(r.steps <= 20);
        TEST_ASSERT_INT_WITHIN(2, 0, r.net_adj);
    }
}

// --- guardas ---------------------------------------------------------------

static void test_sample_over_half_slot_ignored(void)
{
    PhaseServo s;
    phase_servo_reset(s);
    uint32_t t = 0;
    for (uint32_t i = 0; i < PHASE_TARGET_SAMPLES; i++, t += 10)
        phase_servo_update(s, 0, t);
    int32_t ema_before = s.ema_fx;
    TEST_ASSERT_EQUAL_INT(0, phase_servo_update(s, (int32_t)SLOT_MS / 2, t + 1000));
    TEST_ASSERT_EQUAL_INT(0, phase_servo_update(s, -(int32_t)SLOT_MS / 2, t + 2000));
    TEST_ASSERT_EQUAL_INT(0, phase_servo_update(s, 29, t + 3000));
    TEST_ASSERT_EQUAL_INT32(ema_before, s.ema_fx); // filtro intocado
    TEST_ASSERT_TRUE(s.enabled);
}

static void test_isolated_outlier_sample_does_not_trigger_step(void)
{
    // um unico pacote atipico (|e| < meio slot mas longe da fase) no meio de
    // amostras boas nao gera passo: o EMA o dilui (alpha = 1/8).
    PhaseServo s;
    phase_servo_reset(s);
    uint32_t t = 0;
    for (uint32_t i = 0; i < 50; i++, t += 10)
        TEST_ASSERT_EQUAL_INT(0, phase_servo_update(s, 2, t));
    TEST_ASSERT_EQUAL_INT(0, phase_servo_update(s, 8, t)); // +6 ms de salto: ema move 0,75 ms
    t += 10;
    for (uint32_t i = 0; i < 50; i++, t += 10)
        TEST_ASSERT_EQUAL_INT(0, phase_servo_update(s, 2, t));
}

static void test_divergence_disables_servo(void)
{
    // erro forcado a crescer ignorando as correcoes (como com sinal errado):
    // o servo deve se desligar e sinalizar just_disabled uma vez.
    PhaseServo s;
    phase_servo_reset(s);
    uint32_t t = 0;
    for (uint32_t i = 0; i < PHASE_TARGET_SAMPLES; i++, t += 10)
        phase_servo_update(s, 0, t);
    bool saw_disabled = false;
    int32_t e = 0;
    for (uint32_t i = 0; i < 4000 && !saw_disabled; i++, t += 10)
    {
        if (i % 20 == 0 && e < 14) // +1 ms a cada 200 ms, sem compensar os passos
            e++;
        phase_servo_update(s, e, t);
        if (!s.enabled)
            saw_disabled = true;
    }
    TEST_ASSERT_TRUE(saw_disabled);
    TEST_ASSERT_TRUE(s.just_disabled);
    // desligado: nao corrige mais, mesmo com erro enorme persistente
    for (uint32_t i = 0; i < 100; i++, t += 300)
        TEST_ASSERT_EQUAL_INT(0, phase_servo_update(s, 12, t));
    // reset religa
    phase_servo_reset(s);
    TEST_ASSERT_TRUE(s.enabled);
    TEST_ASSERT_FALSE(s.just_disabled);
}

static void test_normal_drift_never_disables(void)
{
    // desvio inicial grande (9 ms, abaixo do limite) e corrigido sem desligar.
    PhaseServo s;
    phase_servo_reset(s);
    uint32_t t = 0;
    for (uint32_t i = 0; i < PHASE_TARGET_SAMPLES; i++, t += 10)
        phase_servo_update(s, 0, t);
    double applied = 0;
    for (; t < 20000; t += 10)
    {
        int32_t e = (int32_t)std::lround(9.0 - applied);
        applied += phase_servo_update(s, e, t);
    }
    TEST_ASSERT_TRUE(s.enabled);
    TEST_ASSERT_TRUE(applied >= 8.0);
}

// --- phase_in_slot_ms ------------------------------------------------------

static void test_phase_in_slot_basic_and_saturation(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, phase_in_slot_ms(1000, 1000, 0));
    TEST_ASSERT_EQUAL_UINT8(12, phase_in_slot_ms(1000 + 3 * SLOT_MS + 12, 1000, 3));
    TEST_ASSERT_EQUAL_UINT8(SLOT_MS - 1, phase_in_slot_ms(1000 + SLOT_MS + 5, 1000, 0)); // satura
    TEST_ASSERT_EQUAL_UINT8(0, phase_in_slot_ms(999, 1000, 0)); // antes do slot
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_drift_plus_50ppm_30min_bounded);
    RUN_TEST(test_drift_minus_50ppm_30min_bounded);
    RUN_TEST(test_drift_78ppm_with_noise_bounded);
    RUN_TEST(test_step_never_over_1ms_and_rate_limited);
    RUN_TEST(test_step_rate_limit_direct);
    RUN_TEST(test_no_correction_before_target_captured);
    RUN_TEST(test_constant_latency_offset_absorbed_by_target);
    RUN_TEST(test_uniform_noise_2ms_no_runaway);
    RUN_TEST(test_noise_1ms_almost_no_corrections);
    RUN_TEST(test_sample_over_half_slot_ignored);
    RUN_TEST(test_isolated_outlier_sample_does_not_trigger_step);
    RUN_TEST(test_divergence_disables_servo);
    RUN_TEST(test_normal_drift_never_disables);
    RUN_TEST(test_phase_in_slot_basic_and_saturation);
    return UNITY_END();
}
