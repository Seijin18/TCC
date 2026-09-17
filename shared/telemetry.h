#ifndef TELEMETRY_H
#define TELEMETRY_H

// telemetry-logging: linha de evento canonica, parseavel por
// tools/logger/main.py, emitida lado a lado com os Serial.printf humanos
// ja existentes (nao os substitui).
//
// Formato: @<src>;t=<millis>;e=<evento>;<chave>=<valor>;...
//
// Nas placas combinadas (two-board-topology) duas tasks FreeRTOS de
// nucleos diferentes escrevem na mesma Serial -- tlm_printf() serializa a
// linha inteira sob um mutex para nao entrelacar com outra chamada
// concorrente (mesma classe de problema que ja afeta o heartbeat '.' do
// jammer colando na linha seguinte).

#ifdef ARDUINO
#include <Arduino.h>
#include <stdarg.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t g_tlm_mutex = nullptr;

static inline void tlm_lock()
{
    if (!g_tlm_mutex)
        g_tlm_mutex = xSemaphoreCreateMutex();
    if (g_tlm_mutex)
        xSemaphoreTake(g_tlm_mutex, portMAX_DELAY);
}

static inline void tlm_unlock()
{
    if (g_tlm_mutex)
        xSemaphoreGive(g_tlm_mutex);
}
#else
static inline void tlm_lock() {}
static inline void tlm_unlock() {}
#endif

// tlm_printf("mestre", "hop", "ch=%u;n=%lu", canal, hop_counter);
static inline void tlm_printf(const char *src, const char *event, const char *fmt, ...)
{
    tlm_lock();

    Serial.print('@');
    Serial.print(src);
    Serial.print(";t=");
    Serial.print(millis());
    Serial.print(";e=");
    Serial.print(event);

    if (fmt && fmt[0] != '\0')
    {
        char buf[160];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        Serial.print(';');
        Serial.print(buf);
    }

    Serial.println();

    tlm_unlock();
}

// Evento sem campos extras, ex.: tlm_event("jammer", "boot").
static inline void tlm_event(const char *src, const char *event)
{
    tlm_printf(src, event, "");
}

// two-board-topology: nas placas combinadas, duas tasks FreeRTOS de
// núcleos diferentes chamam Serial.print/println/printf diretamente para
// log humano (não passam por tlm_printf) -- sem sincronização,
// HardwareSerial não é reentrante entre núcleos, e uma corrida pode
// travar uma das tasks indefinidamente (observado em bancada: o
// analisador imprime uma linha CSV de 126 números a cada ciclo, e uma
// única chamada de Serial.print do mestre nesse meio tempo já basta para
// travar). guarded_print/println/printf tomam o MESMO lock de
// tlm_printf() -- todo Serial.*() dos node.h deve passar por aqui, nunca
// chamar Serial diretamente fora de telemetry.h.
template <typename T>
static inline void guarded_print(T val)
{
    tlm_lock();
    Serial.print(val);
    tlm_unlock();
}

template <typename T>
static inline void guarded_println(T val)
{
    tlm_lock();
    Serial.println(val);
    tlm_unlock();
}

static inline void guarded_println()
{
    tlm_lock();
    Serial.println();
    tlm_unlock();
}

template <typename... Args>
static inline void guarded_printf(const char *fmt, Args... args)
{
    tlm_lock();
    Serial.printf(fmt, args...);
    tlm_unlock();
}

#endif // ARDUINO

#endif // TELEMETRY_H
