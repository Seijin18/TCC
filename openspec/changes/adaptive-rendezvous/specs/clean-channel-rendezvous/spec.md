## ADDED Requirements

### Requirement: Pareamento em canal medido como livre
O mestre DEVE (MUST) medir a atividade de cada candidato de pareamento antes
de acampar nele para sondar o escravo, preferindo o candidato com menor
atividade medida.

#### Scenario: Candidato ocupado é evitado
- **WHEN** um dos candidatos de pareamento tem atividade de RF medida acima
  dos demais
- **THEN** o mestre acampa em outro candidato com atividade menor antes de
  enviar `MSG_LINK_PROBE`

### Requirement: Candidato de pareamento não confiável é temporariamente evitado
O mestre DEVE (MUST) manter, por candidato de pareamento, um histórico de
falhas em completar o handshake real (link estabelecido mas troca de seed
nunca confirmada, ou janela de acampamento inteira sem nenhum link
estabelecido) — não apenas a medição instantânea de atividade (RPD) —, e
excluir temporariamente da seleção um candidato cujo número de falhas
recentes ultrapasse um limiar, reincluindo-o após um período de
recuperação, sem nunca excluir todos os candidatos simultaneamente.

#### Scenario: Candidato "quieto" mas que falha repetidamente é evitado
- **WHEN** um candidato de pareamento mede baixa atividade (RPD) mas falha
  em completar o handshake real por `PAIR_FAIL_THRESHOLD` tentativas
  consecutivas
- **THEN** o mestre para de medir/selecionar esse candidato até o período
  de recuperação (`PAIR_RECOVERY_ROUNDS`) vencer

#### Scenario: Candidato evitado volta a ser considerado após recuperação
- **WHEN** um candidato está temporariamente excluído e o número de
  "rounds" de seleção desde a exclusão atinge `PAIR_RECOVERY_ROUNDS`
- **THEN** o candidato volta a ser medido/considerado na próxima seleção,
  com seu histórico de falhas zerado

#### Scenario: Nunca esvaziar todos os candidatos
- **WHEN** todos os candidatos de pareamento atingiriam o limiar de falhas
  simultaneamente
- **THEN** a exclusão não é aplicada e todos os candidatos permanecem
  disponíveis para seleção

### Requirement: Escravo varre candidatos em vez de escutar um canal fixo
O escravo DEVE (MUST) alternar entre todos os candidatos de pareamento,
escutando cada um por um período curto, até estabelecer o link com o
mestre.

#### Scenario: Encontro com o mestre fora do primeiro candidato
- **WHEN** o mestre está acampado em um candidato diferente do primeiro da
  lista
- **THEN** o escravo, ao varrer os candidatos, eventualmente escuta nesse
  candidato e completa o handshake

### Requirement: Recuperação automática após perda de sincronismo
Mestre e escravo DEVEM (MUST) retornar ao estado de pareamento (acampar /
varrer, respectivamente) sem intervenção manual quando o enlace já
estabelecido deixar de trocar tráfego válido por um número definido de
hops consecutivos.

#### Scenario: Recuperação após uma placa reiniciar
- **WHEN** uma das placas perde e recupera energia enquanto a outra
  permanece ligada
- **THEN** a placa que permaneceu ligada detecta a ausência de tráfego
  válido, retorna ao estado de pareamento, e o enlace é reestabelecido
  quando a placa reiniciada voltar a operar

### Requirement: Contador de hop compartilhado
O escravo DEVE (MUST) adotar o contador de hop recebido em cada pacote de
dado válido do mestre, em vez de manter uma contagem própria independente.

#### Scenario: Convergência após inícios assimétricos
- **WHEN** mestre e escravo iniciam seus contadores de hop em momentos
  diferentes (ex. um reiniciou depois do outro)
- **THEN**, após o próximo pacote de dado válido recebido, o contador de
  hop do escravo é igual ao do mestre
