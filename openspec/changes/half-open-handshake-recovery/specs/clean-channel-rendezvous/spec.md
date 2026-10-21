## ADDED Requirements

### Requirement: Escravo recupera sozinho de pareamento que nunca confirma
O escravo DEVE (MUST) se recuperar sozinho de um handshake meio-aberto — em
que ele entra em pareamento provisório repetidamente mas o enlace nunca
confirma dados — fazendo uma recuperação forçada (reinicialização completa do
rádio + volta à varredura com um ponto de partida aleatorizado), sem depender
de comando do operador.

Contexto (ver proposal.md "Verificação 2026-10-21"): o gatilho conhecido
(falha de rekey sob mitigação causada pela deriva de clock) foi eliminado
por `slot-phase-servo`, e o travamento não reproduz mais. Este requisito é
rede de segurança (defense-in-depth): o estado de máquina de travamento ainda
existe, e esta recuperação garante que, se ele ocorrer por qualquer caminho,
o enlace volta sozinho. A recuperação imita o que um `reset` manual do
escravo (único que recuperava o estado preso real) faz, incluindo quebrar
qualquer ressonância de fase com a rotação de acampamento do mestre via um
deslocamento aleatório do início da varredura.

#### Scenario: Várias sessões provisórias sem confirmar
- **WHEN** o escravo entra em pareamento provisório em
  `HALF_OPEN_SESSIONS` sessões seguidas (nonces distintos) e nenhuma delas
  chega a receber um `FragmentPacket` (confirmar o enlace)
- **THEN** o escravo emite `e=half_open`, reinicializa o rádio por completo e
  volta a varrer os candidatos a partir de um índice/fase aleatorizado

#### Scenario: Pareamento normal não dispara a recuperação
- **WHEN** o escravo pareia e confirma dados dentro de uma ou duas sessões
- **THEN** o contador de sessões sem confirmação zera e a recuperação forçada
  não é acionada

#### Scenario: Recuperação dentro do prazo
- **WHEN** um handshake meio-aberto se instala e o canal de pareamento está
  livre
- **THEN** o enlace volta a trocar fragmentos em no máximo 15 s, sem
  intervenção
