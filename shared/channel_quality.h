#ifndef CHANNEL_QUALITY_H
#define CHANNEL_QUALITY_H

// channel-quality-sensing (tarefa 2.1): qualidade por canal medida pelo
// analisador, agnóstica à causa (jammer, Wi-Fi, outro enlace) -- ver
// openspec/changes/channel-quality-sensing/design.md (D1). Lógica pura,
// só inteiros, sem hardware nem Arduino, para ser testada em
// env:native_test (mesmo padrão de shared/hopping.h).
//
// Entrada: a cada varredura completa do analisador, a ocupação de cada um
// dos TABLE_SIZE canais de HOPPING_CHANNELS, 0..100 (% das amostras de
// testCarrier() que acenderam -- é exatamente o valor da célula do
// spectrum). Saída: máscara de 16 bits com os canais degradados, que o
// mestre funde com a evidência do próprio enlace (update_channel_mask() em
// hopping.h, parâmetro `sensor_degraded_mask`).

// Nos firmwares (ESP32/Arduino) usa os typedefs de Arduino.h; no ambiente de
// testes nativo (env:native_test, sem framework Arduino) cai para <cstdint>.
#ifdef ARDUINO
#include <Arduino.h>
#else
#include <cstdint>
#endif

#include "hopping.h"

// Limiares escolhidos pela calibração offline de 2026-10-21 (design.md,
// "Resultado da calibração 2026-10-21"; tools/validation/calibrate_chq.py).
// Ocupação de canal limpo nunca passou de 15 (p99,9 = 11) e jammer forte
// fica em 90-100, sem sobreposição; T_off = T_on/2 absorve quedas pontuais
// de uma varredura do jammer sem perder o flag. A faixa moderada de Wi-Fi
// (~10-30) NÃO é separável por ocupação -- fica para a evidência de ack do
// enlace (fusão, D2/D4).
static const uint8_t CHQ_WINDOW_SWEEPS = 10; // varreduras na janela (~9 s a ~0,9 s/varredura)
static const uint8_t CHQ_T_ON = 30;          // média da janela >= T_on  -> degradado
static const uint8_t CHQ_T_OFF = 15;         // média da janela <= T_off -> volta a utilizável

// Piso de canais ativos usado na fusão (design.md D4 / spec "Guarda de
// banda mínima"): update_channel_mask(..., min_active) devolve à máscara os
// canais de melhor histórico até completar este número.
static const uint8_t HOP_MIN_ACTIVE_CHANNELS = 4;

struct ChannelQualityState
{
    // Janela deslizante por canal. `ring[ch]` guarda as últimas
    // CHQ_WINDOW_SWEEPS ocupações do canal `ch` (índice de HOPPING_CHANNELS,
    // não o número do canal RF). `head[ch]` é a PRÓXIMA posição a escrever:
    // a cada push grava-se em ring[ch][head[ch]] e head avança módulo W.
    // Enquanto count < W a posição ainda não foi usada (vale 0 após reset);
    // com count == W a posição apontada por head é a amostra MAIS ANTIGA,
    // que é subtraída de `sum` antes de ser sobrescrita.
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
 * Empurra uma varredura (TABLE_SIZE valores de ocupação 0..100, na ordem de
 * HOPPING_CHANNELS) e reavalia a histerese de cada canal. Valores acima de
 * 100 são saturados em 100 (defesa contra entrada fora da faixa).
 *
 * Histerese só é avaliada com a janela CHEIA (count == W): antes disso o
 * canal fica utilizável, para uma única amostra alta na partida não
 * sinalizar. Com a janela cheia, média inteira = sum / W:
 *   utilizável --(média >= CHQ_T_ON)--> degradado
 *   degradado  --(média <= CHQ_T_OFF)--> utilizável
 *   entre T_off e T_on mantém o estado.
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

// Ocupação média atual do canal (0..100); 0 enquanto a janela está vazia.
// Divide pelas amostras já presentes (telemetria `top=<canal:ocupação>`).
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
