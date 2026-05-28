// ============================================================
//  ESP8266 OTA via GitHub Releases
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

#define GH_BASE      "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/latest/download"
#define VERSION_URL  GH_BASE "/version.txt"
#define FIRMWARE_URL GH_BASE "/firmware.bin"

static uint8_t otaBuffer[OTA_CHUNK_SIZE];
static unsigned long lastCheckMs = 0;
static unsigned long lastLedMs   = 0;

// ---------- protótipos ----------
void connectWiFi();
void checkForUpdate();
bool fetchText(const char* url, String& out, uint16_t timeoutMs);
void applyUpdate(const char* url);
void otaAbort(HTTPClient* http, bool cancelUpdate = false);

// ================================
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.printf("\n\n=== ESP8266 OTA | versão %s ===\n", FIRMWARE_VERSION);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  connectWiFi();
  checkForUpdate();
  lastCheckMs = millis();
}

void loop() {
  const unsigned long now = millis();

  if (now - lastCheckMs >= OTA_CHECK_INTERVAL_MS) {
    lastCheckMs = now;
    checkForUpdate();
  }

  if (now - lastLedMs >= LED_BLINK_INTERVAL_MS) {
    lastLedMs = now;
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }

  yield();
}

// ============================================================
//  Wi-Fi
// ============================================================
void connectWiFi() {
  Serial.printf("Conectando ao Wi-Fi: %s", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint8_t tentativas = 0;
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
//  HTTP GET simples (texto pequeno, ex.: version.txt)
//  WiFiClientSecure é destruído ao retornar — libera heap p/ OTA
// ============================================================
bool fetchText(const char* url, String& out, uint16_t timeoutMs) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(4096, 512);

  HTTPClient http;
  http.begin(client, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(timeoutMs);
  http.addHeader("Accept-Encoding", "identity");

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] HTTP %d em %s\n", code, url);
    http.end();
    return false;
  }

  out = http.getString();
  out.trim();
  http.end();
  return true;
}

// ============================================================
//  Verificação de atualização
// ============================================================
void checkForUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] Sem Wi-Fi.");
    return;
  }

  Serial.println("[OTA] Verificando nova versão...");
  Serial.printf("[OTA] Heap: %d bytes\n", ESP.getFreeHeap());

  String latestVersion;
  if (!fetchText(VERSION_URL, latestVersion, 10000)) return;

  Serial.printf("[OTA] Local: %s | Remoto: %s\n",
                FIRMWARE_VERSION, latestVersion.c_str());

  if (latestVersion == FIRMWARE_VERSION) {
    Serial.println("[OTA] Firmware atualizado.");
    return;
  }

  Serial.printf("[OTA] Atualizando para %s...\n", latestVersion.c_str());
  applyUpdate(FIRMWARE_URL);
}

// ============================================================
//  Limpeza em caso de falha no OTA
// ============================================================
void otaAbort(HTTPClient* http, bool cancelUpdate) {
  if (cancelUpdate && Update.isRunning()) Update.end(true);
  if (http) http->end();
  ESP.wdtEnable(0);
  digitalWrite(LED_BUILTIN, HIGH);
}

// ============================================================
//  Download e gravação do firmware
// ============================================================
void applyUpdate(const char* url) {
  digitalWrite(LED_BUILTIN, LOW);
  Serial.printf("[OTA] Heap: %d bytes\n", ESP.getFreeHeap());

  ESP.wdtDisable();

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(OTA_HTTP_TIMEOUT_MS);
  http.addHeader("Accept-Encoding", "identity");

  Serial.println("[OTA] Conectando ao CDN...");
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] Falha HTTP: %d\n", code);
    otaAbort(&http);
    return;
  }

  const int tamanho = http.getSize();
  Serial.printf("[OTA] Tamanho: %d bytes\n", tamanho);
  if (tamanho <= 0) {
    Serial.println("[OTA] Tamanho inválido.");
    otaAbort(&http);
    return;
  }

  if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    Serial.printf("[OTA] Sem espaço: %s\n", Update.getErrorString().c_str());
    otaAbort(&http);
    return;
  }

  Serial.println("[OTA] Baixando...");

  WiFiClient* stream = http.getStreamPtr();
  size_t gravados = 0;
  size_t proximoLog = OTA_PROGRESS_BYTES;
  unsigned long ultimoDado = millis();

  while (gravados < (size_t)tamanho) {
    yield();

    const size_t disponivel = stream->available();
    if (disponivel == 0) {
      if (!stream->connected()) break;
      if (millis() - ultimoDado > OTA_DOWNLOAD_TIMEOUT_MS) {
        Serial.printf("[OTA] Timeout em %d/%d bytes\n", gravados, tamanho);
        break;
      }
      delay(1);
      continue;
    }

    ultimoDado = millis();
    const size_t want = min(disponivel, min((size_t)OTA_CHUNK_SIZE,
                                            (size_t)tamanho - gravados));
    const size_t lido = stream->read(otaBuffer, want);
    if (lido == 0) continue;

    if (Update.write(otaBuffer, lido) != lido) {
      Serial.println("[OTA] Erro ao gravar.");
      break;
    }

    gravados += lido;
    if (gravados >= proximoLog) {
      Serial.printf("[OTA] %d/%d bytes\n", gravados, tamanho);
      proximoLog += OTA_PROGRESS_BYTES;
    }
  }

  Serial.printf("[OTA] Gravados: %d/%d bytes\n", gravados, tamanho);

  if (gravados != (size_t)tamanho) {
    otaAbort(&http, true);
    return;
  }

  if (!Update.end(true)) {
    Serial.printf("[OTA] Erro ao finalizar: %s\n", Update.getErrorString().c_str());
    otaAbort(&http);
    return;
  }

  Serial.println("[OTA] Sucesso! Reiniciando...");
  http.end();
  delay(500);
  ESP.restart();
}
