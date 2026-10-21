#ifndef CHANNEL_QUALITY_H
#define CHANNEL_QUALITY_H

// channel-quality-sensing: qualidade por canal medida pelo analisador,
// agnóstica à causa (jammer, Wi-Fi...). Lógica pura (testada em native_test).
// Entrada: ocupação 0..100 dos TABLE_SIZE canais a cada varredura (% de
// testCarrier() acesos). Saída: máscara de canais degradados, fundida pelo
// mestre em update_channel_mask() (`sensor_degraded_mask`).

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <cstdint>
#endif

#include "hopping.h"

// Limiares da calibração offline de 2026-10-21 (tools/validation/calibrate_chq.py):
// canal limpo nunca passou de 15 e jammer forte fica em 90-100; T_off = T_on/2
// absorve quedas pontuais do jammer. Wi-Fi moderado (~10-30) não é separável
// por ocupação -- fica para a evidência de ack na fusão.
static const uint8_t CHQ_WINDOW_SWEEPS = 10; // varreduras na janela (~9 s a ~0,9 s/varredura)
static const uint8_t CHQ_T_ON = 30;          // média da janela >= T_on  -> degradado
static const uint8_t CHQ_T_OFF = 15;         // média da janela <= T_off -> volta a utilizável

// Piso de canais ativos na fusão (min_active em update_channel_mask()).
static const uint8_t HOP_MIN_ACTIVE_CHANNELS = 4;

struct ChannelQualityState
{
    // Janela deslizante por canal (índice de HOPPING_CHANNELS, não o canal RF).
    // head[ch] é a próxima posição a escrever; com count == W ela guarda a
    // amostra mais antiga, subtraída de sum antes de ser sobrescrita.
    uint8_t ring[TABLE_SIZE][CHQ_WINDOW_SWEEPS];
    uint16_t sum[TABLE_SIZE]; // soma da janela (máx. 10 * 100 = 1000 -> cabe em uint16_t)
    uint8_t head[TABLE_SIZE];
    uint8_t count[TABLE_SIZE]; // amostras válidas na janela (satura em CHQ_WINDOW_SWEEPS)
    bool degraded[TABLE_SIZE];
};

inline void chq_reset(ChannelQualityState &s)
{
    for (uint8_t ch = 0; ch < TABLE_SIZE; ch++)
    {
        for (uint8_t k = 0; k < CHQ_WINDOW_SWEEPS; k++)
            s.ring[ch][k] = 0;
        s.sum[ch] = 0;
        s.head[ch] = 0;
        s.count[ch] = 0;
        s.degraded[ch] = false;
    }
}

/**
 * Empurra uma varredura (ocupação 0..100 na ordem de HOPPING_CHANNELS; acima
 * de 100 satura) e reavalia a histerese de cada canal. Só avalia com a janela
 * cheia, para uma amostra alta na partida não sinalizar:
 *   utilizável --(média >= CHQ_T_ON)--> degradado
 *   degradado  --(média <= CHQ_T_OFF)--> utilizável
 *   entre os dois limiares mantém o estado.
 */
inline void chq_push_sweep(ChannelQualityState &s, const uint8_t occ[TABLE_SIZE])
{
    for (uint8_t ch = 0; ch < TABLE_SIZE; ch++)
    {
        uint8_t v = occ[ch] > 100 ? 100 : occ[ch];

        if (s.count[ch] == CHQ_WINDOW_SWEEPS)
            s.sum[ch] = (uint16_t)(s.sum[ch] - s.ring[ch][s.head[ch]]); // sai a mais antiga
        else
            s.count[ch]++;
        s.ring[ch][s.head[ch]] = v;
        s.sum[ch] = (uint16_t)(s.sum[ch] + v);
        s.head[ch] = (uint8_t)((s.head[ch] + 1) % CHQ_WINDOW_SWEEPS);

        if (s.count[ch] < CHQ_WINDOW_SWEEPS)
            continue; // janela incompleta: permanece utilizável

        uint8_t mean = (uint8_t)(s.sum[ch] / CHQ_WINDOW_SWEEPS);
        if (!s.degraded[ch] && mean >= CHQ_T_ON)
            s.degraded[ch] = true;
        else if (s.degraded[ch] && mean <= CHQ_T_OFF)
            s.degraded[ch] = false;
    }
}

// Ocupação média atual do canal (0..100); 0 com janela vazia (telemetria `top=`).
inline uint8_t chq_mean(const ChannelQualityState &s, uint8_t ch)
{
    if (ch >= TABLE_SIZE || s.count[ch] == 0)
        return 0;
    return (uint8_t)(s.sum[ch] / s.count[ch]);
}

// Bit i = 1 quando HOPPING_CHANNELS[i] está degradado.
inline uint16_t chq_degraded_mask(const ChannelQualityState &s)
{
    uint16_t m = 0;
    for (uint8_t ch = 0; ch < TABLE_SIZE; ch++)
    {
        if (s.degraded[ch])
            m |= (uint16_t)(1u << ch);
    }
    return m;
}

#endif // CHANNEL_QUALITY_H
