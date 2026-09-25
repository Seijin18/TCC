// Testes unitarios de shared/control_policy.h -- logica pura de "devo
// propor um controle agora, e qual", sem hardware nem framework Arduino.
// Rodar com: pio test -e native_test -f test_control_policy
//
// blacklist-decay (achado de bancada 2026-09-24): estes casos cobrem o
// control flood observado em bancada -- mesma mascara reproposta a cada
// hop com apply_at fugindo pra frente enquanto a troca anterior ainda
// estava so agendada, nunca aplicada (ver comentario no topo de
// shared/control_policy.h e tasks.md, secao 4.6).
#include <unity.h>
#include "control_policy.h"

void setUp(void) {}
void tearDown(void) {}

// --- guardas de "nao propor nada" ---------------------------------------

static void test_no_proposal_when_control_already_pending(void)
{
    ControlProposalDecision d = decide_control_proposal(
        /*control_pending=*/true, /*fixed=*/false, /*mitigation=*/true,
        /*seed_scheduled=*/false, /*mask_scheduled=*/false, /*reseed_needed=*/true,
        /*candidate=*/0xFFFB, /*effective=*/0xFFFF);
    TEST_ASSERT_TRUE(d.kind == ControlProposalKind::kNone);
}

static void test_no_proposal_in_fixed_channel_mode(void)
{
    ControlProposalDecision d = decide_control_proposal(
        false, /*fixed=*/true, true,
        false, false, true,
        0xFFFB, 0xFFFF);
    TEST_ASSERT_TRUE(d.kind == ControlProposalKind::kNone);
}

static void test_no_proposal_when_mitigation_disabled(void)
{
    ControlProposalDecision d = decide_control_proposal(
        false, false, /*mitigation=*/false,
        false, false, true,
        0xFFFB, 0xFFFF);
    TEST_ASSERT_TRUE(d.kind == ControlProposalKind::kNone);
}

// --- o bug de bancada: nao repropor enquanto algo esta agendado ---------

static void test_no_proposal_while_mask_change_scheduled(void)
{
    // Candidato ainda difere do que ja foi APLICADO (g_channel_mask), mas
    // uma troca pra esse mesmo candidato ja foi ackada e esta agendada --
    // nao deveria gerar uma nova proposta (era exatamente isto que fugia
    // pra sempre em bancada).
    ControlProposalDecision d = decide_control_proposal(
        false, false, true,
        /*seed_scheduled=*/false, /*mask_scheduled=*/true, /*reseed_needed=*/false,
        /*candidate=*/0xFFFB, /*effective=*/0xFFFB);
    TEST_ASSERT_TRUE(d.kind == ControlProposalKind::kNone);
}

static void test_no_proposal_while_seed_change_scheduled(void)
{
    ControlProposalDecision d = decide_control_proposal(
        false, false, true,
        /*seed_scheduled=*/true, /*mask_scheduled=*/false, /*reseed_needed=*/false,
        0xFFFB, 0xFFFF);
    TEST_ASSERT_TRUE(d.kind == ControlProposalKind::kNone);
}

static void test_pending_reseed_not_lost_while_mask_scheduled(void)
{
    // reseed_needed continua true (quem chama nao limpa a flag quando o
    // guard barra a proposta) -- assim que mask_scheduled cair, a proxima
    // chamada deve finalmente propor o seed.
    ControlProposalDecision blocked = decide_control_proposal(
        false, false, true,
        false, /*mask_scheduled=*/true, /*reseed_needed=*/true,
        0xFFFB, 0xFFFB);
    TEST_ASSERT_TRUE(blocked.kind == ControlProposalKind::kNone);

    ControlProposalDecision released = decide_control_proposal(
        false, false, true,
        false, /*mask_scheduled=*/false, /*reseed_needed=*/true,
        0xFFFB, 0xFFFB);
    TEST_ASSERT_TRUE(released.kind == ControlProposalKind::kSeed);
}

// --- comparacao contra o alvo efetivo, nao so o aplicado ----------------

static void test_no_reproposal_when_candidate_equals_effective_scheduled_mask(void)
{
    // Nada agendado no momento desta chamada (ja consolidou), mas o
    // candidato bate com o que acabou de ser aplicado -- nao deve repropor.
    ControlProposalDecision d = decide_control_proposal(
        false, false, true,
        false, false, false,
        /*candidate=*/0xFFFB, /*effective=*/0xFFFB);
    TEST_ASSERT_TRUE(d.kind == ControlProposalKind::kNone);
}

static void test_proposes_blacklist_when_candidate_differs_from_effective_mask(void)
{
    ControlProposalDecision d = decide_control_proposal(
        false, false, true,
        false, false, false,
        /*candidate=*/0xFFFB, /*effective=*/0xFFFF);
    TEST_ASSERT_TRUE(d.kind == ControlProposalKind::kBlacklist);
    TEST_ASSERT_EQUAL_UINT16(0xFFFB, d.mask_value);
}

// --- prioridade: reseed antes de blacklist -------------------------------

static void test_reseed_takes_priority_over_blacklist(void)
{
    ControlProposalDecision d = decide_control_proposal(
        false, false, true,
        false, false, /*reseed_needed=*/true,
        /*candidate=*/0xFFFB, /*effective=*/0xFFFF);
    TEST_ASSERT_TRUE(d.kind == ControlProposalKind::kSeed);
}

// --- regressao do flood: chamadas repetidas nunca reagendam sozinhas ----

static void test_repeated_calls_with_mask_still_scheduled_never_flood(void)
{
    // Simula varios hops seguidos com a troca ja ackada e agendada, sem
    // apply_scheduled_changes() ainda ter rodado -- decide_control_proposal()
    // nao deve nunca retornar kBlacklist neste intervalo.
    for (int hop = 0; hop < 20; hop++)
    {
        ControlProposalDecision d = decide_control_proposal(
            false, false, true,
            false, /*mask_scheduled=*/true, false,
            /*candidate=*/0xFFFB, /*effective=*/0xFFFB);
        TEST_ASSERT_TRUE_MESSAGE(d.kind == ControlProposalKind::kNone,
                                  "nao deveria repropor enquanto a troca anterior ainda esta agendada");
    }
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_proposal_when_control_already_pending);
    RUN_TEST(test_no_proposal_in_fixed_channel_mode);
    RUN_TEST(test_no_proposal_when_mitigation_disabled);
    RUN_TEST(test_no_proposal_while_mask_change_scheduled);
    RUN_TEST(test_no_proposal_while_seed_change_scheduled);
    RUN_TEST(test_pending_reseed_not_lost_while_mask_scheduled);
    RUN_TEST(test_no_reproposal_when_candidate_equals_effective_scheduled_mask);
    RUN_TEST(test_proposes_blacklist_when_candidate_differs_from_effective_mask);
    RUN_TEST(test_reseed_takes_priority_over_blacklist);
    RUN_TEST(test_repeated_calls_with_mask_still_scheduled_never_flood);
    return UNITY_END();
}
