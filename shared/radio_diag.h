#ifndef RADIO_DIAG_H
#define RADIO_DIAG_H

// pairing-handshake-hardening (instrumentacao, tarefa 1.1): dump dos
// registradores do NRF24 relevantes as hipoteses H1 (FIFO de RX antiga
// atravessando troca de canal/estado) e H4 (radio do escravo preso em
// estado que nao recebe) de design.md -- ver "Evidencias"/"Hipoteses de
// causa" la. Sem mudanca de comportamento nenhuma: so leitura.
//
// Problema: RF24::read_register() (RF24 1.6.2, .pio/libdeps/*/RF24/RF24.h)
// e protected, e nenhum getter publico da lib cobre os registradores que
// interessam aqui (CONFIG, EN_AA, EN_RXADDR, FIFO_STATUS, FEATURE, DYNPD,
// RX_ADDR_P1) -- os publicos existentes (getChannel(), getPALevel(),
// getARC(), getStatusFlags()) so decodificam um subconjunto bem menor.
//
// Alternativas descartadas:
//   - reinterpret_cast/ponteiro de membro sobre o RF24 ja existente: alem
//     de exigir contornar a checagem de acesso do compilador, ficaria
//     dependendo do layout interno da classe (ordem/tamanho dos campos
//     privados) -- quebraria silenciosamente numa atualizacao da lib.
//   - reescrever a leitura de registrador aqui via SPI direto: duplica o
//     framing de protocolo (toggle de CSN, beginTransaction/endTransaction)
//     que a lib ja implementa, arriscando divergir dele com o tempo.
// Escolhida: subclasse fina (RF24Diag) que so acrescenta metodos publicos
// que chamam a implementacao PROTEGIDA ja existente na classe-mae -- nao
// reimplementa nada, nao guarda estado proprio, e tem exatamente o mesmo
// comportamento de RF24 em tudo o mais (nenhum metodo virtual e
// sobrescrito). Por isso e seguro trocar o tipo declarado de
// mestre_radio/escravo_radio de RF24 para RF24Diag: mudanca de tipo, nao
// de comportamento.

#ifdef ARDUINO
#include <Arduino.h>
#include <RF24.h>
#include <nRF24L01.h>
#include "telemetry.h"

class RF24Diag : public RF24
{
public:
    RF24Diag(rf24_gpio_pin_t _cepin, rf24_gpio_pin_t _cspin) : RF24(_cepin, _cspin) {}

    // Expoe o read_register() protegido da classe-mae -- unica razao de
    // existir esta subclasse.
    uint8_t diag_read_register(uint8_t reg) { return read_register(reg); }
    void diag_read_register(uint8_t reg, uint8_t *buf, uint8_t len) { read_register(reg, buf, len); }
};

// tlm_radio_regs: emite "@<src>;t=..;e=regs;why=<reason>;config=..;
// en_aa=..;en_rxaddr=..;rf_ch=..;rf_setup=..;status=..;fifo=..;
// feature=..;dynpd=..;rx_p1=.." (valores em hex) via tlm_printf() -- nao
// escreve na Serial diretamente (ver shared/telemetry.h: tlm_printf ja
// serializa a linha inteira sob o mesmo mutex usado pelos outros eventos,
// necessario nas placas combinadas onde duas tasks de nucleos diferentes
// escrevem na mesma UART).
//
// `reason` documenta o motivo da leitura (boot | loss | pair_timeout |
// seed_timeout | cmd -- ver chamadores em mestre/node.h e escravo/node.h)
// para comparar registradores do boot (que sempre pareia rapido) contra os
// de uma entrada em pareamento por perda (onde H4 preveria diferenca).
static inline void tlm_radio_regs(const char *src, RF24Diag &radio, const char *reason)
{
    uint8_t config = radio.diag_read_register(nRF24L01::CONFIG);
    uint8_t en_aa = radio.diag_read_register(nRF24L01::EN_AA);
    uint8_t en_rxaddr = radio.diag_read_register(nRF24L01::EN_RXADDR);
    uint8_t rf_ch = radio.diag_read_register(nRF24L01::RF_CH);
    uint8_t rf_setup = radio.diag_read_register(nRF24L01::RF_SETUP);
    uint8_t status = radio.diag_read_register(nRF24L01::STATUS);
    uint8_t fifo = radio.diag_read_register(nRF24L01::FIFO_STATUS);
    uint8_t feature = radio.diag_read_register(nRF24L01::FEATURE);
    uint8_t dynpd = radio.diag_read_register(nRF24L01::DYNPD);

    // RX_ADDR_P1 tem 5 bytes (endereco de pipe, nao um unico registrador
    // escalar) -- concatenado em hex sem separador (ex. "5356303031" para
    // o endereco "SLV01").
    uint8_t rx_p1[5] = {0};
    radio.diag_read_register(nRF24L01::RX_ADDR_P1, rx_p1, sizeof(rx_p1));

    tlm_printf(src, "regs",
               "why=%s;config=%02X;en_aa=%02X;en_rxaddr=%02X;rf_ch=%02X;rf_setup=%02X;"
               "status=%02X;fifo=%02X;feature=%02X;dynpd=%02X;rx_p1=%02X%02X%02X%02X%02X",
               reason, config, en_aa, en_rxaddr, rf_ch, rf_setup, status, fifo, feature, dynpd,
               rx_p1[0], rx_p1[1], rx_p1[2], rx_p1[3], rx_p1[4]);
}

#endif // ARDUINO

#endif // RADIO_DIAG_H
