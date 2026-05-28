// ============================================================
//  ESP8266 OTA via GitHub Releases
//  Fluxo: dispositivo verifica version.txt no release mais
//  recente e, se diferente da versão local, baixa firmware.bin
// ============================================================

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Updater.h>

#ifndef UPDATE_SIZE_UNKNOWN
  #define UPDATE_SIZE_UNKNOWN 0xFFFFFFFF
#endif

#include "config.h"
#include "version.h"

// URLs montadas a partir das configurações em config.h
#define GH_BASE      "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/latest/download"
#define VERSION_URL  GH_BASE "/version.txt"
#define FIRMWARE_URL GH_BASE "/firmware.bin"

unsigned long lastCheckMs = 0;

// ---------- protótipos ----------
void connectWiFi();
void checkForUpdate();
void applyUpdate(const String& url);

// ================================
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.printf("\n\n=== ESP8266 OTA | versão %s ===\n", FIRMWARE_VERSION);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // LED apagado (active low)

  connectWiFi();
  checkForUpdate(); // verifica logo ao ligar
}

void loop() {
  if (millis() - lastCheckMs >= OTA_CHECK_INTERVAL_MS) {
    lastCheckMs = millis();
    checkForUpdate();
  }

  // LED pisca a cada 5s
  digitalWrite(LED_BUILTIN, LOW);
  delay(1000);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(1000);
}

// ============================================================
//  Conexão Wi-Fi
// ============================================================
void connectWiFi() {
  Serial.printf("Conectando ao Wi-Fi: %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 30) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConectado! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nFalha ao conectar. Reiniciando...");
    delay(3000);
    ESP.restart();
  }
}

// ============================================================
//  Verificação de atualização OTA
// ============================================================
void checkForUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] Sem Wi-Fi, pulando verificação.");
    return;
  }

  Serial.println("[OTA] Verificando nova versão...");
  Serial.printf("[OTA] Heap livre: %d bytes\n", ESP.getFreeHeap());

  // Bloco isolado: o WiFiClientSecure é destruído ao sair,
  // liberando ~30KB de heap antes de abrir o download
  String latestVersion;
  {
    WiFiClientSecure vClient;
    vClient.setInsecure();

    HTTPClient http;
    http.begin(vClient, VERSION_URL);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(10000);

    int code = http.GET();

    if (code != HTTP_CODE_OK) {
      Serial.printf("[OTA] Erro ao obter version.txt: HTTP %d\n", code);
      http.end();
      return;
    }

    latestVersion = http.getString();
    latestVersion.trim();
    http.end();
  } // vClient destruído aqui — heap liberado

  Serial.printf("[OTA] Versão atual: %s | Versão no repositório: %s\n",
                FIRMWARE_VERSION, latestVersion.c_str());

  if (latestVersion == FIRMWARE_VERSION) {
    Serial.println("[OTA] Firmware já está atualizado.");
    return;
  }

  Serial.printf("[OTA] Nova versão encontrada (%s). Atualizando...\n",
                latestVersion.c_str());

  delay(500); // garante que o heap se estabilize
  applyUpdate(FIRMWARE_URL);
}

// ============================================================
//  Download e gravação do firmware
//  Segue redirecionamentos manualmente para garantir stream
//  limpo — HTTPC_STRICT_FOLLOW_REDIRECTS contamina o stream
//  com bytes do corpo HTML das respostas de redirect.
// ============================================================
void applyUpdate(const String& startUrl) {
  digitalWrite(LED_BUILTIN, LOW);
  Serial.printf("[OTA] Heap livre antes do download: %d bytes\n", ESP.getFreeHeap());

  String url = startUrl;

  for (int hop = 0; hop < 6; hop++) {

    // Cada iteração cria objetos novos — sem estado residual entre hops
    WiFiClientSecure fClient;
    fClient.setInsecure();

    HTTPClient http;
    http.begin(fClient, url);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setTimeout(30000);
    http.addHeader("Accept-Encoding", "identity");
    const char* hdrs[] = { "Location", "Content-Type", "Transfer-Encoding" };
    http.collectHeaders(hdrs, 3);

    int code = http.GET();

    // Redirecionar: salva nova URL, fecha conexão e repete
    if (code == 301 || code == 302 || code == 303 ||
        code == 307 || code == 308) {
      String loc = http.header("Location");
      http.end();
      // fClient e http destruídos ao sair do escopo → sem vazamento
      if (loc.isEmpty()) {
        Serial.println("[OTA] Redirect sem Location header.");
        digitalWrite(LED_BUILTIN, HIGH);
        return;
      }
      url = loc;
      Serial.printf("[OTA] Redirect %d → %s...\n", hop + 1,
                    url.substring(0, 60).c_str());
      continue;
    }

    // Erro HTTP
    if (code != HTTP_CODE_OK) {
      Serial.printf("[OTA] Falha no download: HTTP %d\n", code);
      http.end();
      digitalWrite(LED_BUILTIN, HIGH);
      return;
    }

    // 200 OK — conexão direta ao CDN, stream limpo
    Serial.printf("[OTA] Content-Type: %s\n",        http.header("Content-Type").c_str());
    Serial.printf("[OTA] Transfer-Encoding: '%s'\n", http.header("Transfer-Encoding").c_str());
    int tamanho = http.getSize();
    Serial.printf("[OTA] Tamanho: %d bytes\n", tamanho);

    // Limpa qualquer estado anterior do Update
    if (Update.isRunning()) Update.end(true);

    if (!Update.begin(tamanho > 0 ? tamanho : UPDATE_SIZE_UNKNOWN)) {
      Serial.printf("[OTA] Sem espaço: %s\n", Update.getErrorString().c_str());
      http.end();
      digitalWrite(LED_BUILTIN, HIGH);
      return;
    }

    WiFiClient* stream = http.getStreamPtr();
    stream->setTimeout(30000);

    size_t gravados = Update.writeStream(*stream);
    Serial.printf("[OTA] writeStream: %d de %d bytes\n", gravados, tamanho);

    if (!Update.end(true)) {
      Serial.printf("[OTA] Erro ao finalizar: %s\n",
                    Update.getErrorString().c_str());
      http.end();
      digitalWrite(LED_BUILTIN, HIGH);
      return;
    }

    Serial.printf("[OTA] %d bytes gravados. Reiniciando...\n", gravados);
    http.end();
    delay(500);
    ESP.restart();
    return;
  }

  Serial.println("[OTA] Erro: muitos redirecionamentos.");
  digitalWrite(LED_BUILTIN, HIGH);
}
