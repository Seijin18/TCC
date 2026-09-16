/*
 * Placa B (two-board-topology): escravo + jammer na mesma placa ESP32.
 *
 * Escravo roda no loop() padrão do Arduino (core 1, sensível a timing).
 * Jammer roda como task FreeRTOS pinada ao core 0.
 *
 * Pinagem: ver docs/pinout.md ("Placa B — escravo + jammer").
 */

#include <Arduino.h>
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
    escravo_setup();

    xTaskCreatePinnedToCore(
        jammerTask,
        "jammer",
        8192,
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
