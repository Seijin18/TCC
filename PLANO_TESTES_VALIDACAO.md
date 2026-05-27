# Plano de Testes - Validação das Melhorias

## ✅ Checklist de Validação

### Fase 1: Compilação e Flasher (✅ COMPLETO)

```
[✅] Compilação sem erros
[  ] Flash no ESP32
[  ] Serial output no boot
```

**Passos:**
```bash
# 1. Compilar
platformio run --environment jammer

# 2. Flasher (via USB)
platformio run --target upload --environment jammer --upload-port COM3

# 3. Verificar boot message no Serial (115200 baud)
# Deve ver: "=== JAMMER ANTI-FHSS ==="
```

---

### Fase 2: Comandos Básicos

```
[  ] Comando: status
[  ] Comando: sweep
[  ] Comando: targeted
[  ] Comando: adaptive 50
[  ] Comando: stop
```

**Teste:**
```
> status
# Deve mostrar menu com todos os comandos

> sweep
# Deve ativar jamming sweep
# LED deve piscar rapidamente

> stats
# Deve mostrar:
# - Taxa média (pps)
# - Total de pacotes
# - Tempo de jamming
# - Saúde do NRF24 (OK)

> stop
# Deve parar jamming
# LED deve apagar
```

---

### Fase 3: Perfis de Jamming

```
[  ] Profile 0 - STEALTH
[  ] Profile 1 - BALANCED (padrão)
[  ] Profile 2 - AGGRESSIVE
[  ] Profile 3 - CHAOS
```

**Teste cada um:**
```
# STEALTH
sweep
profile 0
stats
# Esperado: ~50 pps, potência baixa

# BALANCED (novo padrão)
profile 1
stats
# Esperado: ~200 pps, potência máxima

# AGGRESSIVE
profile 2
stats
# Esperado: ~500 pps, potência máxima

# CHAOS
profile 3
stats
# Esperado: ~1000 pps, potência máxima
```

---

### Fase 4: Health Check

```
[  ] Detecção de NRF24 online
[  ] Reinicialização automática (teste manual)
[  ] Log de falha/recuperação
```

**Teste:**
```
# 1. Iniciar jamming
sweep

# 2. Aguardar 5 segundos
# Deve ver em status: "Saúde do NRF24: OK"

# 3. Desligar ou remover NRF24 (opcional)
# Após 5s, deve ver: "[ERRO] NRF24 travado - tentando reiniciar..."
# Reconectar: "[OK] NRF24 reinicializado com sucesso"

# 4. Ver status novamente
status
# Deve mostrar saúde recuperada
```

---

### Fase 5: Taxa de Pacotes

```
[  ] Validar incremento de 100 → 200 pps
[  ] Validar modo CHAOS até 1000 pps
[  ] Validar mudança instantânea de perfil
```

**Teste com analisador externo (opcional):**
```
# Usar HackRF One ou RTL-SDR para validar
# Equipamento: Software-defined radio (SDR)

# Esperado:
# STEALTH: ~50 pacotes/s em 2.4 GHz
# BALANCED: ~200 pacotes/s
# AGGRESSIVE: ~500 pacotes/s
# CHAOS: ~1000 pacotes/s
```

---

### Fase 6: Modos de Jamming

```
[  ] SWEEP: Varre todos 125 canais
[  ] TARGETED: Apenas 16 canais da tabela
[  ] ADAPTIVE: Seed-based com diferentes seeds
```

**Teste:**
```
# SWEEP
sweep
# Logs: "Canal 0, 1, 2, ..., 124, 0, 1..."

# TARGETED
targeted
# Logs: "Canal 10, 22, 34, 46..."

# ADAPTIVE com seed
adaptive 25
# Logs: Sequência determinística com seed=25

adaptive 25
# Mesma sequência deve repetir

adaptive 42
# Sequência diferente com seed=42
```

---

### Fase 7: Estabilidade

```
[  ] Rodando 1 minuto sem erros
[  ] Rodando 5 minutos sem erros
[  ] Trocar de modo/perfil sem travamento
[  ] Sem memory leaks (RAM estável)
```

**Teste:**
```bash
# Monitor RAM a cada mudança
status
# Deve mostrar sempre o mesmo valor de "Pacotes enviados" por segundo

# Após 1 minuto
stats
# Taxa atual deve estar próxima à taxa média

# Após 5 minutos
stats
# Sem degradação de performance

# Trocar modo
sweep
# (aguarde 10s)
targeted
# (aguarde 10s)
stop
# (sem erros)
```

---

### Fase 8: Serial Robustness

```
[  ] Comandos inválidos não causam crash
[  ] Inputs muito longos ignorados
[  ] Sem buffer overflow
```

**Teste:**
```
> asdfghjkl
# [JAMMER] Comando desconhecido: asdfghjkl

> sweep sweep sweep sweep sweep sweep
# Deve ignorar ou aceitar primeira palavra

> [MUITO TEXTO LONGO POR MUITO TEMPO...]
# Não deve crashear

> profile 99
# [JAMMER] Perfil inválido. Use: profile <0-3>
```

---

## 📊 Resultados Esperados

### Taxa de Pacotes

| Perfil | Delay/Canal | PPS Esperado | PPS Tolerância |
|--------|-------------|--------------|----------------|
| STEALTH | 20ms | 50 | ±10 |
| BALANCED | 5ms | 200 | ±20 |
| AGGRESSIVE | 2ms | 500 | ±50 |
| CHAOS | 1ms | 1000 | ±100 |

### Health Check

```
✅ Detecta NRF24 travado em < 10s
✅ Tenta reinicializar 1x
✅ Volta ao normal em < 2s
✅ Log apropriado em Serial
```

### Memória

```
✅ RAM: < 30KB usado (temos 320KB)
✅ Flash: < 300KB usado (temos 1.3MB)
✅ Sem crescimento durante operação
```

---

## 🐛 Possíveis Issues e Soluções

| Problema | Causa | Solução |
|----------|-------|---------|
| Não envia pacotes | NRF24 não inicializa | Verificar fiação/SPI |
| Taxa baixa | Perfil STEALTH ativo | Usar `profile 1` ou superior |
| Travamento | NRF24 desconectado | Health check detecta, reinicia |
| Serial lento | Baud rate errado | Usar 115200 |
| Comando ignorado | Syntax error | Ver `status` para help |

---

## 📋 Relatório de Testes

### Modelo de Relatório

```
Data: ___________
ESP32: [ ] Working [ ] Issue: ___________
NRF24: [ ] OK [ ] Falho [ ] Intermitente

Testes Passou:
[  ] Compilação
[  ] Boot
[  ] Status command
[  ] Sweep mode
[  ] Targeted mode
[  ] Adaptive mode (seed = ___)
[  ] Profile STEALTH
[  ] Profile BALANCED
[  ] Profile AGGRESSIVE
[  ] Profile CHAOS
[  ] Health check
[  ] Stats command
[  ] Serial robustness

Taxa média obtida:
- STEALTH: _____ pps (esperado: ~50)
- BALANCED: _____ pps (esperado: ~200)
- AGGRESSIVE: _____ pps (esperado: ~500)
- CHAOS: _____ pps (esperado: ~1000)

Notas:
_________________________________
_________________________________
_________________________________

Aprovado para uso: [ ] SIM [ ] NÃO
```

---

## 🎯 Critério de Sucesso

✅ **SUCESSO** se:
- Compila sem erros
- Flash funciona
- Todos os comandos respondem
- Taxa de pacotes dentro de tolerância
- Health check funciona
- Sem crashes em 5+ minutos

⚠️ **ATENÇÃO** se:
- Taxa muito baixa ou alta
- Erros ocasionais de serial
- NRF24 precisa reiniciar com frequência

❌ **FALHA** se:
- Não compila
- Não flasheia
- Crashes na serial
- NRF24 não responde
- Muitos erros de buffer

---

## 📚 Próximas Etapas Pós-Validação

1. **Se tudo OK:**
   - Documentar resultados
   - Adicionar ao seu relatório de TCC
   - Considerar melhorias futuras

2. **Se houver issues:**
   - Verificar cabeamento NRF24
   - Testar SPI com exemplo simples
   - Verificar alimentação (3.3V)

3. **Para publicar:**
   - Criar branch `release/v2.0`
   - Atualizar README com novas funcionalidades
   - Adicionar exemplos de uso

---

## 🔗 Referências

- [ESP32 Serial Monitor Setup](https://docs.platformio.org/en/latest/boards/espressif32/esp32dev.html)
- [RF24 Library Documentation](https://nrf24.github.io/RF24/)
- [Teste de NRF24](https://github.com/nRF24/RF24/blob/master/examples/GettingStarted_HandlingData/GettingStarted_HandlingData.ino)

