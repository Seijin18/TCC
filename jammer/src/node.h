#ifndef JAMMER_NODE_H
#define JAMMER_NODE_H

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <cstring>
#include "../../shared/hopping.h"
#include "../../shared/telemetry.h"

#define PIN_CE_VSPI 22
#define PIN_CSN_VSPI 21
#define PIN_SCK_VSPI 18
#define PIN_MISO_VSPI 19
#define PIN_MOSI_VSPI 23

#define PIN_CE_HSPI 16
#define PIN_CSN_HSPI 15
#define PIN_SCK_HSPI 4
#define PIN_MISO_HSPI 2
#define PIN_MOSI_HSPI 17

// 1 = dwell lento (2 ms/canal) para visualizar no heatmap do analisador
// 0 = modo agressivo para teste com fone BT
#define JAMMER_SPECTRUM_TEST 0

// Banda Bluetooth Classic + BLE: 2402–2480 MHz (F0 = 2400 + RF_CH)
#define BT_CH_MIN 2
#define BT_CH_MAX 80
#define BT_CH_MID 41

#if JAMMER_SPECTRUM_TEST
#define PLL_DWELL_US 2000
#else
#define PLL_DWELL_US 130
#endif

// Achado de bancada (2026-09-18, validacao end-to-end): em modo targeted,
// com o dwell padrao (~1ms/canal, dominado pelo vTaskDelay(1) da task),
// o jammer troca de canal rapido demais em relacao ao scanSpectrum() do
// analisador (~860ms pra varrer os 126 canais) -- um canal-alvo so era
// pego pelo analisador raramente e por acaso, levando dezenas de segundos
// entre hits no MESMO canal. Aumentar o hold especificamente em targeted
// (nao mexe no sweep, que deve continuar rapido/amplo) da mais chance de
// QUALQUER scanner (nao so este analisador) perceber a presenca -- ainda
// nao elimina o problema de fundo (um radio so pode estar em 1 canal por
// vez), mas reduz o tempo medio ate uma primeira deteccao.
static const uint32_t JAM_TARGETED_HOLD_MS = 60;

static SPIClass vspi(VSPI);
static SPIClass hspi(HSPI);

static RF24 jammer_radio(PIN_CE_VSPI, PIN_CSN_VSPI);
static RF24 jammer_radio2(PIN_CE_HSPI, PIN_CSN_HSPI);

static bool has_radio2 = false;

enum JamMode
{
    JAM_MODE_SWEEP,
    JAM_MODE_TARGETED
};

static JamMode g_mode = JAM_MODE_SWEEP;

// Fonte opcional de "handshake concluído" — quando registrada (placa_b,
// via jammer_set_ready_source()), o jammer usa **só** o rádio HSPI (o
// barramento VSPI da placa pertence ao escravo -- two-board-topology
// seção 7) e só inicia a portadora depois que a fonte retornar true. Sem
// fonte registrada (nós standalone, cenário de 4 placas), preserva o
// comportamento original: dois rádios (VSPI+HSPI), emite desde o boot.
static bool (*g_jammer_ready_source)() = nullptr;
static bool g_combined_mode = false;
static bool g_carrier_started = false;

// jam on|off — na placa combinada, default é OFF (não emitir até um
// comando explícito), diferente do padrão standalone (emite assim que o
// handshake completa). "power min|low|high|max" ajusta RF24_PA_* em
// runtime nos dois testes graduais (T4/T5 do plano de bancada).
static bool g_jam_enabled = true;
static rf24_pa_dbm_e g_jam_power = RF24_PA_MAX;

static void jammer_set_ready_source(bool (*source)())
{
    g_jammer_ready_source = source;
}

// two-board-topology / scenario-automation: na placa combinada, jammer e
// escravo compartilham a MESMA UART física, mas só um dos dois pode ler
// Serial.available()/read() sem correr (os bytes de uma linha se
// espalhariam entre os dois leitores concorrentes, corrompendo os dois
// comandos). jammer continua sendo o único a ler fisicamente (não mudou);
// comandos que ele não reconhece (ex. "mode fixed"/"mode hopping", do
// modo de teste de scenario-automation) são repassados a este sink,
// registrado por placa_b/src/main.cpp -> handle_escravo_command(). Sem
// sink registrado (jammer standalone/4 placas), comando desconhecido só
// imprime o erro, como antes.
static bool (*g_jammer_unknown_cmd_sink)(const char *) = nullptr;

static void jammer_set_unknown_cmd_sink(bool (*sink)(const char *))
{
    g_jammer_unknown_cmd_sink = sink;
}

#define CMD_BUF_SIZE 32
static char cmdBuf[CMD_BUF_SIZE];
static uint8_t cmdLen = 0;

static void applyPowerLevel()
{
    jammer_radio.setPALevel(g_jam_power);
    if (has_radio2)
        jammer_radio2.setPALevel(g_jam_power);
}

static void stopCarrier()
{
    if (!g_carrier_started)
        return;
    jammer_radio.stopConstCarrier();
    if (has_radio2)
        jammer_radio2.stopConstCarrier();
    g_carrier_started = false;
    tlm_event("jammer", "jam_state_off");
    guarded_println("[JAMMER] Emissao interrompida (jam off).");
}

static void configureJammerRadio(RF24 &r)
{
    r.setAutoAck(false);
    r.setRetries(0, 0);
    r.disableCRC();
    r.setDataRate(RF24_2MBPS);
    r.setPALevel(g_jam_power);
    r.stopListening();
}

static void startCarrierOn(RF24 &r, uint8_t channel)
{
    r.startConstCarrier(g_jam_power, channel);
}

static void startCarrier()
{
    startCarrierOn(jammer_radio, BT_CH_MIN);
    if (has_radio2)
        startCarrierOn(jammer_radio2, BT_CH_MAX);
    g_carrier_started = true;
    tlm_event("jammer", "jam_state_on");
    guarded_println("[JAMMER] Emissao CW ativa. '.' = heartbeat a cada 1 s\n");
}

static void hopCarrier(RF24 &r, uint8_t channel)
{
    r.setChannel(channel);
    delayMicroseconds(PLL_DWELL_US);
}

static bool cmdMatches(const char *buf, const char *cmd)
{
    return strcasecmp(buf, cmd) == 0;
}

static void handleCommand(char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;

    for (char *p = line; *p; p++)
    {
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p - 'A' + 'a');
    }

    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t'))
        line[--len] = '\0';

    if (len == 0)
        return;

    if (cmdMatches(line, "mode sweep"))
    {
        g_mode = JAM_MODE_SWEEP;
        tlm_event("jammer", "jam_mode_sweep");
        guarded_println("[MODE] sweep");
    }
    else if (cmdMatches(line, "mode targeted"))
    {
        g_mode = JAM_MODE_TARGETED;
        tlm_event("jammer", "jam_mode_targeted");
        guarded_println("[MODE] targeted");
    }
    else if (cmdMatches(line, "jam off"))
    {
        g_jam_enabled = false;
        stopCarrier();
    }
    else if (cmdMatches(line, "jam on"))
    {
        g_jam_enabled = true;
        guarded_println("[JAMMER] Habilitado (jam on) -- emissao retoma no proximo ciclo.");
    }
    else if (cmdMatches(line, "power min"))
    {
        g_jam_power = RF24_PA_MIN;
        applyPowerLevel();
        guarded_println("[JAMMER] Potencia: MIN");
    }
    else if (cmdMatches(line, "power low"))
    {
        g_jam_power = RF24_PA_LOW;
        applyPowerLevel();
        guarded_println("[JAMMER] Potencia: LOW");
    }
    else if (cmdMatches(line, "power high"))
    {
        g_jam_power = RF24_PA_HIGH;
        applyPowerLevel();
        guarded_println("[JAMMER] Potencia: HIGH");
    }
    else if (cmdMatches(line, "power max"))
    {
        g_jam_power = RF24_PA_MAX;
        applyPowerLevel();
        guarded_println("[JAMMER] Potencia: MAX");
    }
    else if (!g_jammer_unknown_cmd_sink || !g_jammer_unknown_cmd_sink(line))
    {
        guarded_printf("[ERRO] comando desconhecido: %s\n", line);
    }
}

static void processSerial()
{
    while (Serial.available())
    {
        char c = (char)Serial.read();

        if (c == '\n' || c == '\r')
        {
            if (cmdLen > 0)
            {
                cmdBuf[cmdLen] = '\0';
                handleCommand(cmdBuf);
                cmdLen = 0;
            }
            continue;
        }

        if (cmdLen < CMD_BUF_SIZE - 1)
            cmdBuf[cmdLen++] = c;
        else
            cmdLen = 0;
    }
}

static void jammer_setup()
{
    Serial.begin(115200);
    delay(1000);

    g_combined_mode = (g_jammer_ready_source != nullptr);
    // Placa combinada: nao emitir por padrao ate um "jam on"/"mode ..."
    // explicito, e em potencia baixa -- PA_MAX a centimetros do escravo
    // (docs/pinout.md) satura o receptor na banda inteira, destruindo a
    // premissa de "escapar para um canal livre".
    if (g_combined_mode)
    {
        g_jam_enabled = false;
        g_jam_power = RF24_PA_LOW;
    }

    guarded_println("\n\n=== JAMMER BLUETOOTH (CW) — TESTE CELULAR + FONE ===");
    guarded_println("Faixa: 2402–2480 MHz | NRF24 ch 2–80");
#if JAMMER_SPECTRUM_TEST
    guarded_printf("Modo: SPECTRUM_TEST | Dwell: %u us/canal (heatmap)\n", PLL_DWELL_US);
#else
    guarded_printf("Modo: NORMAL | Dwell: %u us/canal\n", PLL_DWELL_US);
#endif

    hspi.begin(PIN_SCK_HSPI, PIN_MISO_HSPI, PIN_MOSI_HSPI, PIN_CSN_HSPI);

    if (g_combined_mode)
    {
        // Um único rádio, no HSPI -- o barramento VSPI desta placa
        // pertence ao escravo (two-board-topology seção 7).
        jammer_radio.begin(&hspi, PIN_CE_HSPI, PIN_CSN_HSPI);
        has_radio2 = false;

        if (!jammer_radio.isChipConnected())
            guarded_println("[ERRO] Radio (HSPI) nao detectado. Verifique fiação e 3.3V.");

        configureJammerRadio(jammer_radio);
        guarded_println("[OK] 1 radio (HSPI, placa combinada): varredura completa 2–80");
    }
    else
    {
        vspi.begin(PIN_SCK_VSPI, PIN_MISO_VSPI, PIN_MOSI_VSPI, PIN_CSN_VSPI);

        jammer_radio.begin(&vspi, PIN_CE_VSPI, PIN_CSN_VSPI);
        jammer_radio2.begin(&hspi, PIN_CE_HSPI, PIN_CSN_HSPI);
        has_radio2 = jammer_radio2.isChipConnected();

        if (!jammer_radio.isChipConnected())
            guarded_println("[ERRO] Radio 1 (VSPI) nao detectado. Verifique fiação e 3.3V.");

        configureJammerRadio(jammer_radio);
        if (has_radio2)
        {
            configureJammerRadio(jammer_radio2);
            guarded_println("[OK] 2 radios: faixa dividida (2–41 e 41–80 MHz)");
        }
        else
        {
            guarded_println("[OK] 1 radio: varredura completa 2–80");
        }
    }

    tlm_event("jammer", "boot");

    if (!g_jam_enabled)
    {
        guarded_println("[JAMMER] Desabilitado no boot (placa combinada). Envie 'jam on' para habilitar.");
    }
    else if (g_jammer_ready_source)
    {
        guarded_println("[JAMMER] Aguardando handshake mestre/escravo antes de emitir...");
    }
    else
    {
        startCarrier();
    }
}

static void loopSweep()
{
    static uint8_t ch_fwd = BT_CH_MIN;
    static uint8_t ch_rev = BT_CH_MAX;

    if (has_radio2)
    {
        hopCarrier(jammer_radio, ch_fwd);
        hopCarrier(jammer_radio2, ch_rev);

        ch_fwd++;
        if (ch_fwd > BT_CH_MID)
            ch_fwd = BT_CH_MIN;

        if (ch_rev <= BT_CH_MID)
            ch_rev = BT_CH_MAX;
        else
            ch_rev--;
    }
    else
    {
        hopCarrier(jammer_radio, ch_fwd);

        ch_fwd++;
        if (ch_fwd > BT_CH_MAX)
            ch_fwd = BT_CH_MIN;
    }
}

static void loopTargeted()
{
    static uint8_t idx_fwd = 0;
    static uint8_t idx_rev = TABLE_SIZE - 1;

    const uint8_t fwd_limit = has_radio2 ? (TABLE_SIZE / 2) : TABLE_SIZE;

    hopCarrier(jammer_radio, HOPPING_CHANNELS[idx_fwd]);
    idx_fwd++;
    if (idx_fwd >= fwd_limit)
        idx_fwd = 0;

    if (has_radio2)
    {
        hopCarrier(jammer_radio2, HOPPING_CHANNELS[idx_rev]);
        if (idx_rev <= fwd_limit)
            idx_rev = TABLE_SIZE - 1;
        else
            idx_rev--;
    }

    delay(JAM_TARGETED_HOLD_MS);
}

static void jammer_loop()
{
    static unsigned long last_print = 0;

    processSerial(); // mantém comandos (mode/jam/power) sendo aceitos mesmo em espera

    if (!g_jam_enabled)
    {
        if (g_carrier_started)
            stopCarrier();
        return;
    }

    if (!g_carrier_started)
    {
        if (g_jammer_ready_source && !g_jammer_ready_source())
            return; // ainda esperando o handshake mestre/escravo

        startCarrier();
    }

    if (g_mode == JAM_MODE_TARGETED)
        loopTargeted();
    else
        loopSweep();

    if (millis() - last_print >= 1000)
    {
        last_print = millis();
        guarded_print('.');
    }
}

#endif // JAMMER_NODE_H
