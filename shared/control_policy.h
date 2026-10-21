#ifndef CONTROL_POLICY_H
#define CONTROL_POLICY_H

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <cstdint>
#endif

// blacklist-decay (achado de bancada 2026-09-24): logica pura de "devo
// propor um controle agora, e qual" -- extraida de
// mestre/src/node.h::maybe_enqueue_control() pra poder ser testada sem
// hardware/Arduino (mesmo padrao de shared/hopping.h).
//
// Achado: com jamming real em bancada (logs/events.log ~00:19), o mestre
// mandava `rekey_tx kind=blacklist mask=0xFFFB apply=289`, o escravo
// ackava, e no PROXIMO hop o mestre mandava a MESMA mascara de novo com
// apply=290, depois 291, 292... pra sempre -- nenhum dado fluia, so
// controle. Causa: apos o ack, a troca so fica AGENDADA
// (g_mask_apply_scheduled/g_seed_apply_scheduled, aplicada mais tarde por
// apply_scheduled_changes() quando hop_counter >= apply_at), mas
// maybe_enqueue_control() comparava o candidato so contra a mascara JA
// APLICADA (g_channel_mask) -- que so muda quando a troca agendada
// consolida. Ate la o candidato recalculado a cada hop continua diferente
// da mascara aplicada, uma NOVA proposta e enfileirada com apply_at mais
// distante, o escravo reagenda pro hop mais tarde, e o ponto de aplicacao
// foge indefinidamente.
//
// Fix: enquanto uma troca de seed OU mascara estiver agendada (ack
// recebido, ainda nao aplicada), nao propor nada novo -- so reavalia depois
// que apply_scheduled_changes() consolidar a mudanca. Um reseed pendente
// (`reseed_needed`) nao se perde: a flag so e limpa quando a proposta e de
// fato enfileirada, entao continua pendente e sai assim que o guard libera.
// A comparacao de mascara usa o alvo EFETIVO (valor agendado, se houver;
// senao o aplicado) -- nunca reproposta uma mascara identica a ja aplicada
// ou ja agendada.

enum class ControlProposalKind
{
    kNone,
    kSeed,
    kBlacklist
};

struct ControlProposalDecision
{
    ControlProposalKind kind;
    uint16_t mask_value; // valido apenas quando kind == kBlacklist
};

/**
 * `effective_mask` deve ser o alvo JA agendado (g_mask_apply_value) quando
 * `mask_apply_scheduled` for true, ou a mascara ja aplicada (g_channel_mask)
 * caso contrario -- e responsabilidade do chamador (que tem essas duas
 * variaveis) montar esse valor; esta funcao permanece pura e sem estado.
 */
inline ControlProposalDecision decide_control_proposal(
    bool control_pending,
    bool fixed_channel_mode,
    bool mitigation_enabled,
    bool seed_apply_scheduled,
    bool mask_apply_scheduled,
    bool reseed_needed,
    uint16_t candidate_mask,
    uint16_t effective_mask)
{
    if (control_pending)
        return {ControlProposalKind::kNone, 0};

    // scenario-automation, Cenario A/B: mesma guarda ja existente em
    // maybe_enqueue_control() -- modo fixo ou mitigacao desligada nunca
    // propoe nada.
    if (fixed_channel_mode)
        return {ControlProposalKind::kNone, 0};
    if (!mitigation_enabled)
        return {ControlProposalKind::kNone, 0};

    // blacklist-decay (achado de bancada 2026-09-24): troca ainda agendada
    // (ack recebido, apply_at no futuro) -- nao propoe nada ate consolidar,
    // senao o candidato nunca bate com o alvo ainda-nao-aplicado e o
    // apply_at foge pra frente a cada hop (control flood).
    if (seed_apply_scheduled || mask_apply_scheduled)
        return {ControlProposalKind::kNone, 0};

    if (reseed_needed)
        return {ControlProposalKind::kSeed, 0};

    if (candidate_mask != effective_mask)
        return {ControlProposalKind::kBlacklist, candidate_mask};

    return {ControlProposalKind::kNone, 0};
}

// -------------------------------------------------------------
// channel-quality-sensing: gatilho de reseed
// -------------------------------------------------------------
// Seed nova só quando a blacklist não basta: muitos canais degradados ao mesmo
// tempo, ou miss alto mesmo com a blacklist aplicada. Limiares conservadores,
// ainda não calibrados com dado.
static const uint8_t CHQ_RESEED_MIN_DEGRADED = 6;  // >= 6 de 16 canais degradados juntos
static const uint8_t CHQ_RESEED_MISS_PCT = 40;     // miss da janela > 40% conta como "alto"
static const uint8_t CHQ_RESEED_MISS_WINDOWS = 3;  // janelas CONSECUTIVAS altas após a blacklist

/**
 * Contador de janelas consecutivas com miss alto depois de uma blacklist
 * aplicada (o chamador guarda `prev`; uma chamada por janela). Zera sem
 * blacklist aplicada ou com janela abaixo do limiar. Satura em 255.
 */
inline uint8_t chq_update_post_blacklist_miss_windows(uint8_t prev, bool blacklist_applied, uint8_t miss_pct)
{
    if (!blacklist_applied || miss_pct <= CHQ_RESEED_MISS_PCT)
        return 0;
    return prev == 255 ? 255 : (uint8_t)(prev + 1);
}

/**
 * O reseed se justifica? Alimenta `reseed_needed` de decide_control_proposal(),
 * que continua dono das guardas (pendente/agendado/modo fixo/mitigação).
 */
inline bool should_reseed_for_channel_quality(uint8_t degraded_count, uint8_t post_blacklist_miss_windows)
{
    return degraded_count >= CHQ_RESEED_MIN_DEGRADED ||
           post_blacklist_miss_windows >= CHQ_RESEED_MISS_WINDOWS;
}

#endif // CONTROL_POLICY_H
