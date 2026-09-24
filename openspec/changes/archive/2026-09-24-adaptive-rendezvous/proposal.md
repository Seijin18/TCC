## Why

`PAIRING_CHANNEL` é hoje um `76` fixo, hardcoded nos dois lados
(`mestre/src/node.h`, `escravo/src/node.h`). O escravo nunca varre nada — só
escuta nesse canal fixo. Se um jammer estiver ativo sobre o canal 76 (dentro
da faixa de varredura 2–80 do `jammer/src/node.h`), mestre e escravo nunca
conseguem se encontrar: não há como negociar um canal limpo, nem como o
escravo procurar o mestre em outro lugar. Isso é diferente do problema que
`dynamic-seed-blacklist` resolve — aquele change lida com escapar de um
ataque **depois** que o hopping já está em andamento, negociando nova
seed/máscara via `ControlSeedPacket`/`ControlBlacklistPacket`
(`openspec/changes/dynamic-seed-blacklist/tasks.md`); este change lida com o
**primeiro encontro**, antes de haver qualquer seed ou hopping estabelecido,
que hoje não tem nenhuma defesa e continua sendo o único jeito de o enlace
se recuperar quando uma renegociação em voo falha.

Adicionalmente, o enlace de dados morre depois de uma única mensagem de 5
fragmentos (`g_done = true` no mestre) e a seed só pode ser configurada via
uma leitura bloqueante da serial (`wait_for_seed()`) — os dois bloqueiam
qualquer teste de bancada não assistido e qualquer medição de PLR sustentada,
pré-requisito para validar o próprio rendezvous. O contador de hop também
corre livre em cada lado desde instantes diferentes, sem nenhuma correção —
qualquer deriva desloca os dois para canais diferentes permanentemente
(indistinguível de jamming nos dados), e a comutação sincronizada que
`dynamic-seed-blacklist` precisa fazer ("os dois lados trocam seed no mesmo
hop") depende de mestre e escravo concordarem sobre qual é o hop atual.

## What Changes

- `shared/protocol.h` (novo): extrai as definições hoje duplicadas em
  `mestre/src/node.h` e `escravo/src/node.h` (`MessageType`, `FragmentPacket`,
  `FragmentAck`, `LinkPacket`, `SeedAckPacket`, tamanhos) para uma única
  fonte — pré-requisito para os pacotes de controle de
  `dynamic-seed-blacklist` não divergirem entre os dois lados. Substitui o
  `PAIRING_CHANNEL` único por `PAIRING_CANDIDATES[8]`, com a maioria fora da
  faixa 2–80 varrida pelo jammer sweep.
- **Mestre**: antes de sondar, mede atividade em cada candidato (mesma
  técnica de `testCarrier()` do analisador) e acampa no mais silencioso,
  reenviando `MSG_LINK_PROBE`; sem ACK depois de uma janela, roda para o
  próximo candidato limpo (rotação lenta).
- **Escravo**: varre os candidatos (curto período em cada, escutando) em vez
  de escutar um canal fixo — ciclo de varredura mais rápido que a rotação do
  mestre, para garantir encontro em poucos ciclos (rotação lenta × varredura
  rápida).
- Fallback de perda de sincronismo: depois de N hops sem tráfego/ACK válido,
  o escravo volta a varrer e o mestre volta a acampar — mesmo mecanismo serve
  tanto para a fase de pareamento inicial quanto para quando uma
  renegociação de `dynamic-seed-blacklist` falha e o enlace se perde de
  vez.
- Tráfego contínuo: o mestre reinicia a mensagem (novo `msg_id`) em vez de
  parar após uma única troca completa, para haver PLR sustentado e um alvo
  real para jammer/analisador.
- Seed não-bloqueante: janela curta na serial com valor default, mais um
  comando `seed <n>` em runtime — remove o bloqueio de `setup()`.
- Resync de contador de hop: `FragmentPacket` ganha `hop_counter`; o escravo
  adota o valor recebido do mestre em vez de manter contagem própria
  independente — condição necessária para a comutação sincronizada por
  `apply_at_hop` que `dynamic-seed-blacklist` faz.

## Capabilities

### New Capabilities
- `clean-channel-rendezvous`: mestre mede/acampa em canal limpo, escravo
  varre candidatos, ambos com fallback de perda de sincronismo e contador de
  hop ressincronizado a cada fragmento válido.

### Modified Capabilities
(nenhuma capability publicada previamente cobre pareamento — o pareamento
existente nunca foi formalizado como spec)

## Impact

- Novo: `shared/protocol.h`.
- `mestre/src/node.h`, `escravo/src/node.h`: reescrita da fase de
  pareamento (`try_establish_link`, `wait_for_seed`), remoção de `g_done`,
  adição de `hop_counter` ao `FragmentPacket` e do fallback de perda de
  sincronismo.
- `openspec/changes/dynamic-seed-blacklist/design.md`: adicionar nota de
  dependência — a comutação sincronizada por `apply_at_hop` (se adotada,
  ver revisão deste change) precisa do `hop_counter` compartilhado entregue
  aqui.
- Nenhuma mudança de hardware/pinagem.
