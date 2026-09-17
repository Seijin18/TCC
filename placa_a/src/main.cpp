/*
 * Placa A (two-board-topology): mestre + analisador na mesma placa ESP32.
 *
 * Mestre roda no loop() padrão do Arduino (core 1, sensível a timing).
 * Analisador roda como task FreeRTOS pinada ao core 0.
 * Alerta de ataque (spectrum-attack-classification) trafega por uma flag
 * compartilhada (shared_state.h) em vez do GPIO físico usado no cenário de
 * 4 placas.
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
    // mestre_set_alert_source() já foi chamado quando mestre_setup() roda.
    mestre_set_alert_source(placaA_alert_get);
    analisador_set_alert_sink(placaA_alert_set);

    mestre_setup();

    xTaskCreatePinnedToCore(
        analisadorTask,
        "analisador",
        8192,
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
