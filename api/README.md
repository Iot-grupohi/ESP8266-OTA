# API MQTT Gateway — LAV60

Controle remoto de lavadoras, secadoras, ar condicionado e dosadores via MQTT.

**Base URL:** `https://gateway.lav60.com`  
**Swagger:** `https://gateway.lav60.com/docs`

---

## Autenticação

Todos os endpoints (exceto `GET /`) exigem o header:

```
X-Token: seu_token
```

---

## Parâmetro `{store}`

Código da loja configurado no ESP8266 (ex.: `pb05`).

---

## Máquinas válidas

| Tipo | IDs |
|------|-----|
| Lavadora / Dosador | `321`, `432`, `543`, `654` |
| Secadora | `765`, `876`, `987`, `210` |

---

## Endpoints

### Geral

| Método | Endpoint | Descrição |
|--------|----------|-----------|
| `GET` | `/` | Verifica se a API está online |

---

### Status (máquina online?)

| Método | Endpoint | Descrição |
|--------|----------|-----------|
| `GET` | `/{store}/status` | Status de todos os equipamentos |
| `GET` | `/{store}/status/washer/{machine}` | Lavadora online? |
| `GET` | `/{store}/status/dryer/{machine}` | Secadora online? |
| `GET` | `/{store}/status/ac` | Ar condicionado online? |
| `GET` | `/{store}/status/doser/{machine}` | Dosador online? |

**Exemplo — lavadora 321:**

```http
GET https://gateway.lav60.com/pb05/status/washer/321
X-Token: seu_token
```

**Resposta:**

```json
{ "id": "321", "online": true }
```

**Exemplo — todos:**

```http
GET https://gateway.lav60.com/pb05/status
X-Token: seu_token
```

**Resposta:**

```json
{
  "washers": { "321": true, "432": false, "543": true, "654": true },
  "dryers":  { "765": true, "876": true, "987": false, "210": true },
  "ac": true,
  "dosers":  { "321": true, "432": true, "543": false, "654": true }
}
```

---

### LED

| Método | Endpoint | Body | Descrição |
|--------|----------|------|-----------|
| `POST` | `/{store}/led/on` | — | Liga o LED |
| `POST` | `/{store}/led/off` | — | Desliga o LED |
| `POST` | `/{store}/led` | `{ "command": "ON" }` ou `"OFF"` | Liga ou desliga |

---

### Lavadoras

| Método | Endpoint | Body | Descrição |
|--------|----------|------|-----------|
| `POST` | `/{store}/washer/{machine}` | — | Libera a lavadora |
| `POST` | `/{store}/washer/{machine}` | `{ "am": "am01-1" }` | Dosador AM + libera lavadora |

**Valores de `am`:** `am01-1`, `am01-2`, `am02-1`, `am02-2`

---

### Secadoras

| Método | Endpoint | Body | Descrição |
|--------|----------|------|-----------|
| `POST` | `/{store}/dryer/{machine}` | `{ "minutes": 15 }` | Inicia secadora |

**Minutos:** `15`, `30` ou `45`

---

### Ar condicionado

| Método | Endpoint | Body | Descrição |
|--------|----------|------|-----------|
| `POST` | `/{store}/ac` | `{ "temperature": "18" }` | Liga a 18°C |
| `POST` | `/{store}/ac` | `{ "temperature": "22" }` | Liga a 22°C |
| `POST` | `/{store}/ac` | `{ "temperature": "off" }` | Desliga |

---

### Dosadores

#### Comando direto (endpoint HTTP no payload)

| Método | Endpoint | Body | Descrição |
|--------|----------|------|-----------|
| `POST` | `/{store}/doser/{machine}` | `{ "type": "softener1" }` | Aciona endpoint HTTP direto |

**Tipos:** `softener0`, `softener1`, `softener2`, `softener3`, `am01-1`, `am01-2`, `am02-1`, `am02-2`, `rele1on`, `rele2on`, `rele3on`, `consultasb01`, `consultaam01`, `consultaam02`, `eepromread`, `status`

#### Ações específicas (firmware v1.5.0+)

| Método | Endpoint | Body | Descrição |
|--------|----------|------|-----------|
| `POST` | `/{store}/doser/{machine}/amaciante` | — ou `{ "number": 2 }` ou `{ "endpoint": "softener2" }` | Aciona amaciante |
| `POST` | `/{store}/doser/{machine}/dosagem` | — ou `{ "endpoint": "am01-1" }` | Executa dosagem |
| `POST` | `/{store}/doser/{machine}/bomba` | `{ "pump": 2 }` | Liga bomba/relé (1–3) |
| `GET` | `/{store}/doser/{machine}/consulta` | — | Consulta tempos sabão/floral/sport |
| `POST` | `/{store}/doser/{machine}/settime` | `{ "rele": 1, "seconds": 2.5 }` | Ajusta tempo de dosagem (segundos) |
| `POST` | `/{store}/doser/{machine}/settime/sabao` | `{ "seconds": 2.5 }` | Ajusta tempo do sabão (relé 1) |
| `POST` | `/{store}/doser/{machine}/settime/floral` | `{ "seconds": 2.5 }` | Ajusta tempo floral (relé 2) |
| `POST` | `/{store}/doser/{machine}/settime/sport` | `{ "seconds": 2.5 }` | Ajusta tempo sport (relé 3) |
| `GET` | `/{store}/doser/{machine}/device-status` | — | Conectividade HTTP da dosadora |

**Exemplo — consulta de tempos:**

```http
GET https://gateway.lav60.com/pb05/doser/432/consulta
X-Token: seu_token
```

**Resposta (tempos em segundos):**

```json
{
  "store": "pb05",
  "machine": "432",
  "tempos": {
    "sabao": 11,
    "floral": 13,
    "sport": 12
  }
}
```

> O dispositivo retorna milissegundos; a API converte para segundos. Valores decimais são suportados (ex.: `2.5` = 2,5 segundos).

**Exemplo — acionar bomba 2:**

```bash
curl -X POST -H "X-Token: seu_token" \
  -H "Content-Type: application/json" \
  -d '{"pump": 2}' \
  https://gateway.lav60.com/pb05/doser/432/bomba
```

---

## Erros comuns

| Código | Significado |
|--------|-------------|
| `401` | Token inválido ou ausente |
| `400` | Parâmetro inválido ou ESP8266 não respondeu a tempo |
| `503` | API não conectou ao broker MQTT |

---

## Exemplo curl

```bash
# Status da lavadora 321
curl -H "X-Token: seu_token" https://gateway.lav60.com/pb05/status/washer/321

# Liberar lavadora 321
curl -X POST -H "X-Token: seu_token" https://gateway.lav60.com/pb05/washer/321

# Secadora 765 por 30 min
curl -X POST -H "X-Token: seu_token" \
  -H "Content-Type: application/json" \
  -d '{"minutes": 30}' \
  https://gateway.lav60.com/pb05/dryer/765

# Consultar tempos da dosadora 432
curl -H "X-Token: seu_token" https://gateway.lav60.com/pb05/doser/432/consulta

# Ajustar tempo do sabão para 5 segundos
curl -X POST -H "X-Token: seu_token" \
  -H "Content-Type: application/json" \
  -d '{"seconds": 5}' \
  https://gateway.lav60.com/pb05/doser/432/settime/sabao
```
