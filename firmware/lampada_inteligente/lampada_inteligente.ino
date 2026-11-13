/*
 * Lâmpada Inteligente IoT — ESP32-S3
 *
 * Lê os sensores e envia para a API (POST /leituras) em JSON:
 *   {"ldr":1234,"refletancia":true,"timestamp":1731456000,"dispositivo":"esp32-lampada"}
 *
 * A resposta da API traz config.leitura_ativa (controlado pelo dashboard do Arduino IoT Cloud):
 *   - true  -> continua enviando a cada INTERVALO_ENVIO_MS
 *   - false -> para de enviar e só consulta GET /config a cada INTERVALO_CONFIG_MS até ser reativado
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ======================= CONFIGURAÇÃO =======================
const char* WIFI_SSID   = "WIFI";
const char* WIFI_SENHA  = "SENHA";

// Sem barra no final. Ex.: "http://192.168.0.10:8000" (PC na mesma rede) ou "https://minha-api.onrender.com"
const char* API_URL     = "https://lampada-iot-api.onrender.com";
const char* API_KEY     = "1X9IIxykMVNzBkhi0QJiYZMVEYbAGkxJe1DAQvUoWgA=";  // mesma API_KEY do servidor
const char* DISPOSITIVO = "esp32-lampada";  // máx. 64 caracteres

const unsigned long INTERVALO_ENVIO_MS  = 5000;   // envio de leituras quando leitura_ativa = true
const unsigned long INTERVALO_CONFIG_MS = 10000;  // consulta a /config quando leitura_ativa = false
const unsigned long TIMEOUT_HTTP_MS     = 5000;

const time_t TIMESTAMP_MINIMO = 1704067200;  // 01/01/2024 (mesmo limite da API): abaixo disso o NTP não sincronizou

// Definição dos pinos
const int pinoRefletancia = 4; // GPIO 4 conectado ao pino DIGITAL (D0) do sensor
const int pinoLDR = 5;         // GPIO 5 conectado ao divisor de tensão do LDR (Analógico)

// Módulos tipo TCRT5000/LM393 costumam colocar D0 em LOW quando detectam.
// Se no seu módulo D0 = HIGH significa "detectou", troque para false.
const bool REFLETANCIA_ATIVA_EM_BAIXO = true;
// ============================================================

WiFiClient clienteHttp;
WiFiClientSecure clienteHttps;

bool leituraAtiva = true;
unsigned long ultimoEnvio = 0;
unsigned long ultimaConsultaConfig = 0;

// ---------------------- Wi-Fi e hora ----------------------
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);

  Serial.print("Conectando ao Wi-Fi");
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi conectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Não conectou ao Wi-Fi ainda; vou continuar tentando no loop.");
  }
}

// Reconexão sem travar o loop. Retorna true se está conectado.
bool garantirWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  static unsigned long ultimaTentativa = 0;
  if (millis() - ultimaTentativa >= 10000) {
    ultimaTentativa = millis();
    Serial.println("Wi-Fi desconectado, tentando reconectar...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_SENHA);
  }
  return false;
}

void iniciarNTP() {
  // Epoch é sempre UTC; o SNTP continua ressincronizando em segundo plano.
  configTime(0, 0, "pool.ntp.org", "a.st1.ntp.br");

  Serial.print("Sincronizando hora (NTP)");
  unsigned long inicio = millis();
  while (time(nullptr) < TIMESTAMP_MINIMO && millis() - inicio < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println(time(nullptr) >= TIMESTAMP_MINIMO
                   ? "Hora sincronizada."
                   : "NTP ainda não sincronizou; a API vai usar a hora do servidor.");
}

// ---------------------- HTTP ----------------------
bool apiUsaHttps() {
  return strncmp(API_URL, "https://", 8) == 0;
}

bool iniciarRequisicao(HTTPClient& http, const String& url) {
  http.setTimeout(TIMEOUT_HTTP_MS);
  http.setConnectTimeout(TIMEOUT_HTTP_MS);
  if (apiUsaHttps()) return http.begin(clienteHttps, url);
  return http.begin(clienteHttp, url);
}

// Procura "leitura_ativa": true/false no JSON de resposta (sem precisar de biblioteca).
bool extrairLeituraAtiva(const String& json, bool& valor) {
  int i = json.indexOf("\"leitura_ativa\"");
  if (i < 0) return false;
  i = json.indexOf(':', i);
  if (i < 0) return false;
  i++;
  while (i < (int)json.length() && isspace((unsigned char)json[i])) i++;
  if (json.startsWith("true", i))  { valor = true;  return true; }
  if (json.startsWith("false", i)) { valor = false; return true; }
  return false;
}

void atualizarLeituraAtiva(bool novoValor) {
  if (novoValor == leituraAtiva) return;
  leituraAtiva = novoValor;
  if (leituraAtiva) {
    Serial.println(">> Leitura REATIVADA pelo dashboard.");
    ultimoEnvio = millis() - INTERVALO_ENVIO_MS;  // envia na próxima volta do loop
  } else {
    Serial.println(">> Leitura PAUSADA pelo dashboard. Só vou consultar /config.");
    ultimaConsultaConfig = millis();
  }
}

// GET /config  -> {"leitura_ativa": true}
bool consultarConfig() {
  HTTPClient http;
  if (!iniciarRequisicao(http, String(API_URL) + "/config")) {
    Serial.println("GET /config: URL inválida");
    return false;
  }

  int codigo = http.GET();
  bool ok = false;
  if (codigo == 200) {
    bool ativa;
    if (extrairLeituraAtiva(http.getString(), ativa)) {
      atualizarLeituraAtiva(ativa);
      ok = true;
    }
  } else if (codigo < 0) {
    Serial.printf("GET /config: falha de conexão (%s)\n", http.errorToString(codigo).c_str());
  } else {
    Serial.printf("GET /config: HTTP %d\n", codigo);
  }
  http.end();
  return ok;
}

// POST /leituras
void enviarLeitura(int ldr, bool refletancia) {
  time_t agora = time(nullptr);
  char corpo[192];

  if (agora >= TIMESTAMP_MINIMO) {
    snprintf(corpo, sizeof(corpo),
             "{\"ldr\":%d,\"refletancia\":%s,\"timestamp\":%lld,\"dispositivo\":\"%s\"}",
             ldr, refletancia ? "true" : "false", (long long)agora, DISPOSITIVO);
  } else {
    // Sem hora válida: omite o timestamp e a API usa a hora do servidor.
    snprintf(corpo, sizeof(corpo),
             "{\"ldr\":%d,\"refletancia\":%s,\"dispositivo\":\"%s\"}",
             ldr, refletancia ? "true" : "false", DISPOSITIVO);
  }

  HTTPClient http;
  if (!iniciarRequisicao(http, String(API_URL) + "/leituras")) {
    Serial.println("POST /leituras: URL inválida");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  if (strlen(API_KEY) > 0) http.addHeader("X-API-Key", API_KEY);

  int codigo = http.POST((uint8_t*)corpo, strlen(corpo));

  if (codigo == 201) {
    bool ativa;
    if (extrairLeituraAtiva(http.getString(), ativa)) atualizarLeituraAtiva(ativa);
    Serial.println("  -> enviado (201)");
  } else if (codigo == 401) {
    Serial.println("  -> 401: X-API-Key inválida ou ausente. Confira API_KEY.");
  } else if (codigo == 422) {
    Serial.print("  -> 422: dados rejeitados pela API: ");
    Serial.println(http.getString());
  } else if (codigo < 0) {
    Serial.printf("  -> falha de conexão (%s). A API está acessível em %s?\n",
                  http.errorToString(codigo).c_str(), API_URL);
  } else {
    Serial.printf("  -> HTTP %d: ", codigo);
    Serial.println(http.getString());
  }
  http.end();
}

// ---------------------- sensores ----------------------
void lerEEnviar() {
  // Leitura DIGITAL do sensor de refletância (Retorna 0 ou 1)
  int valorRefletancia = digitalRead(pinoRefletancia);

  // Leitura ANALÓGICA do LDR
  int valorLDR = analogRead(pinoLDR);

  bool detectou = REFLETANCIA_ATIVA_EM_BAIXO ? (valorRefletancia == LOW) : (valorRefletancia == HIGH);

  // Exibe os valores no Monitor Serial
  Serial.print("Refletância (Digital): ");
  Serial.print(valorRefletancia);
  Serial.print(" | Luz (LDR): ");
  Serial.println(valorLDR);

  enviarLeitura(valorLDR, detectou);
}

// ---------------------- setup / loop ----------------------
void setup() {
  Serial.begin(115200);

  // Espera o Monitor Serial no máximo 3 s. Sem esse limite, o ESP32-S3 (USB CDC)
  // trava aqui para sempre quando está ligado só na fonte, sem computador.
  unsigned long inicioSerial = millis();
  while (!Serial && millis() - inicioSerial < 3000) {
    delay(10);
  }

  // Configura o pino do sensor de refletância como entrada digital
  pinMode(pinoRefletancia, INPUT);

  // Configura a resolução do ADC em 12 bits para o LDR (0 a 4095)
  analogReadResolution(12);

  clienteHttps.setInsecure();  // HTTPS sem validar certificado (ok para protótipo)

  Serial.println("Iniciando leitura dos sensores...");

  conectarWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    iniciarNTP();
    consultarConfig();  // descobre se o dashboard está com a leitura pausada antes de enviar
  }

  ultimoEnvio = millis() - INTERVALO_ENVIO_MS;  // primeiro envio imediato
  ultimaConsultaConfig = millis();
}

void loop() {
  if (!garantirWiFi()) {
    delay(100);
    return;
  }

  unsigned long agora = millis();

  if (leituraAtiva) {
    if (agora - ultimoEnvio >= INTERVALO_ENVIO_MS) {
      ultimoEnvio = agora;
      lerEEnviar();
    }
  } else {
    if (agora - ultimaConsultaConfig >= INTERVALO_CONFIG_MS) {
      ultimaConsultaConfig = agora;
      consultarConfig();
    }
  }

  delay(10);
}