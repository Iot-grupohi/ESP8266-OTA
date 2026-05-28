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
  delay(100);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(100);
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
//  Download e gravação do firmware — duas fases:
//  Fase 1: resolve URL do CDN e obtém tamanho, fecha conexão
//  Fase 2: apaga flash (Update.begin), abre nova conexão e grava
// ============================================================
void applyUpdate(const String& startUrl) {
  digitalWrite(LED_BUILTIN, LOW);
  Serial.printf("[OTA] Heap: %d bytes\n", ESP.getFreeHeap());

  // ── Fase 1: resolver URL final e obter tamanho ─────────────
  String cdnUrl = startUrl;
  int tamanho = -1;

  for (int hop = 0; hop < 6; hop++) {
    WiFiClientSecure c;
    c.setInsecure();
    HTTPClient h;
    h.begin(c, cdnUrl);
    h.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    h.setTimeout(15000);
    h.addHeader("Accept-Encoding", "identity");
    const char* hdrs[] = { "Location" };
    h.collectHeaders(hdrs, 1);

    int code = h.GET();

    if (code == 301 || code == 302 || code == 303 ||
        code == 307 || code == 308) {
      String loc = h.header("Location");
      h.end();
      if (loc.isEmpty()) break;
      cdnUrl = loc;
      Serial.printf("[OTA] Redirect %d\n", hop + 1);
      continue;
    }

    if (code == HTTP_CODE_OK) {
      tamanho = h.getSize();
      Serial.printf("[OTA] CDN URL obtida. Tamanho: %d bytes\n", tamanho);
    } else {
      Serial.printf("[OTA] Erro HTTP: %d\n", code);
    }
    h.end(); // fecha conexão ANTES do apagamento do flash
    break;
  }

  if (tamanho <= 0) {
    Serial.println("[OTA] Tamanho inválido. Abortando.");
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }

  // ── Fase 2: apagar flash (conexão já fechada) ───────────────
  Serial.println("[OTA] Preparando flash...");
  if (!Update.begin(tamanho)) {
    Serial.printf("[OTA] Sem espaço: %s\n", Update.getErrorString().c_str());
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }
  Serial.println("[OTA] Flash pronto. Conectando ao CDN...");

  // ── Fase 3: nova conexão ao CDN e gravação ──────────────────
  WiFiClientSecure fClient;
  fClient.setInsecure();

  HTTPClient http;
  http.begin(fClient, cdnUrl);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.setTimeout(30000);
  http.addHeader("Accept-Encoding", "identity");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] Falha na conexão ao CDN: HTTP %d\n", code);
    http.end();
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  stream->setTimeout(30000);

  size_t gravados = Update.writeStream(*stream);
  Serial.printf("[OTA] writeStream: %d de %d bytes\n", gravados, tamanho);

  if (!Update.end(true)) {
    Serial.printf("[OTA] Erro ao finalizar: %s\n", Update.getErrorString().c_str());
    http.end();
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }

  Serial.printf("[OTA] %d bytes gravados. Reiniciando...\n", gravados);
  http.end();
  delay(500);
  ESP.restart();
}
