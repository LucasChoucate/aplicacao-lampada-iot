/*
 * Lâmpada Inteligente IoT - Etapa 02 (Banco de Dados + Arduino IoT Cloud)
 *
 * Sensores: 1x LDR (fotorresistor) + 1x sensor de refletância (saída digital, ex.: TCRT5000)
 *
 * Fluxo:  ESP32 --HTTPS POST /leituras--> API (Render) --> PostgreSQL --> ponte --> Arduino IoT Cloud
 *         A resposta da API traz "leitura_ativa", que é controlada pelo dashboard do Cloud.
 *
 * Placa: ESP32 Dev Module (core "esp32" da Espressif na Arduino IDE). Não usa bibliotecas extras.
 *
 * >>> PREENCHA TODOS OS CAMPOS MARCADOS COM "TODO" ANTES DE GRAVAR <<<
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

// ===================== TODO: CONFIGURAÇÕES =====================
const char* WIFI_SSID  = "TODO_NOME_DA_REDE";
const char* WIFI_SENHA = "TODO_SENHA_DA_REDE";

// URL do serviço no Render, SEM barra no final. Ex.: "https://lampada-iot-api.onrender.com"
const char* API_URL = "https://TODO-SEU-SERVICO.onrender.com";
// Valor da variável de ambiente API_KEY do serviço no Render (aba Environment)
const char* API_KEY = "TODO_API_KEY";

const char* DISPOSITIVO = "esp32-lampada";

// Pinos. ATENÇÃO: o LDR precisa ficar em um pino do ADC1 (32, 33, 34, 35, 36, 39),
// porque o ADC2 não funciona enquanto o Wi-Fi está ligado.
const int PINO_LDR         = 34;  // TODO: confirme na sua montagem
const int PINO_REFLETANCIA = 27;  // TODO: confirme na sua montagem (saída digital D0 do módulo)

// O TCRT5000 costuma colocar a saída em LOW quando detecta. Se o seu for o contrário, troque para false.
const bool REFLETANCIA_ATIVA_EM_LOW = true;

const unsigned long INTERVALO_ENVIO_MS = 10000;  // 10 s (também mantém o Render "acordado")
// ===============================================================

bool leituraAtiva = true;          // controlada pelo dashboard (variável leitura_ativa)
unsigned long ultimoEnvio = 0;

void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Conectando ao Wi-Fi \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " conectado!" : " falhou (tento de novo depois)");
  if (WiFi.status() == WL_CONNECTED) Serial.println(WiFi.localIP());
}

// Epoch em segundos (via NTP). Retorna 0 se o relógio ainda não sincronizou -> a API usa a hora dela.
long long timestampAtual() {
  time_t agora = time(nullptr);
  return agora > 1704067200 ? (long long)agora : 0;
}

// Procura "leitura_ativa": true/false no JSON de resposta (sem precisar de biblioteca de JSON).
void atualizarConfig(const String& json) {
  int i = json.indexOf("\"leitura_ativa\"");
  if (i < 0) return;
  i = json.indexOf(':', i);
  if (i < 0) return;
  while (++i < (int)json.length() && json[i] == ' ') {}
  bool novo = json.startsWith("true", i);
  if (novo != leituraAtiva) {
    Serial.printf(">> Dashboard mudou leitura_ativa para %s\n", novo ? "true" : "false");
  }
  leituraAtiva = novo;
}

// Faz a requisição HTTPS. corpo vazio = GET, senão POST. Retorna o código HTTP (ou < 0 em erro).
int requisitar(const String& caminho, const String& corpo, String& resposta) {
  WiFiClientSecure cliente;
  cliente.setInsecure();  // não valida o certificado (suficiente para o projeto acadêmico)
  HTTPClient http;
  http.setTimeout(20000);  // o Render gratuito pode levar ~1 min para "acordar"
  if (!http.begin(cliente, String(API_URL) + caminho)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  int codigo = corpo.length() ? http.POST(corpo) : http.GET();
  resposta = codigo > 0 ? http.getString() : http.errorToString(codigo);
  http.end();
  return codigo;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Lâmpada Inteligente IoT - LDR + Refletância ===");

  pinMode(PINO_REFLETANCIA, INPUT);
  analogReadResolution(12);  // 0..4095

  conectarWiFi();
  configTime(0, 0, "pool.ntp.org", "time.google.com");  // UTC
}

void loop() {
  if (millis() - ultimoEnvio < INTERVALO_ENVIO_MS) return;
  ultimoEnvio = millis();

  conectarWiFi();

  int ldr = analogRead(PINO_LDR);
  bool refletancia = digitalRead(PINO_REFLETANCIA) == (REFLETANCIA_ATIVA_EM_LOW ? LOW : HIGH);

  Serial.printf("LDR=%4d  Refletancia=%s  leitura_ativa=%s\n",
                ldr, refletancia ? "DETECTADO" : "nada", leituraAtiva ? "sim" : "nao");

  if (WiFi.status() != WL_CONNECTED) return;

  String resposta;
  int codigo;
  if (leituraAtiva) {
    char corpo[160];
    snprintf(corpo, sizeof(corpo),
             "{\"ldr\":%d,\"refletancia\":%s,\"timestamp\":%lld,\"dispositivo\":\"%s\"}",
             ldr, refletancia ? "true" : "false", timestampAtual(), DISPOSITIVO);
    codigo = requisitar("/leituras", corpo, resposta);
    Serial.printf("POST /leituras -> %d\n", codigo);
  } else {
    // Envio pausado pelo dashboard: só consulta se já pode voltar a enviar.
    codigo = requisitar("/config", "", resposta);
    Serial.printf("Envio pausado pelo dashboard. GET /config -> %d\n", codigo);
  }

  if (codigo >= 200 && codigo < 300) {
    atualizarConfig(resposta);
  } else {
    Serial.println("Erro: " + resposta);
  }
}
