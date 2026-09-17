/*
 * Placa B (two-board-topology): escravo + jammer na mesma placa ESP32.
 *
 * Escravo roda no loop() padrão do Arduino (core 1, sensível a timing).
 * Jammer roda como task FreeRTOS pinada ao core 0.
 *
 * Pinagem: ver docs/pinout.md ("Placa B — escravo + jammer").
 */

#include <Arduino.h>
#include "shared_state.h"
#include "../../escravo/src/node.h"
#include "../../jammer/src/node.h"

static void jammerTask(void *pvParameters)
{
    (void)pvParameters;
    jammer_setup();
    for (;;)
    {
        jammer_loop();
        vTaskDelay(1);
    }
}

void setup()
{
    // Registrar o callback ANTES de escravo_setup(): o guard do blink de
    // LED_BUILTIN (GPIO 2 == MISO do HSPI do jammer, two-board-topology
    // seção 7) só decide corretamente se escravo_set_ready_sink() já foi
    // chamado quando escravo_setup() roda.
    escravo_set_ready_sink(placaB_ready_set);
    jammer_set_ready_source(placaB_ready_get);

    escravo_setup();

    xTaskCreatePinnedToCore(
        jammerTask,
        "jammer",
        16384,
        nullptr,
        tskIDLE_PRIORITY + 1,
        nullptr,
        0 // core 0 — o loop() padrão do Arduino roda no core 1
    );
}

void loop()
{
    escravo_loop();
}
