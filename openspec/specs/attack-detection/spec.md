# attack-detection Specification

## Purpose
TBD - created by archiving change spectrum-attack-classification. Update Purpose after archive.
## Requirements
### Requirement: Classificação por ciclo de varredura
A cada ciclo de `scanSpectrum()`, o analisador DEVE (MUST) calcular a atividade
média nos canais de `HOPPING_CHANNELS` (`alvo_avg`) e nos demais canais
varridos (`fora_avg`), e marcar o ciclo como "candidato a ataque" quando
`alvo_avg` exceder um limiar mínimo E a diferença `alvo_avg - fora_avg`
exceder um limiar de concentração.

#### Scenario: Ruído disperso em todo o espectro
- **WHEN** um ciclo de varredura mostra atividade elevada de forma
  uniforme em praticamente todos os canais (não concentrada nos 16 canais
  da tabela)
- **THEN** o ciclo NÃO é marcado como candidato a ataque (diferença
  `alvo_avg - fora_avg` abaixo do limiar de concentração)

#### Scenario: Energia concentrada nos canais da tabela
- **WHEN** um ciclo de varredura mostra atividade alta especificamente nos
  16 canais de `HOPPING_CHANNELS` e atividade baixa no restante do
  espectro
- **THEN** o ciclo é marcado como candidato a ataque

### Requirement: Persistência antes de declarar ataque
O analisador DEVE (MUST) exigir N ciclos candidatos consecutivos (configurável,
default 3) antes de declarar estado de "ataque ativo", e resetar a
contagem de persistência assim que um ciclo não-candidato ocorrer.

#### Scenario: Pico isolado de um único ciclo
- **WHEN** apenas 1 ciclo é candidato a ataque, seguido por um ciclo
  não-candidato
- **THEN** o estado de "ataque ativo" nunca é declarado

#### Scenario: Ataque sustentado
- **WHEN** N ciclos consecutivos são candidatos a ataque
- **THEN** o estado interno transiciona para "ataque ativo" a partir do
  N-ésimo ciclo

### Requirement: Sinalização por GPIO ao mestre
Enquanto o estado interno for "ataque ativo", o analisador DEVE (MUST) manter um
pino GPIO de saída em nível lógico HIGH; assim que um ciclo deixar de ser
candidato (streak de persistência resetada), o analisador DEVE (MUST) trazer o
pino de volta a LOW no mesmo ciclo.

#### Scenario: Início do alerta
- **WHEN** o estado interno transiciona para "ataque ativo"
- **THEN** o pino de alerta é levado a HIGH no mesmo ciclo dessa transição

#### Scenario: Fim do alerta
- **WHEN** o estado interno estava "ataque ativo" e o ciclo atual não é
  mais candidato a ataque
- **THEN** o pino de alerta é levado a LOW no mesmo ciclo, e a contagem de
  persistência é resetada

### Requirement: Recepção do alerta pelo mestre
O mestre DEVE (MUST) fazer polling do pino de alerta no seu loop principal e
registrar via log serial toda transição de estado (início e fim de
alerta), sem alterar seed, canal ou blacklist em resposta a isso (fora de
escopo deste change).

#### Scenario: Mestre detecta início de alerta
- **WHEN** o pino de alerta lido pelo mestre transiciona de LOW para HIGH
- **THEN** o mestre imprime uma linha de log indicando início de ataque
  detectado, sem alterar seu comportamento de hopping

#### Scenario: Mestre detecta fim de alerta
- **WHEN** o pino de alerta lido pelo mestre transiciona de HIGH para LOW
- **THEN** o mestre imprime uma linha de log indicando fim do alerta

