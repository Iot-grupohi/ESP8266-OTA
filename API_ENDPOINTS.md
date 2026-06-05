# Documentação de Endpoints da API

Documentação extraída de `BKP01_esp8266_firebase_manager.py`.

O sistema não expõe um servidor HTTP próprio. Ele atua como **cliente** que consome:

1. **API HTTP dos dispositivos ESP8266** (rede local)
2. **Firebase Realtime Database** (controle e monitoramento em tempo real)
3. **API externa** (configurada, porém desabilitada e não implementada)

---

## 1. API HTTP dos Dispositivos ESP8266

**Padrão de URL:** `http://{IP}/{endpoint}`  
**Método:** `GET` (todas as requisições)  
**Timeout padrão:** 3s (lavadoras: 4s, secadoras: 6s)

### 1.1 Lavadoras

| Endpoint | Método | IP | ID | Descrição |
|----------|--------|-----|-----|-----------|
| `/lb` | GET | `10.1.40.101` | 432 | Libera a porta da lavadora |
| `/lb` | GET | `10.1.40.102` | 543 | Libera a porta da lavadora |
| `/lb` | GET | `10.1.40.103` | 654 | Libera a porta da lavadora |

**Gatilho:** evento Firebase em `/{STORE_ID}/lavadoras/{id}` com valor `true` ou `"liberando"`.

**Resposta esperada (sucesso):** HTTP 200–299 com indicadores como `GPIO is now high`, `high`, `OK`, `Success`.

---

### 1.2 Secadoras

| Endpoint | Método | IP | ID | Descrição |
|----------|--------|-----|-----|-----------|
| `/lb` | GET | `10.1.40.104` | 765 | Ativa a secadora |
| `/lb` | GET | `10.1.40.105` | 876 | Ativa a secadora |
| `/lb` | GET | `10.1.40.106` | 987 | Ativa a secadora |

**Gatilho:** evento Firebase em `/{STORE_ID}/secadoras/{id}_{minutos}` com valor `true` ou `"liberando"`.

**Repetições do GET conforme tempo:**
- 15 min → 1 GET
- 30 min → 2 GETs
- 45 min → 3 GETs

**Chaves Firebase de tempo:** `765_15`, `765_30`, `765_45`, `876_15`, `876_30`, `876_45`, `987_15`, `987_30`, `987_45`.

---

### 1.3 Dosadoras

**IPs:** `10.1.40.151` (432), `10.1.40.152` (543), `10.1.40.153` (654)

| Endpoint | Método | Parâmetros | Descrição |
|----------|--------|------------|-----------|
| `/status` | GET | — | Verificação de status / conectividade |
| `/am01-1` | GET | — | Executa dosagem (padrão; configurável via Firebase) |
| `/softener1` | GET | — | Amaciante tipo 1 (padrão) |
| `/softener{N}` | GET | `N` = valor do campo `amaciante` | Amaciante dinâmico (ex.: `/softener2`) |
| `/rele{N}on` | GET | `N` = 1, 2 ou 3 | Liga bomba/relé (ex.: `/rele1on`) |
| `/consultasb01` | GET | — | Consulta tempo atual do sabão (retorna segundos ou ms) |
| `/consultaam01` | GET | — | Consulta tempo atual do amaciante floral |
| `/consultaam02` | GET | — | Consulta tempo atual do amaciante sport |
| `/settime` | GET | `rele` (1–3), `time` (ms) | Ajusta tempo de dosagem (ex.: `/settime?rele=1&time=5000`) |

**Gatilhos Firebase** em `/{STORE_ID}/dosadora_01/{id}/`:

| Campo Firebase | Condição | Endpoint chamado |
|----------------|----------|------------------|
| `amaciante` | valor > 0 | `softener_endpoint` ou `/softener{N}` |
| `dosagem` | valor > 0 | `dosagem_endpoint` ou `/am01-1` |
| `bomba` | 1 ≤ valor ≤ 3 | `/rele{N}on` |
| `consulta_tempo` | `true` | `/consultasb01`, `/consultaam01`, `/consultaam02` |
| `ajuste_tempo_sabao` | valor > 0 | `/settime?rele=1&time={valor*1000}` |
| `ajuste_tempo_floral` | valor > 0 | `/settime?rele=2&time={valor*1000}` |
| `ajuste_tempo_sport` | valor > 0 | `/settime?rele=3&time={valor*1000}` |

**Endpoints configuráveis por dosadora no Firebase:**
- `dosagem_endpoint` (padrão: `am01-1`)
- `softener_endpoint` (padrão: `softener1`)

---

### 1.4 Ar Condicionado

**IP:** `10.1.40.110`

| Endpoint | Método | Comando Firebase | Descrição |
|----------|--------|------------------|-----------|
| `/airon1` | GET | `18` = `true` | Liga ar em 18°C |
| `/airon2` | GET | `22` = `true` | Liga ar em 22°C |
| `/airon3` | GET | `OFF` = `true` | Desliga o ar condicionado |

**Gatilho:** evento Firebase em `/{STORE_ID}/ar_condicionado/{18|22|OFF}`.

---

## 2. API Externa (placeholder — não implementada)

Configurada no código, porém **desabilitada** (`API_ENABLED = False`) e sem uso no fluxo atual.

| Configuração | Valor |
|--------------|-------|
| `API_ENABLED` | `false` |
| `API_ENDPOINT` | `https://api.example.com/machines/status` |
| `API_TOKEN` | `your-api-token` |
| `API_CHECK_INTERVAL` | `300` segundos (5 min) |

> Não há chamadas `requests` para este endpoint no código atual.

---

## 3. Firebase Realtime Database

**Base URL:** `https://hipag-02-default-rtdb.firebaseio.com`  
**Raiz da loja:** `/{STORE_ID}` (ex.: `/SP01`)

O Firebase funciona como API de controle em tempo real (leitura/escrita + listeners `stream`).

### 3.1 Caminhos principais

| Caminho | Tipo | Descrição |
|---------|------|-----------|
| `/{STORE_ID}/lavadoras/{id}` | write (listener) | Comando de liberação (`432`, `543`, `654`) |
| `/{STORE_ID}/secadoras/{id}_{min}` | write (listener) | Comando de secagem (`765_15`, `765_30`, etc.) |
| `/{STORE_ID}/ar_condicionado/{18\|22\|OFF}` | write (listener) | Comando do ar condicionado |
| `/{STORE_ID}/dosadora_01/{id}/*` | write (listener) | Controle da dosadora (amaciante, dosagem, bomba, tempos) |
| `/{STORE_ID}/status/lavadoras/{id}` | read/write | Status `online` / `offline` |
| `/{STORE_ID}/status/secadoras/{id}` | read/write | Status `online` / `offline` (ID de 3 dígitos) |
| `/{STORE_ID}/status/dosadoras/{id}` | read/write | Status `online` / `offline` |
| `/{STORE_ID}/status/ar_condicionado` | read/write | Status `online` / `offline` |
| `/{STORE_ID}/heartbeat` | write | Timestamp (ms) do heartbeat do sistema |
| `/{STORE_ID}/pc_status` | read/write | Status do totem/PC (`ON`/`OFF`) |
| `/{STORE_ID}/pc_status/timestamp` | write | Timestamp da última atualização do PC |
| `/{STORE_ID}/status_motherboard` | read (listener) | Estado do botão físico do totem |
| `/{STORE_ID}/reset` | write | Controle de reset (inicializado com `0`) |
| `/{STORE_ID}/config` | read/write | Configurações globais |
| `/{STORE_ID}/config/intervalo_global` | read/write | Intervalo de heartbeat e verificação de rede (s) |
| `/{STORE_ID}/config/network_check_interval` | read/write | Intervalo de verificação de rede (s) |
| `/{STORE_ID}/config/last_update` | write | Timestamp da última atualização de config |
| `/{STORE_ID}/check_network` | read/write | Flag de verificação manual de rede |
| `/{STORE_ID}/status_machines` | — | Caminho reservado no código |
| `/{STORE_ID}/intervalo_global` | read/write | Legado (compatibilidade) |
| `/{STORE_ID}/coordenadas` | read/write | Latitude e longitude da loja |

### 3.2 Estrutura de dosadora (`dosadora_01/{id}`)

| Campo | Tipo | Descrição |
|-------|------|-----------|
| `amaciante` | int | Dispara endpoint de amaciante quando > 0 |
| `dosagem` | int | Dispara endpoint de dosagem quando > 0 |
| `bomba` | int (1–3) | Dispara `/rele{N}on` quando > 0 |
| `consulta_tempo` | bool | Dispara consulta dos 3 tempos quando `true` |
| `ajuste_tempo_sabao` | int | Ajusta tempo do sabão (segundos) |
| `ajuste_tempo_floral` | int | Ajusta tempo floral (segundos) |
| `ajuste_tempo_sport` | int | Ajusta tempo sport (segundos) |
| `tempo_atual_sabao` | string | Tempo consultado do sabão |
| `tempo_atual_floral` | string | Tempo consultado floral |
| `tempo_atual_sport` | string | Tempo consultado sport |
| `dosagem_endpoint` | string | Endpoint customizado de dosagem |
| `softener_endpoint` | string | Endpoint customizado de amaciante |

### 3.3 Listeners ativos

| Listener | Caminho monitorado | Handler |
|----------|-------------------|---------|
| Unificado | `/{STORE_ID}` | `unified_stream_handler` |
| Botão totem | `/{STORE_ID}/status_motherboard` | `button_stream_handler` |

**Sub-handlers do listener unificado:**
- `lavadoras/*` → `lavadoras_stream_handler`
- `secadoras/*` → `secadoras_stream_handler`
- `ar_condicionado/*` → `ar_condicionado_stream_handler`
- `dosadora_01/*` → `processar_evento_dosadora`

---

## 4. Resumo rápido — Endpoints HTTP por dispositivo

```
Lavadoras (101–103):     GET /lb
Secadoras (104–106):     GET /lb  (1–3x conforme tempo)
Dosadoras (151–153):     GET /status, /am01-1, /softener{N}, /rele{N}on,
                         GET /consultasb01, /consultaam01, /consultaam02,
                         GET /settime?rele={N}&time={ms}
Ar Condicionado (110):   GET /airon1, /airon2, /airon3
API Externa (inativa):   GET https://api.example.com/machines/status
```

---

## 5. Variáveis de ambiente

| Variável | Descrição | Padrão |
|----------|-----------|--------|
| `STORE_ID` | Identificador da loja no Firebase | `SP01` |

---

*Gerado automaticamente a partir da análise de `BKP01_esp8266_firebase_manager.py`.*
