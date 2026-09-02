## Why

A Fase 4 é o núcleo do TCC ("parte de maior interesse analítico da banca"):
o sistema precisa reagir a um ataque detectado trocando a seed do FHSS e
retirando de circulação os canais comprovadamente comprometidos, depois
comprovar que o link se recupera ("self-healing"). Hoje nenhuma dessas três
coisas existe: a seed é fixada uma única vez antes do hopping começar
(`mestre/src/main.cpp:113-141`), `HOPPING_CHANNELS` é uma tabela `const`
imutável (`shared/hopping.h:12-16`), e não há nenhum mecanismo de
recuperação medido. Este change depende de `plr-metric` (métrica para
comprovar a recuperação) e `spectrum-attack-classification` (sinal de
alerta que dispara a mitigação).

## What Changes

- Dois novos tipos de pacote de controle — troca de seed (`MUDAR_SEED`) e
  atualização de blacklist (`BLACKLIST_UPDATE`) — enviados pelo mestre
  reaproveitando o mesmo mecanismo de envio-e-espera-de-ACK por hop que já
  existe para `FragmentPacket`/`FragmentAck`, em vez de um canal de
  rendezvous separado.
- Ao detectar transição de alerta (LOW→HIGH no pino de
  `spectrum-attack-classification`), o mestre gera uma nova seed aleatória
  e enfileira `MUDAR_SEED` com prioridade sobre fragmentos de dados
  pendentes.
- O mestre passa a contar falhas de ACK por canal (índice na tabela de 16)
  continuamente; enquanto o alerta estiver ativo, canais com falha severa e
  persistente são adicionados a uma máscara de blacklist, sincronizada ao
  escravo via `BLACKLIST_UPDATE`.
- `shared/hopping.h` ganha uma variante de `get_next_channel` que opera
  sobre o subconjunto de canais não-blacklisted, usada identicamente por
  mestre e escravo depois que ambos confirmam a mesma máscara.
- Escravo aplica seed/blacklist recebidos e confirma via ACK antes de
  mudar seu próprio cálculo de canal — evita dessincronia entre os dois
  nós.

## Capabilities

### New Capabilities
- `adaptive-mitigation`: troca dinâmica de seed e blacklisting de canais
  disparados por alerta de ataque, com sincronização confiável entre
  mestre e escravo via o mecanismo de retry por hop já existente.

### Modified Capabilities
- `packet-loss-metric` (de `plr-metric`): nenhuma mudança de requisito, mas
  o comportamento observável (PLR caindo após mitigação) passa a ser usado
  como critério de validação deste change — sem alterar a spec já
  aprovada.

## Impact

- `shared/hopping.h`: nova função de seleção de canal com máscara, novos
  tipos de pacote de controle.
- `mestre/src/main.cpp`: novo estado (contadores de falha por canal,
  máscara de blacklist, fila de prioridade de controle vs. dado), consumo
  do pino de alerta de `spectrum-attack-classification`.
- `escravo/src/main.cpp`: novo handler para os pacotes de controle,
  recomputação do canal ativo ao aplicar seed/blacklist novos.
- Depende de `plr-metric` (validação) e `spectrum-attack-classification`
  (gatilho); é pré-requisito de `scenario-automation`.
