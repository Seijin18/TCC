// Testes unitarios de shared/channel_quality.h -- ocupacao por janela +
// histerese por canal (channel-quality-sensing, tarefa 2.1), sem hardware
// nem framework Arduino. Rodar com: pio test -e native_test -f test_channel_quality
#include <unity.h>
#include "channel_quality.h"

void setUp(void) {}
void tearDown(void) {}

// Empurra `n` varreduras com o mesmo valor em todos os canais.
static void push_uniform(ChannelQualityState &s, uint8_t value, uint8_t n)
{
    uint8_t occ[TABLE_SIZE];
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
        occ[i] = value;
    for (uint8_t k = 0; k < n; k++)
        chq_push_sweep(s, occ);
}

// Empurra `n` varreduras onde so o canal `ch` tem `value` e o resto 0.
static void push_single(ChannelQualityState &s, uint8_t ch, uint8_t value, uint8_t n)
{
    uint8_t occ[TABLE_SIZE] = {0};
    occ[ch] = value;
    for (uint8_t k = 0; k < n; k++)
        chq_push_sweep(s, occ);
}

static void test_constants_match_calibration(void)
{
    TEST_ASSERT_EQUAL_UINT8(10, CHQ_WINDOW_SWEEPS);
    TEST_ASSERT_EQUAL_UINT8(30, CHQ_T_ON);
    TEST_ASSERT_EQUAL_UINT8(15, CHQ_T_OFF);
}

static void test_reset_clears_everything(void)
{
    ChannelQualityState s;
    chq_reset(s);
    push_uniform(s, 100, CHQ_WINDOW_SWEEPS);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, chq_degraded_mask(s));
    chq_reset(s);
    TEST_ASSERT_EQUAL_UINT16(0, chq_degraded_mask(s));
    for (uint8_t i = 0; i < TABLE_SIZE; i++)
    {
        TEST_ASSERT_EQUAL_UINT8(0, s.count[i]);
        TEST_ASSERT_EQUAL_UINT16(0, s.sum[i]);
        TEST_ASSERT_EQUAL_UINT8(0, chq_mean(s, i));
    }
}

static void test_clean_channel_stays_clean(void)
{
    ChannelQualityState s;
    chq_reset(s);
    push_uniform(s, 5, 3 * CHQ_WINDOW_SWEEPS);
    TEST_ASSERT_EQUAL_UINT16(0, chq_degraded_mask(s));
    TEST_ASSERT_EQUAL_UINT8(5, chq_mean(s, 0));
}

static void test_full_occupancy_flags_only_when_window_full(void)
{
    ChannelQualityState s;
    chq_reset(s);
    // Ocupacao 100 desde a partida: media ja >= T_on, mas so vale com janela cheia.
    push_single(s, 4, 100, CHQ_WINDOW_SWEEPS - 1);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, chq_degraded_mask(s), "janela incompleta nao pode sinalizar");
    push_single(s, 4, 100, 1);
    TEST_ASSERT_EQUAL_UINT16(1u << 4, chq_degraded_mask(s));
}

static void test_single_spike_does_not_flag(void)
{
    ChannelQualityState s;
    chq_reset(s);
    uint8_t occ[TABLE_SIZE] = {0};
    occ[2] = 100;
    chq_push_sweep(s, occ); // 1 amostra 100, resto 0
    occ[2] = 0;
    for (uint8_t k = 1; k < CHQ_WINDOW_SWEEPS + 5; k++)
        chq_push_sweep(s, occ);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, chq_degraded_mask(s), "media 10 < T_on: nao deve sinalizar");
}

static void test_single_spike_on_first_sweep_with_full_window_does_not_flag(void)
{
    ChannelQualityState s;
    chq_reset(s);
    uint8_t occ[TABLE_SIZE] = {0};
    occ[2] = 100;
    chq_push_sweep(s, occ);
    occ[2] = 0;
    for (uint8_t k = 1; k < CHQ_WINDOW_SWEEPS; k++)
        chq_push_sweep(s, occ); // janela acaba de ficar cheia: soma 100, media 10
    TEST_ASSERT_EQUAL_UINT16(0, chq_degraded_mask(s));
}

static void test_ramp_crosses_t_on(void)
{
    ChannelQualityState s;
    chq_reset(s);
    // Janela cheia a 0, depois rampa: cada varredura nova troca um 0 por 100.
    // Media = 10*k; atinge T_on=30 na 3a varredura de 100.
    push_single(s, 7, 0, CHQ_WINDOW_SWEEPS);
    push_single(s, 7, 100, 1);
    TEST_ASSERT_EQUAL_UINT16(0, chq_degraded_mask(s)); // media 10
    push_single(s, 7, 100, 1);
    TEST_ASSERT_EQUAL_UINT16(0, chq_degraded_mask(s)); // media 20
    push_single(s, 7, 100, 1);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(1u << 7, chq_degraded_mask(s), "media 30 == T_on deve sinalizar");
}

static void test_drop_to_t_off_clears(void)
{
    ChannelQualityState s;
    chq_reset(s);
    push_single(s, 7, 100, CHQ_WINDOW_SWEEPS);
    TEST_ASSERT_EQUAL_UINT16(1u << 7, chq_degraded_mask(s));
    // Cada 0 novo derruba a media em 10: 90,80,...,20 (ainda degradado).
    push_single(s, 7, 0, CHQ_WINDOW_SWEEPS - 2);
    TEST_ASSERT_EQUAL_UINT8(20, chq_mean(s, 7));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(1u << 7, chq_degraded_mask(s), "media 20 > T_off: segura");
    push_single(s, 7, 0, 1);
    TEST_ASSERT_EQUAL_UINT8(10, chq_mean(s, 7));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, chq_degraded_mask(s), "media 10 <= T_off: volta a utilizavel");
}

static void test_exactly_t_off_clears(void)
{
    ChannelQualityState s;
    chq_reset(s);
    push_single(s, 1, 100, CHQ_WINDOW_SWEEPS);
    // janela com media exatamente 15: sum = 150 -> 10 amostras de 15
    push_single(s, 1, 15, CHQ_WINDOW_SWEEPS);
    TEST_ASSERT_EQUAL_UINT8(15, chq_mean(s, 1));
    TEST_ASSERT_EQUAL_UINT16(0, chq_degraded_mask(s));
}

static void test_hold_in_band_keeps_state(void)
{
    ChannelQualityState s;
    chq_reset(s);
    // Limpo + banda (T_off, T_on) -> continua limpo.
    push_single(s, 3, 22, 3 * CHQ_WINDOW_SWEEPS);
    TEST_ASSERT_EQUAL_UINT8(22, chq_mean(s, 3));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, chq_degraded_mask(s), "limpo na banda deve continuar limpo");

    // Degradado + banda -> continua degradado.
    push_single(s, 3, 100, CHQ_WINDOW_SWEEPS);
    TEST_ASSERT_EQUAL_UINT16(1u << 3, chq_degraded_mask(s));
    push_single(s, 3, 22, 3 * CHQ_WINDOW_SWEEPS);
    TEST_ASSERT_EQUAL_UINT8(22, chq_mean(s, 3));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(1u << 3, chq_degraded_mask(s), "degradado na banda deve continuar degradado");
}

static void test_jammer_with_single_sweep_dropouts_stays_flagged(void)
{
    ChannelQualityState s;
    chq_reset(s);
    // Padrao do design.md: 100,100,0,100 repetido -- queda pontual nao desliga.
    uint8_t pattern[4] = {100, 100, 0, 100};
    for (uint8_t k = 0; k < 40; k++)
        push_single(s, 5, pattern[k % 4], 1);
    TEST_ASSERT_EQUAL_UINT16(1u << 5, chq_degraded_mask(s));
}

static void test_mask_bit_mapping_and_independence(void)
{
    ChannelQualityState s;
    chq_reset(s);
    uint8_t occ[TABLE_SIZE] = {0};
    occ[0] = 100;
    occ[15] = 90;
    occ[8] = 40;
    occ[9] = 25; // abaixo de T_on
    for (uint8_t k = 0; k < CHQ_WINDOW_SWEEPS; k++)
        chq_push_sweep(s, occ);
    TEST_ASSERT_EQUAL_UINT16((1u << 0) | (1u << 8) | (1u << 15), chq_degraded_mask(s));

    // Canal 8 cai, 15 segue: so o bit 8 limpa.
    occ[8] = 0;
    for (uint8_t k = 0; k < CHQ_WINDOW_SWEEPS; k++)
        chq_push_sweep(s, occ);
    TEST_ASSERT_EQUAL_UINT16((1u << 0) | (1u << 15), chq_degraded_mask(s));
}

static void test_window_slides_old_samples_leave(void)
{
    ChannelQualityState s;
    chq_reset(s);
    // Mais de uma volta do anel: a soma deve refletir so as ultimas W amostras.
    for (uint8_t k = 0; k < 37; k++)
        push_single(s, 6, (uint8_t)(k % 7), 1);
    uint16_t expect = 0;
    for (uint8_t k = 27; k < 37; k++)
        expect = (uint16_t)(expect + (k % 7));
    TEST_ASSERT_EQUAL_UINT16(expect, s.sum[6]);
    TEST_ASSERT_EQUAL_UINT8(CHQ_WINDOW_SWEEPS, s.count[6]);
}

static void test_out_of_range_input_saturates(void)
{
    ChannelQualityState s;
    chq_reset(s);
    push_single(s, 2, 255, CHQ_WINDOW_SWEEPS);
    TEST_ASSERT_EQUAL_UINT8(100, chq_mean(s, 2));
    TEST_ASSERT_EQUAL_UINT16(1000, s.sum[2]);
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_constants_match_calibration);
    RUN_TEST(test_reset_clears_everything);
    RUN_TEST(test_clean_channel_stays_clean);
    RUN_TEST(test_full_occupancy_flags_only_when_window_full);
    RUN_TEST(test_single_spike_does_not_flag);
    RUN_TEST(test_single_spike_on_first_sweep_with_full_window_does_not_flag);
    RUN_TEST(test_ramp_crosses_t_on);
    RUN_TEST(test_drop_to_t_off_clears);
    RUN_TEST(test_exactly_t_off_clears);
    RUN_TEST(test_hold_in_band_keeps_state);
    RUN_TEST(test_jammer_with_single_sweep_dropouts_stays_flagged);
    RUN_TEST(test_mask_bit_mapping_and_independence);
    RUN_TEST(test_window_slides_old_samples_leave);
    RUN_TEST(test_out_of_range_input_saturates);
    return UNITY_END();
}
