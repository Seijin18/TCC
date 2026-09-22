## MODIFIED Requirements

### Requirement: Blacklisting de canal sob alerta ativo
O mestre DEVE (MUST) avaliar continuamente os contadores de falha por canal e, para qualquer canal habilitado cujo contador exceda um limiar configurável, adicionar esse canal à máscara de blacklist e enfileirar `BLACKLIST_UPDATE` com a máscara atualizada, com prioridade sobre fragmentos de dados pendentes (mas não sobre um `MUDAR_SEED` já enfileirado) — independentemente de o alerta de ataque estar ativo ou não.

#### Scenario: Canal com falha severa durante ataque
- **WHEN** o alerta de ataque está ativo e o contador de falha de um canal habilitado excede o limiar configurado
- **THEN** esse canal é marcado como blacklisted na máscara local do mestre e `BLACKLIST_UPDATE` é enfileirado

#### Scenario: Falha severa sem alerta ativo
- **WHEN** o contador de falha de um canal habilitado excede o limiar configurado e o alerta de ataque NÃO está ativo
- **THEN** esse canal também é marcado como blacklisted e `BLACKLIST_UPDATE` é enfileirado — a decisão depende só da evidência própria do mestre (contador de falha por canal), não do estado do alerta

## ADDED Requirements

### Requirement: Recuperação automática de canal blacklisted
Um canal atualmente excluído da máscara DEVE (MUST) voltar a ser incluído assim que tiver permanecido excluído por pelo menos um número configurável de hops (`RECOVERY_COOLDOWN_HOPS`), independentemente do estado do alerta de ataque e sem exigir nenhuma nova tentativa de transmissão nesse canal.

#### Scenario: Cooldown de recuperação atingido
- **WHEN** um canal está excluído da máscara há pelo menos `RECOVERY_COOLDOWN_HOPS` hops
- **THEN** esse canal é reincluído na máscara e `BLACKLIST_UPDATE` é enfileirado com a máscara atualizada

#### Scenario: Cooldown de recuperação ainda não atingido
- **WHEN** um canal está excluído da máscara há menos de `RECOVERY_COOLDOWN_HOPS` hops
- **THEN** esse canal permanece excluído e nenhuma atualização é enfileirada só por causa dele

#### Scenario: Reinclusão zera a evidência de falha anterior
- **WHEN** um canal é reincluído na máscara após cumprir o cooldown
- **THEN** o contador de falha desse canal é zerado, exigindo evidência de falha totalmente nova (não a que causou a exclusão original) para excluí-lo de novo

### Requirement: Estado de recuperação não sobrevive a um retorno ao pareamento
Ao voltar ao estado de pareamento (perda de sincronismo), o mestre DEVE (MUST) zerar tanto a máscara de blacklist quanto qualquer estado de cooldown de recuperação associado a cada canal, para que a contagem de hops de uma sessão anterior não seja reaproveitada incorretamente na sessão seguinte.

#### Scenario: Reset de sincronismo com canais em cooldown
- **WHEN** o mestre volta ao estado de pareamento enquanto um ou mais canais estavam excluídos aguardando cooldown de recuperação
- **THEN** a máscara volta a habilitar todos os 16 canais e o estado de cooldown de cada canal é zerado, para a próxima sessão de hopping recalcular blacklist/recuperação do zero
