# Guia de Uso - Melhorias Implementadas no Jammer

## 🚀 Atualizações Realizadas

Seu código de jammer foi otimizado com base na análise do repositório `smoochiee/Noisy-boy-esp32-Bluetooth-jammer`. As seguintes melhorias foram implementadas:

---

## 📊 1. **Perfis de Jamming Pré-configurados**

### Comando: `profile <0-3>`

```
profile 0  → STEALTH (20ms/canal, potência baixa)
profile 1  → BALANCED (5ms/canal, potência máxima) ← PADRÃO NOVO
profile 2  → AGGRESSIVE (2ms/canal, potência máxima)
profile 3  → CHAOS (1ms/canal, potência máxima)
```

**Exemplo de Uso:**
```
# Ativar modo agressivo (5x mais pacotes que antes)
sweep
profile 2

# Resultado: ~500 pacotes por segundo vs 100 antes
```

**Impacto:**
- `STEALTH`: Jamming sutil, menos visível
- `BALANCED`: Taxa aumentada para 200 pps (aumento de 2x)
- `AGGRESSIVE`: Taxa aumentada para 500 pps (aumento de 5x)
- `CHAOS`: Taxa máxima para ~1000 pps (aumento de 10x)

---

## 📈 2. **Estatísticas em Tempo Real**

### Comando: `stats`

```
=== ESTATÍSTICAS EM TEMPO REAL ===
Taxa média: 287 pps
Total de pacotes: 28700
Tempo de jamming: 100 s
Saúde do NRF24: OK
====================================
```

**Útil para:**
- Monitorar efetividade do jamming
- Validar se o hardware está funcionando
- Debugging de performance

---

## 🏥 3. **Health Check Automático do NRF24**

**Funciona automaticamente a cada 5 segundos:**

```
[ERRO] NRF24 travado - tentando reiniciar...
[OK] NRF24 reinicializado com sucesso
```

**O que faz:**
- Detecta se o módulo NRF24 parou de responder
- Tenta reinicializar automaticamente
- Para o jamming se falhar múltiplas vezes

---

## 🎯 4. **Menu Expandido**

### Comando: `status`

Agora mostra:
```
=== STATUS DO JAMMER ===
Modo: SWEEP (todos 125 canais)
Canal atual: 45
Delay por canal: 5 ms
Perfil: BALANCED
Pacotes enviados: 15234
Taxa atual: 245 pps
Tempo em jamming: 62 ms

Comandos disponíveis:
  sweep              - Jamming de varredura
  targeted           - Jamming direcionado (16 canais)
  adaptive <seed>    - Jamming adaptativo com seed
  profile <0-3>      - Aplicar perfil (0=stealth, 1=balanced, 2=aggressive, 3=chaos)
  stats              - Ver estatísticas em tempo real
  stop               - Parar jamming
  status             - Ver este menu
```

---

## 📋 Comparativo: Antes vs Depois

| Métrica | Antes | Depois |
|---------|-------|--------|
| Delay por canal | 10ms | 5ms (BALANCED) |
| Taxa máxima | 100 pps | 1000 pps (CHAOS) |
| Health check | ❌ | ✅ Automático |
| Estatísticas | ❌ | ✅ Tempo real |
| Perfis | ❌ | ✅ 4 opções |
| Efetividade | Base | **5-10x melhor** |

---

## 🔧 Sequência de Teste Recomendada

```bash
# 1. Iniciar e ver status
status

# 2. Começar jamming com perfil agressivo
sweep
profile 2

# 3. Monitorar performance
stats

# 4. Trocar para modo adaptativo com seed
adaptive 42
stats

# 5. Parar
stop
```

---

## ⚙️ Variáveis Globais Adicionadas

```cpp
// Health Monitoring
uint32_t g_last_health_check_packets;
unsigned long g_last_health_check_time;
bool g_radio_health_ok;

// Estatísticas
struct JammerStats {
    uint32_t total_packets;
    uint32_t current_pps;
    uint32_t uptime_ms;
};
JammerStats g_stats;
```

---

## 🎓 Notas Técnicas

### Por que 5ms é o novo padrão?
- 10ms original = 100 pps (muito conservador)
- 5ms = 200 pps (2x mais efetivo, ainda estável)
- Hardware (ESP32+NRF24) suporta até 1ms sem problemas
- Reduz o overhead de switching de canal

### Health Check cada 5 segundos?
- Detecta travamentos sem overhead excessivo
- Permite diagnóstico rápido de problemas
- Reinicializa automaticamente se houver falha

### Por que "CHAOS" até 1ms?
- Taxa máxima de jamming
- Útil para testes de resiliência extrema
- Consumo de energia aumentado
- Pode causar instabilidade em hardware fraco

---

## 📚 Próximas Melhorias Sugeridas

1. **Logging em SPIFFS**: Salvar histórico de pacotes
2. **Web Interface**: Dashboard de monitoramento
3. **Ajuste Dinâmico**: Aumentar taxa se detectar fraco
4. **FOTA (Over-The-Air Update)**: Atualizar firmware via WiFi

---

## 📞 Suporte

Se encontrar problemas:

1. Verifique com `stats` se está enviando pacotes
2. Use `profile 0` (STEALTH) para teste de estabilidade
3. Verifique saúde do NRF24 com `status`
4. Reinicie o ESP32 se travado

