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

  // LED pisca a cada 500 ms
  digitalWrite(LED_BUILTIN, LOW);
  delay(500);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
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

  // 1) Baixa version.txt do release mais recente
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

  String latestVersion = http.getString();
  latestVersion.trim();
  http.end();

  Serial.printf("[OTA] Versão atual: %s | Versão no repositório: %s\n",
                FIRMWARE_VERSION, latestVersion.c_str());

  // 2) Compara versões
  if (latestVersion == FIRMWARE_VERSION) {
    Serial.println("[OTA] Firmware já está atualizado.");
    return;
  }

  // 3) Nova versão encontrada → aplica atualização
  Serial.printf("[OTA] Nova versão encontrada (%s). Atualizando...\n",
                latestVersion.c_str());

  applyUpdate(FIRMWARE_URL);
}

// ============================================================
//  Download e gravação do firmware via HTTPClient + Updater
//  (suporta redirecionamentos entre domínios — ex: GitHub CDN)
// ============================================================
void applyUpdate(const String& url) {
  digitalWrite(LED_BUILTIN, LOW); // acende LED durante atualização

  WiFiClientSecure fClient;
  fClient.setInsecure();

  HTTPClient http;
  http.begin(fClient, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(30000);

  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] Falha no download: HTTP %d\n", code);
    http.end();
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }

  int tamanho = http.getSize(); // -1 se desconhecido
  Serial.printf("[OTA] Tamanho do firmware: %d bytes\n", tamanho);

  if (!Update.begin(tamanho > 0 ? tamanho : UPDATE_SIZE_UNKNOWN)) {
    Serial.printf("[OTA] Sem espaço para gravar: %s\n", Update.getErrorString().c_str());
    http.end();
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }

  // Grava o stream diretamente na flash
  WiFiClient* stream = http.getStreamPtr();
  size_t gravados = Update.writeStream(*stream);

  if (!Update.end()) {
    Serial.printf("[OTA] Erro ao finalizar gravação: %s\n", Update.getErrorString().c_str());
    http.end();
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }

  Serial.printf("[OTA] %d bytes gravados. Reiniciando...\n", gravados);
  http.end();

  delay(500);
  ESP.restart();
}
