# Lâmpada Inteligente IoT — Camada de Aplicação (AV2)

Sistema IoT com **ESP32** que lê **2 fotorresistores (LDR)** e **1 sensor de refletância**, envia as leituras
para uma **API em Python (FastAPI)** hospedada no **Render**, grava tudo em **PostgreSQL** e publica os dados
num **dashboard do Arduino IoT Cloud**, que também controla o dispositivo.

> A Etapa 01 (circuito completo em laboratório) já foi entregue. Esta branch (`feat/lampada-lab`) cobre as
> Etapas 02 e 03: banco de dados, integração com o Arduino IoT Cloud e versionamento.

## Arquitetura

```
┌──────────┐ HTTPS POST /leituras ┌──────────────────────┐   SQL   ┌──────────────┐
│  ESP32   │ ───────────────────► │  API FastAPI (Render) │ ──────► │  PostgreSQL  │
│ LDR1     │ ◄─────────────────── │                      │ ◄────── │   (Render)   │
│ LDR2     │  resposta: config    │  thread "ponte"      │         └──────────────┘
│ Refletân.│  (leitura_ativa)     └──────────┬───────────┘
└──────────┘                                 │ MQTT (biblioteca arduino-iot-cloud)
                                             ▼
                                ┌──────────────────────────┐
                                │ Arduino IoT Cloud         │
                                │ Thing "Ponte_BD"          │
                                │ ldr1, ldr2, refletancia   │──► Dashboard (gráficos + indicadores)
                                │ leitura_ativa (switch)    │◄── usuário liga/desliga o envio
                                └──────────────────────────┘
```

- **ESP32 → API:** a cada 10 s o ESP32 envia `ldr1`, `ldr2`, `refletancia` e o `timestamp` (NTP).
- **API → Banco:** cada leitura vira uma linha na tabela `leituras`.
- **Banco → Arduino Cloud:** a "ponte" (`backend/ponte_arduino_cloud.py`) é um *dispositivo manual* do
  Arduino IoT Cloud. A cada 5 s ela lê a leitura mais recente do banco e atualiza as variáveis da Thing.
- **Arduino Cloud → ESP32:** ao mudar o switch `leitura_ativa` no dashboard, a ponte grava o valor no banco
  e a API o devolve ao ESP32 na próxima resposta. Com `false`, o ESP32 para de enviar leituras.

## Estrutura do repositório

```
firmware/lampada_inteligente/lampada_inteligente.ino   firmware do ESP32
backend/app.py                                         API (rotas REST)
backend/database.py                                    modelos e conexão com o banco
backend/ponte_arduino_cloud.py                         ponte Banco <-> Arduino IoT Cloud
backend/requirements.txt                               dependências Python
backend/.env.example                                   variáveis de ambiente usadas
render.yaml                                            Blueprint de deploy no Render (API + PostgreSQL)
```

## 1. Como montar o projeto (hardware)

| Componente | Pino do componente | Pino do ESP32 |
|---|---|---|
| LDR 1 (divisor com resistor de 10 kΩ) | ponto médio do divisor | GPIO 34 |
| LDR 2 (divisor com resistor de 10 kΩ) | ponto médio do divisor | GPIO 35 |
| Sensor de refletância (ex.: TCRT5000) | D0 (saída digital) | GPIO 27 |
| Todos | VCC / GND | 3V3 / GND |

Divisor de tensão de cada LDR: `3V3 ── LDR ──┬── 10 kΩ ── GND`, com o ponto `┬` ligado ao GPIO.

> Os LDRs precisam ficar em pinos do **ADC1** (32–39). O ADC2 não funciona com o Wi-Fi ligado.

### Firmware

1. Arduino IDE → instale a placa **esp32** (Espressif) e selecione **ESP32 Dev Module**.
2. Abra `firmware/lampada_inteligente/lampada_inteligente.ino`.
3. Preencha os campos marcados com `TODO`: `WIFI_SSID`, `WIFI_SENHA`, `API_URL` (URL do Render) e `API_KEY`
   (o mesmo valor da variável `API_KEY` do serviço no Render). Confira os pinos.
4. Grave e abra o Monitor Serial em **115200**. Deve aparecer `POST /leituras -> 201` a cada 10 s.

## 2. Como rodar o backend

### No Render (produção)

1. No Render: **New → Blueprint** → selecione este repositório e a branch `feat/lampada-lab`.
   O `render.yaml` cria o web service `lampada-iot-api` e o banco `lampada-iot-db`.
2. Quando pedir, preencha `ARDUINO_DEVICE_ID` e `ARDUINO_SECRET_KEY` (veja a seção 3).
   `API_KEY` é gerada automaticamente; copie o valor da aba **Environment** para o firmware.
3. Após o deploy, abra `https://<seu-servico>.onrender.com/docs` para ver e testar a API.

> **Plano gratuito do Render:** o serviço "dorme" após 15 min sem requisições, e a ponte com o Cloud para junto.
> Com o ESP32 enviando a cada 10 s ele fica acordado. A primeira requisição após dormir leva ~1 min.
> O PostgreSQL gratuito expira 30 dias após ser criado.

### Localmente

```bash
cd backend
python -m venv .venv
.venv\Scripts\activate          # Linux/Mac: source .venv/bin/activate
pip install -r requirements.txt
uvicorn app:app --reload
```

Sem `DATABASE_URL` a API usa SQLite (`backend/lampada_iot.db`). Sem `API_KEY` o POST fica sem autenticação.
Sem `ARDUINO_DEVICE_ID`/`ARDUINO_SECRET_KEY` a ponte não inicia, e a API funciona normalmente.

A ponte também pode rodar sozinha, apontando para o banco do Render (use a *External Database URL*):

```powershell
$env:DATABASE_URL="postgresql://...render.com/lampada_iot"
$env:ARDUINO_DEVICE_ID="..."; $env:ARDUINO_SECRET_KEY="..."
python ponte_arduino_cloud.py
```

### Variáveis de ambiente

| Variável | Para que serve | Padrão |
|---|---|---|
| `DATABASE_URL` | conexão com o PostgreSQL | SQLite local |
| `API_KEY` | autenticação do ESP32 (header `X-API-Key`) | vazio = sem autenticação |
| `RETENCAO_DIAS` | apaga leituras mais antigas que N dias (0 = nunca) | 30 |
| `ARDUINO_DEVICE_ID` | Device ID do dispositivo manual no Arduino Cloud | — |
| `ARDUINO_SECRET_KEY` | Secret Key do dispositivo manual | — |
| `PONTE_INTERVALO_S` | intervalo da ponte Banco → Cloud, em segundos | 5 |

## 3. Arduino IoT Cloud

1. **Devices → Add → Any device / Manual** (dispositivo Python). Nome: `Ponte_BD`.
   Guarde o **Device ID** e a **Secret Key** (a Secret Key só aparece uma vez).
2. **Things → Create Thing** `Lampada_Inteligente` e associe o device `Ponte_BD`.
3. Crie as variáveis com **exatamente** estes nomes:

   | Nome | Tipo | Permissão | Update policy |
   |---|---|---|---|
   | `ldr1` | Integer Number | Read Only | On change |
   | `ldr2` | Integer Number | Read Only | On change |
   | `refletancia` | Boolean | Read Only | On change |
   | `leitura_ativa` | Boolean | Read & Write | On change |

4. **Dashboards → Create**, ligando os widgets às variáveis:
   - **Chart** + **Value** para `ldr1` e `ldr2` (luminosidade ao longo do tempo);
   - **LED/Status** para `refletancia`;
   - **Switch** para `leitura_ativa` (pausa/retoma o envio do ESP32).

   Os widgets atualizam sozinhos sempre que a ponte publica um valor novo.

## 4. Banco de dados

PostgreSQL gerenciado pelo Render. As tabelas são criadas automaticamente quando a API inicia.

**`leituras`**

| Coluna | Tipo | Descrição |
|---|---|---|
| `id` | integer (PK) | identificador |
| `timestamp` | timestamptz | momento da medição (NTP do ESP32; se não houver, hora do servidor) |
| `recebido_em` | timestamptz | momento em que a API recebeu |
| `dispositivo` | varchar(64) | identificação do ESP32 |
| `ldr1`, `ldr2` | integer | leitura bruta do ADC (0–4095) |
| `refletancia` | boolean | `true` = sensor detectou |

**`configuracoes`** (chave/valor): guarda os comandos vindos do dashboard. Hoje só existe `leitura_ativa`.

- **Autenticação:** só quem envia o header `X-API-Key` correto consegue gravar (`POST /leituras`).
  O acesso ao PostgreSQL é protegido por usuário e senha gerados pelo Render.
- **Retenção:** leituras com mais de `RETENCAO_DIAS` dias são apagadas automaticamente.

## 5. Como consultar os dados salvos

Pela API (abre no navegador):

| Rota | Retorno |
|---|---|
| `GET /leituras?limite=100` | últimas leituras (máx. 1000) |
| `GET /leituras/ultima` | leitura mais recente |
| `GET /config` | configuração atual (`leitura_ativa`) |
| `GET /docs` | documentação interativa (Swagger) |

Enviar uma leitura de teste sem o ESP32:

```bash
curl -X POST https://<seu-servico>.onrender.com/leituras \
  -H "Content-Type: application/json" -H "X-API-Key: <API_KEY>" \
  -d '{"ldr1": 1200, "ldr2": 3100, "refletancia": true}'
```

Direto no PostgreSQL (Render → banco → **Connect** → *PSQL Command*):

```sql
SELECT * FROM leituras ORDER BY id DESC LIMIT 20;
SELECT date_trunc('minute', timestamp) AS minuto, avg(ldr1), avg(ldr2)
FROM leituras GROUP BY 1 ORDER BY 1 DESC LIMIT 60;
```
