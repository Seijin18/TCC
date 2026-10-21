/*
 * Placa A (two-board-topology): mestre + analisador na mesma placa ESP32.
 *
 * Mestre roda no loop() padrão do Arduino (core 1, sensível a timing).
 * Analisador roda como task FreeRTOS pinada ao core 0.
 * A máscara de canais degradados do analisador (channel-quality-sensing)
 * trafega por uma variável compartilhada (shared_state.h) em vez do GPIO
 * usado no cenário de 4 placas.
 *
 * Pinagem: ver docs/pinout.md ("Placa A — mestre + analisador").
 */

#include <Arduino.h>
#include "shared_state.h"
#include "../../mestre/src/node.h"
#include "../../analisador/src/node.h"

static void analisadorTask(void *pvParameters)
{
    (void)pvParameters;
    analisador_setup();
    for (;;)
    {
        analisador_loop();
        vTaskDelay(1);
    }
}

void setup()
{
    // Registrar os callbacks ANTES de mestre_setup(): o guard de pinMode do
    // pino de alerta e do blink de LED_BUILTIN (GPIO 2 == MISO do HSPI do
    // analisador, two-board-topology seção 7) só decide corretamente se
    // mestre_set_chq_source() já foi chamado quando mestre_setup() roda.
    // channel-quality-sensing: a placa combinada passa a máscara, não o booleano
    mestre_set_chq_source(placaA_chq_get);
    analisador_set_chq_sink(placaA_chq_set);

    mestre_setup();

    xTaskCreatePinnedToCore(
        analisadorTask,
        "analisador",
        16384,
        nullptr,
        tskIDLE_PRIORITY + 1,
        nullptr,
        0 // core 0 — o loop() padrão do Arduino roda no core 1
    );
}

void loop()
{
    mestre_loop();
}
