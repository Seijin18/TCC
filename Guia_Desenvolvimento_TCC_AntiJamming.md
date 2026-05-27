# Guia de Desenvolvimento - TCC: Sistema de Comunicação Anti-Jamming (NRF24L01 + ESP32)

Este documento atua como o roadmap definitivo para a implementação, testes e validação do Trabalho de Conclusão de Curso (TCC). O projeto está dividido em cinco fases progressivas, indo desde a criação do link de comunicação básico até a mitigação adaptativa de ataques cibernéticos em radiofrequência.

---

## 🏁 Fase 1: Fundação do Protocolo de Comunicação (Camada de Enlace)
**Objetivo:** Estabelecer um "aperto de mão" (handshake) robusto e criar a base do FHSS (Frequency-Hopping Spread Spectrum).

- [ ] **Sincronização de Tempo (Beacon):**
  - Implementar um Beacon (sinal de sincronismo) enviado pelo "Mestre".
  - Garantir que o "Escravo" utilize esse beacon para saber o momento exato de trocar de canal.
- [ ] **Algoritmo de Hopping Inicial:**
  - Criar uma tabela fixa de canais (ex: 16 canais selecionados dentro dos 125 disponíveis no módulo NRF24L01).
- [ ] **Mecanismo de Seed (Semente):**
  - Desenvolver uma função matemática para seleção de canais. Exemplo: `Canal_Atual = Tabela[f(Seed, Contador)]`.
- [ ] **Métrica de Qualidade (QoS):**
  - Implementar um contador de pacotes perdidos (*Packet Loss Rate* - PLR) diretamente no código do receptor (Escravo) para medir a estabilidade do link.

---

## ⚔️ Fase 2: O Sistema de Ataque (O Jammer)
**Objetivo:** Construir a ferramenta de teste ("vilão") necessária para estressar e homologar a resiliência do sistema defensivo.

- [ ] **Jammer de Varredura (Sweep):**
  - Programar um conjunto (ESP32 + NRF24) para disparar portadoras de ruído e inundar os canais em uma sequência de varredura rápida.
- [ ] **Jammer Alvo (Targeted):**
  - Programar o Jammer para atacar especificamente os canais da sua "Tabela de 16".
  - Simular o comportamento de um atacante que já interceptou e descobriu parte do padrão de saltos do seu sistema.

---

## 🧠 Fase 3: Detecção e Inteligência (O Analisador)
**Objetivo:** Elevar o projeto ao nível do TCC incorporando inteligência para percepção de anomalias espectrais.

- [ ] **Mapeamento de Espectro:**
  - Configurar o analisador para escanear os canais em background e identificar elevações drásticas e constantes no "piso de ruído" (*noise floor*).
- [ ] **Identificação de Padrão (Classificação):**
  - Criar uma lógica (heurística ou baseada em ML local) para diferenciar:
    - *Interferência Aleatória:* Ruído natural ou esparso (Wi-Fi, Bluetooth).
    - *Ataque Direcionado:* Ruído focado estritamente nos canais utilizados pelo algoritmo de hopping.
- [ ] **Geração de Alerta:**
  - O analisador deve notificar o "Cérebro" do sistema de comunicação primária sobre o ataque iminente (via pino de interrupção de hardware ou barramento serial).

---

## 🛡️ Fase 4: Atuação e Mitigação (A "Esquiva")
**Objetivo:** Demonstrar a proatividade do sistema; a parte de maior interesse analítico da banca avaliadora.

- [ ] **Troca Dinâmica de Seed (Chaveamento de Padrão):**
  - Ao receber o alerta da Fase 3, o nodo Mestre deve transmitir um pacote crítico de comando ("MUDAR_SEED") contendo o novo valor pseudoaleatório para as próximas sequências.
- [ ] **Blacklisting de Canais Dinâmico:**
  - O sistema deve isolar (remover da lista) os canais detectados sob ataque severo e redistribuir a carga de comunicação entre os canais "limpos" remanescentes.
- [ ] **Validação e Recuperação em Tempo Real:**
  - Monitorar e comprovar via logs seriais que a taxa de perda de pacotes (PLR) se recupera e cai para níveis aceitáveis quase imediatamente após a troca de Seed/Blacklist, mesmo com o Jammer ainda ativo no cenário anterior.

---

## 📊 Fase 5: Testes, Coleta de Dados e Escrita da Monografia
**Objetivo:** Tabular métricas comparativas rigorosas para gerar o material técnico exigido na monografia.

- [ ] **Bateria de Testes (Tabelas de Comparação):**
  - **Cenário A:** Comunicação sem Hopping (Canal Fixo) vs Jammer (Resultado esperado: Taxa de erro ~100%, falha catastrófica).
  - **Cenário B:** Comunicação com Hopping Fixo (Fase 1) vs Jammer Target (Resultado esperado: Taxa de erro média/alta).
  - **Cenário C:** *O Seu Sistema Completo* (Hopping Adaptativo + Troca de Seed + Blacklisting) vs Jammer (Resultado esperado: Taxa de erro baixa / recuperação rápida - "Self-healing").
- [ ] **Geração de Gráficos Analíticos:**
  - Exportar CSV via porta serial com dados temporais.
  - Plotar gráficos de *Throughput* (Vazão de dados) vs Tempo.
  - Plotar gráficos de *Latência* antes do ataque, durante o início do ataque e logo após a mitigação computada do sistema.
