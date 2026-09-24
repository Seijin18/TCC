## Context

O mestre já sabe, hop a hop, se um fragmento foi confirmado ou não
(`wait_fragment_ack`, `mestre/src/main.cpp:255-281`, com log explícito
"sera reenviada no proximo salto" em caso de timeout,
`mestre/src/main.cpp:442`). Esse retry-por-hop já é a forma como o
protocolo lida com perda pontual hoje. `get_next_channel(seed, counter)`
(`shared/hopping.h:47-52`) é puramente funcional (sem estado) e indexa
`HOPPING_CHANNELS` por `(seed + counter) % TABLE_SIZE` — os 16 canais são
sempre todos alcançáveis, não há como excluir um canal hoje. A seed só é
lida uma vez, via serial, antes do hopping começar (`wait_for_seed`,
`mestre/src/main.cpp:113-141`) e nunca mais muda depois disso. O alerta de
`spectrum-attack-classification` chega ao mestre como um nível de GPIO
(HIGH enquanto ataque ativo), lido por polling no loop principal.

## Goals / Non-Goals

**Goals:**
- Reagir ao alerta trocando a seed sem exigir handshake fora da rotação de
  hops normal (reaproveitar o mecanismo de retry já validado, não inventar
  um canal de rendezvous novo).
- Blacklist de canal só quando há evidência própria e persistente de falha
  naquele canal específico (não uma suposição baseada só no alerta
  binário) — evita blacklistar canais saudáveis por coincidência de
  timing do alerta.
- Escravo só passa a usar a nova seed/máscara depois de confirmar
  recepção — mestre e escravo nunca operam com visões divergentes do
  canal ativo por mais de um hop.
- Deixar o critério de "recuperação" mensurável via a métrica já existente
  (`plr_window` de `plr-metric`), sem inventar uma métrica nova só para
  este change.

**Non-Goals:**
- Não implementa nenhuma forma de autenticação/criptografia dos pacotes de
  controle — fora do escopo de RF do TCC (mitigar jamming, não spoofing).
- Não faz o mestre remover canais da blacklist automaticamente quando eles
  "melhoram" (des-blacklist) — o guia pede demonstrar recuperação via
  troca de seed/blacklist, não um ciclo completo de reavaliação contínua;
  pode virar um change futuro se a banca pedir.
- Não muda `PAIRING_CHANNEL`/o handshake inicial — a mudança de seed
  acontece só depois que a sessão de hopping já está ativa.

## Decisions

- **Pacotes de controle reaproveitam o retry por hop, não um canal de
  rendezvous separado.** Dois novos tipos, `MUDAR_SEED { new_seed }` e
  `BLACKLIST_UPDATE { mask }`, cada um com seu ACK dedicado
  (`ControlAck { type, echo }`), enviados pela mesma função de "enviar no
  hop atual e esperar ACK, senão reenviar no próximo hop" já usada para
  `FragmentPacket`. Motivo: esse mecanismo já é testado e funciona mesmo
  sob perda parcial — reescrevê-lo para controle é reuso direto, não uma
  feature nova.
- **Fila de prioridade simples**: quando há um pacote de controle
  pendente, ele é enviado no lugar do próximo fragmento de dado (fragmento
  fica "pausado", retomado de onde parou depois que o controle for
  confirmado). Não é necessário fragmentar o controle (`SeedPacket`/nova
  máscara cabem em 32 bytes tranquilamente).
- **Contadores de falha por canal, não por hop absoluto**:
  `uint8_t g_channel_fail[TABLE_SIZE]`, incrementado a cada timeout de ACK
  no índice do canal correspondente, decrementado (até 0) a cada sucesso —
  um contador tipo "leaky bucket" simples em vez de janela deslizante
  completa, suficiente para capturar "esse canal específico está morto" ao
  longo de vários ciclos da tabela.
- **Blacklist só é avaliada/aplicada enquanto o alerta de
  `spectrum-attack-classification` está ativo.** Isso evita que perda de
  pacote por motivo não relacionado a ataque (ex. distância, obstáculo)
  blackliste um canal permanentemente — o blacklist é uma resposta a
  ataque confirmado, não a qualquer perda.
- **Máscara de canais ativos compartilhada**: `uint16_t g_channel_mask`
  (bit *i* = 1 → canal `HOPPING_CHANNELS[i]` disponível). Nova função em
  `shared/hopping.h`, `get_next_active_channel(seed, counter, mask)`, que
  filtra os índices habilitados antes de aplicar o módulo — usada por
  ambos os nós assim que a máscara estiver confirmada dos dois lados.
  Precisa de pelo menos 1 canal sempre habilitado (guarda contra máscara
  vazia — nunca blacklistar o último canal restante).
- **Reseed é reação imediata (na borda do alerta), blacklist é reação
  contínua e cumulativa** enquanto o alerta persistir — refletindo a
  descrição do guia ("ao receber o alerta... transmitir MUDAR_SEED" vs.
  "isolar canais detectados sob ataque severo", que é um processo, não um
  evento único).

## Dependência: `shared/protocol.h` e `hop_counter` compartilhado

`openspec/changes/archive/2026-09-24-adaptive-rendezvous/` extrai `FragmentPacket` e os demais
tipos hoje duplicados em `mestre/src/node.h`/`escravo/src/node.h` para
`shared/protocol.h`, e adiciona um `hop_counter` explícito ao
`FragmentPacket` para os dois lados concordarem sobre o hop atual. Os novos
tipos deste change (`MUDAR_SEED`, `BLACKLIST_UPDATE`, `ControlAck`) devem
ser definidos em `shared/protocol.h` (não duplicados de novo em cada
`node.h`), e a implementação da seção 2 deste change deve rodar depois que
`adaptive-rendezvous` tiver entregue esse arquivo — sem isso não há um lugar
único para declarar os novos pacotes de controle sem repetir a definição
nos dois nós.

## Risks / Trade-offs

- Pacotes de controle ainda podem, em teoria, colidir repetidamente com o
  jammer nos poucos hops seguintes ao alerta (mesma limitação que
  fragmentos de dados já têm hoje) — mitigado pelo próprio propósito do
  blacklist: à medida que canais ruins saem de circulação, a chance de
  controle emplacar sobe a cada retry.
- Máscara de blacklist mal calibrada (threshold de falha muito baixo) pode
  esvaziar a tabela de canais rápido demais — a guarda de "nunca
  blacklistar o último canal" evita travar o sistema, mas a calibração
  fina do threshold é tarefa de validação em bancada, não uma constante
  fechada de antemão.
- Sem des-blacklist automático, uma sessão longa com múltiplos ataques
  intermitentes pode esgotar canais "saudáveis mas temporariamente
  ruidosos" — aceito como limitação conhecida (Non-Goal), documentar para
  a banca como trabalho futuro.
