# 🛡️ TCC: Sistema de Comunicação Anti-Jamming (NRF24L01 + ESP32)

Este repositório contém o código-fonte e a documentação do Trabalho de Conclusão de Curso focado no desenvolvimento de um sistema de comunicação em radiofrequência (2.4 GHz) resiliente a ataques de interferência intencional (Jamming). 

O sistema utiliza módulos **NRF24L01+** e microcontroladores **ESP32** para implementar uma técnica adaptativa de *Frequency-Hopping Spread Spectrum* (FHSS), capaz de detectar ataques e desviar ativamente das frequências comprometidas (Troca Dinâmica de Seed e Blacklisting).

---

## 📋 Arquitetura do Projeto

O projeto é fragmentado em 4 nós independentes (atores do ecossistema) e módulos compartilhados:

- 📡 **mestre/**: O transmissor confiável. Envia beacons de sincronismo e os dados úteis. Responsável por comandar as mudanças táticas de hopping sob ataque.
- 🎧 **escravo/**: O receptor confiável. Acompanha os saltos do mestre, contabiliza a métrica de qualidade (QoS / Packet Loss Rate) e valida a comunicação.
- ⚡ **jammer/**: O sistema atacante (vilão). Pode ser configurado como *Sweep Jammer* (varredura rápida por ruído) ou *Targeted Jammer* (atacando alvos fixos da nossa lista).
- 🧠 **analisador/**: O observador inteligente. Analisa o piso de ruído (noise floor) do espectro de 2.4GHz para detectar identificar padrões de ataque ativo.
- 🔗 **shared/**: Bibliotecas e estruturas de dados compartilhadas entre os nós, como o algoritmo inicial de cálculo de canais (Hopping).

---

## 🛠️ Requisitos e Dependências

### 🔧 Requisitos de Hardware
Para montar o cenário completo do projeto, você necessitará de:

- **4x** Placas de Desenvolvimento **ESP32** (ou compatíveis como ESP8266/Arduino, embora o ESP32 forneça mais poder de processamento para o Analisador).
- **4x** Módulos Transceptores **NRF24L01+** (Recomenda-se a versão com Antena **PA+LNA** para testes mais fidedignos de alcance e interferência).
- **4x** Módulos Adaptadores de Tensão para NRF24L01 (O chip NRF24 é muito sensível a ruído na linha de 3.3V. Os adaptadores resolvem os problemas de instabilidade).
- Protoboards, Jumpers e cabos USB de dados.

### 💻 Requisitos de Software
- **Ambiente de Desenvolvimento:** [VS Code](https://code.visualstudio.com/) com a extensão [PlatformIO](https://platformio.org/) (Recomendado) ou a clássica Arduino IDE.
- **Linguagem:** C/C++ padrão Arduino.
- **Bibliotecas:** 
  - `RF24` por TMRh20 (Gerencia a comunicação do rádio).
  - `SPI` (Nativa do core do Arduino/ESP32).

---

## 🗺️ Plano de Desenvolvimento e Execução (As 5 Fases)

O projeto foi organizado em 5 fases evolutivas para garantir marcos de entrega bem definidos:

### Fase 1: Fundação do Protocolo de Comunicação (Camada de Enlace)
- Estabelecimento do "Handshake" entre Mestre e Escravo.
- Transmissão de pacotes Beacon (sincronismo).
- Implementação da base do FHSS com tabela fixa (16 canais) e algoritmo gerador com Seed constante.
- Implementação de QoS via controle de `Packet Loss Rate`.

### Fase 2: O Sistema de Ataque (O Jammer)
- Criação das ferramentas de teste e negação de serviço.
- Modos de ataque previstos: *Varrer todo o espectro (Sweep)* ou *Focar nos canais do algoritmo de salto*.

### Fase 3: Detecção e Inteligência (O Analisador)
- Rastreamento contínuo dos 125 canais de RF pelo ESP32.
- Lógica de distinção entre Interferência Aleatória (ex: tráfego natural de roteadores Wi-Fi) e Ataques Intencionais Direcionados.
- Geração de Alertas que interligarão o Analisador com o Mestre.

### Fase 4: Atuação e Mitigação (A "Esquiva")
- **Troca Dinâmica de Seed:** Reestabelecer o padrão criptográfico do hopping no meio de um ataque.
- **Blacklisting de Canais:** Retirar canais mortos de circulação e distribuir a rede para canais silenciosos.
- Comprovação de "Self-Healing" (recuperação) do link em tempo real.

### Fase 5: Testes Finais e Validação de Dados
- Construção dos cenários comparativos: Sem hopping VS Jammer; Hopping Fixo VS Jammer; Hopping Adaptativo dinâmico VS Jammer.
- Levantamento de relatórios: Gráficos de Latência e Throughput documentados para a banca de avaliação da monografia.

---
> *Consulte o diretório atual e o `Guia_Desenvolvimento_TCC_AntiJamming.md` para visualizar as tarefas ("To-Do") técnicas e check-lists diários.*
