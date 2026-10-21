#ifndef TIME_SYNC_H
#define TIME_SYNC_H

// slot-phase-servo: servo de fase de 1a ordem do relogio de slots do ESCRAVO.
// Logica pura (testada em native_test); o firmware soma o retorno de
// phase_servo_update() em hop_start_at a cada FragmentPacket valido.

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <cstdint>
#endif

#include "protocol.h" // SLOT_MS

// Sinal: e = fase_escravo - fase_mestre [ms], com
//   fase_escravo = agora - (hop_start_at + master_slot * SLOT_MS)
//   fase_mestre  = FragmentPacket.slot_phase_ms
// e > 0: escravo adiantado. Somar +1 ms a hop_start_at atrasa a fronteira dele
// e reduz e em 1 ms, entao: hop_start_at += phase_servo_update(...)

// slot_phase_ms desconhecido: o mestre manda no bootstrap (ancora ainda nao
// fixada); o escravo ignora a amostra. Validos: 0..SLOT_MS-1.
static const uint8_t PHASE_UNKNOWN = 0xFF;

static const int32_t PHASE_FX = 64;           // escala de ponto fixo: 1 ms = 64
static const uint8_t PHASE_TARGET_SAMPLES = 8; // N: amostras que formam o e_alvo
static const int32_t PHASE_STEP_THRESHOLD_MS = 1;  // |ema - alvo| minimo p/ corrigir
static const uint32_t PHASE_MIN_STEP_INTERVAL_MS = 200; // limite de taxa do passo
static const int32_t PHASE_GUARD_MS = (int32_t)(SLOT_MS / 2); // |e| >= isto: amostra atipica
static const int32_t PHASE_DIVERGE_MS = 10;   // |ema - alvo| acima disto (com correcoes) desliga
static const uint8_t PHASE_DIVERGE_RUN = 3;   // correcoes consecutivas de mesmo sinal p/ contar divergencia

struct PhaseServo
{
    int32_t ema_fx;          // EMA (alpha = 1/8) de e, em ms * PHASE_FX
    int32_t target_fx;       // e_alvo (ms * PHASE_FX); valido se target_n >= PHASE_TARGET_SAMPLES
    int32_t target_sum;      // soma das primeiras amostras (ms)
    uint8_t target_n;        // amostras do alvo capturadas
    bool have_ema;           // false ate a primeira amostra valida (EMA parte dela)
    bool has_adj;            // ja houve um passo (last_adj_ms valido)
    uint32_t last_adj_ms;    // instante do ultimo passo
    int8_t run_sign;         // sinal da corrente de passos consecutivos (0 = nenhuma)
    uint8_t run_len;         // tamanho dela
    bool enabled;            // false apos divergencia: servo parado ate phase_servo_reset()
    bool just_disabled;      // true no instante em que desligou; o chamador le e limpa (loga 1x)
};

static inline void phase_servo_reset(PhaseServo &s)
{
    s.ema_fx = 0;
    s.target_fx = 0;
    s.target_sum = 0;
    s.target_n = 0;
    s.have_ema = false;
    s.has_adj = false;
    s.last_adj_ms = 0;
    s.run_sign = 0;
    s.run_len = 0;
    s.enabled = true;
    s.just_disabled = false;
}

static inline bool phase_servo_target_ready(const PhaseServo &s)
{
    return s.target_n >= PHASE_TARGET_SAMPLES;
}

// Desvio filtrado em relacao ao alvo (ms) -- o "err" da telemetria.
static inline int32_t phase_servo_err_ms(const PhaseServo &s)
{
    if (!phase_servo_target_ready(s))
        return 0;
    return (s.ema_fx - s.target_fx) / PHASE_FX;
}

// Alimenta uma amostra de desvio e devolve o ajuste (-1, 0 ou +1 ms) para
// hop_start_at. 0 se: desligado; |e| >= SLOT_MS/2 (slip de slot inteiro e
// tratado por escravo_reanchor()); alvo em captura; desvio < 1 ms; ou
// limite de taxa.
static inline int8_t phase_servo_update(PhaseServo &s, int32_t e_ms, uint32_t now_ms)
{
    if (!s.enabled)
        return 0;
    if (e_ms >= PHASE_GUARD_MS || e_ms <= -PHASE_GUARD_MS)
        return 0;

    // EMA alpha = 1/8, em ponto fixo; a primeira amostra inicializa o filtro.
    int32_t e_fx = e_ms * PHASE_FX;
    if (!s.have_ema)
    {
        s.ema_fx = e_fx;
        s.have_ema = true;
    }
    else
    {
        s.ema_fx += (e_fx - s.ema_fx) / 8;
    }

    // Alvo = media das primeiras N amostras: absorve a latencia fixa
    // (ar + processamento) sem constante por placa.
    if (!phase_servo_target_ready(s))
    {
        s.target_sum += e_ms;
        s.target_n++;
        if (phase_servo_target_ready(s))
            s.target_fx = (s.target_sum * PHASE_FX) / (int32_t)PHASE_TARGET_SAMPLES;
        return 0;
    }

    int32_t dev_fx = s.ema_fx - s.target_fx;
    int32_t dev_abs = dev_fx < 0 ? -dev_fx : dev_fx;

    if (dev_abs < PHASE_STEP_THRESHOLD_MS * PHASE_FX)
    {
        s.run_sign = 0;
        s.run_len = 0;
        return 0;
    }

    // Divergencia: desvio > 10 ms apos varias correcoes no mesmo sentido =>
    // a malha nao fecha; desliga.
    if (dev_abs > PHASE_DIVERGE_MS * PHASE_FX && s.run_len >= PHASE_DIVERGE_RUN)
    {
        s.enabled = false;
        s.just_disabled = true;
        return 0;
    }

    if (s.has_adj && (uint32_t)(now_ms - s.last_adj_ms) < PHASE_MIN_STEP_INTERVAL_MS)
        return 0;

    int8_t adj = dev_fx > 0 ? 1 : -1;
    // Desloca o acumulador junto com a ancora p/ nao re-disparar de imediato.
    s.ema_fx -= (int32_t)adj * PHASE_STEP_THRESHOLD_MS * PHASE_FX;
    s.last_adj_ms = now_ms;
    s.has_adj = true;
    if (s.run_sign == adj)
    {
        if (s.run_len < 255)
            s.run_len++;
    }
    else
    {
        s.run_sign = adj;
        s.run_len = 1;
    }
    return adj;
}

// Posicao do mestre no slot corrente (ms), saturada em SLOT_MS-1.
static inline uint8_t phase_in_slot_ms(uint32_t now_ms, uint32_t anchor_ms, uint32_t slot)
{
    uint32_t p = (uint32_t)(now_ms - (anchor_ms + slot * SLOT_MS));
    if ((int32_t)p < 0)
        return 0;
    return (uint8_t)(p > SLOT_MS - 1 ? SLOT_MS - 1 : p);
}

#endif // TIME_SYNC_H
