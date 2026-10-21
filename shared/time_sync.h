#ifndef TIME_SYNC_H
#define TIME_SYNC_H

// slot-phase-servo: servo de fase de 1a ordem para o relogio de slots do
// ESCRAVO. Logica pura (inteira, sem Arduino/RF24) -- testada em
// env:native_test (test/test_time_sync/). O firmware so chama
// phase_servo_update() a cada FragmentPacket valido e soma o retorno em
// hop_start_at. Ver openspec/changes/slot-phase-servo/design.md.

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <cstdint>
#endif

#include "protocol.h" // SLOT_MS

// Convencao de sinal (design D1/D3):
//   e = fase_escravo - fase_mestre   [ms]
//   fase_escravo = agora - (hop_start_at + master_slot * SLOT_MS)
//   fase_mestre  = FragmentPacket.slot_phase_ms
// e > 0: o relogio do escravo esta ADIANTADO (a fronteira de slot dele vem
// antes da do mestre; os pacotes "chegam tarde" no slot dele). Somar +1 ms a
// hop_start_at ATRASA a fronteira do escravo e REDUZ e em 1 ms. Logo o ajuste
// devolvido por phase_servo_update() e somado a hop_start_at:
//   hop_start_at += phase_servo_update(...)

// "Fase desconhecida" no campo slot_phase_ms (valores validos: 0..SLOT_MS-1):
// o mestre manda isto no pacote de bootstrap, onde a ancora dele ainda nao
// existe (e fixada so quando o ack chega). O escravo ignora a amostra.
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

// Desvio filtrado em relacao ao alvo (ms, arredondado p/ zero) -- o "err" da
// telemetria. 0 enquanto o alvo nao foi capturado.
static inline int32_t phase_servo_err_ms(const PhaseServo &s)
{
    if (!phase_servo_target_ready(s))
        return 0;
    return (s.ema_fx - s.target_fx) / PHASE_FX;
}

// Alimenta UMA amostra de desvio de fase e devolve o ajuste (-1, 0 ou +1 ms)
// a somar em hop_start_at. Devolve 0 se: servo desligado; amostra atipica
// (|e| >= SLOT_MS/2 -- slip real de slot inteiro e tratado por
// escravo_reanchor()); alvo ainda sendo capturado (as primeiras
// PHASE_TARGET_SAMPLES amostras so acumulam); desvio filtrado < 1 ms; ou
// limite de taxa (1 passo por PHASE_MIN_STEP_INTERVAL_MS).
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

    // Captura do alvo: media das primeiras N amostras apos o reset. Absorve a
    // latencia fixa (ar + processamento) sem constante por placa (design D4).
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

    // Guarda de divergencia (design D5): desvio filtrado acima de 10 ms mesmo
    // depois de varias correcoes seguidas no mesmo sentido => o servo nao esta
    // fechando a malha (erro de sinal ou fonte de erro que ele nao controla).
    if (dev_abs > PHASE_DIVERGE_MS * PHASE_FX && s.run_len >= PHASE_DIVERGE_RUN)
    {
        s.enabled = false;
        s.just_disabled = true;
        return 0;
    }

    if (s.has_adj && (uint32_t)(now_ms - s.last_adj_ms) < PHASE_MIN_STEP_INTERVAL_MS)
        return 0;

    int8_t adj = dev_fx > 0 ? 1 : -1;
    // Mover a ancora de adj ms muda e (futuro) de -adj: desloca o acumulador
    // junto pra nao re-disparar de imediato.
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

// Posicao do mestre dentro do slot corrente, em ms, saturada em SLOT_MS-1
// (cabe em uint8_t; protege contra um envio que atravessou a fronteira).
static inline uint8_t phase_in_slot_ms(uint32_t now_ms, uint32_t anchor_ms, uint32_t slot)
{
    uint32_t p = (uint32_t)(now_ms - (anchor_ms + slot * SLOT_MS));
    if ((int32_t)p < 0) // relogio antes do inicio do slot (nao deveria ocorrer)
        return 0;
    return (uint8_t)(p > SLOT_MS - 1 ? SLOT_MS - 1 : p);
}

#endif // TIME_SYNC_H
