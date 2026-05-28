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
//  - Uma única conexão SSL com HTTPC_STRICT_FOLLOW_REDIRECTS
//  - WDT desabilitado antes do Update.begin() (~4s de apagamento)
//  - Loop manual para download (WiFiClient::readBytes() não bloqueia)
// ============================================================
void applyUpdate(const String& url) {
  digitalWrite(LED_BUILTIN, LOW);
  Serial.printf("[OTA] Heap: %d bytes\n", ESP.getFreeHeap());

  // SSL + redirect + apagamento de flash bloqueiam a CPU por >6s.
  // Desabilita WDT ANTES de qualquer operação longa (GET ou Update.begin).
  ESP.wdtDisable();

  WiFiClientSecure fClient;
  fClient.setInsecure();

  HTTPClient http;
  http.begin(fClient, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(30000);
  http.addHeader("Accept-Encoding", "identity");

  Serial.println("[OTA] Conectando ao CDN...");
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] Falha HTTP: %d\n", code);
    http.end();
    ESP.wdtEnable(0);
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }

  int tamanho = http.getSize();
  Serial.printf("[OTA] Tamanho: %d bytes\n", tamanho);
  if (tamanho <= 0) {
    Serial.println("[OTA] Tamanho inválido.");
    http.end();
    ESP.wdtEnable(0);
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }

  // Apaga setores sob demanda durante o write — evita ~4s bloqueando
  // com a conexão HTTPS já aberta (CDN fecha se demorar demais).
  if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    Serial.printf("[OTA] Sem espaço: %s\n", Update.getErrorString().c_str());
    http.end();
    ESP.wdtEnable(0);
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }

  Serial.println("[OTA] Flash pronto. Baixando...");

  WiFiClient* stream = http.getStreamPtr();
  const size_t CHUNK = 1024;
  uint8_t* buf = (uint8_t*)malloc(CHUNK);
  if (!buf) {
    Serial.println("[OTA] Sem memória para buffer.");
    Update.end(true);
    http.end();
    ESP.wdtEnable(0);
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }

  size_t gravados = 0;
  unsigned long ultimoDado = millis();

  while (gravados < (size_t)tamanho) {
    yield();

    size_t disponivel = stream->available();
    if (disponivel == 0) {
      if (!stream->connected() && gravados >= (size_t)tamanho) break;
      if (millis() - ultimoDado > 30000) {
        Serial.printf("[OTA] Timeout! %d/%d bytes\n", gravados, tamanho);
        break;
      }
      delay(1);
      continue;
    }

    ultimoDado = millis();
    size_t lido = stream->read(buf,
                  min(disponivel, min(CHUNK, (size_t)tamanho - gravados)));
    if (lido == 0) continue;

    if (Update.write(buf, lido) != lido) {
      Serial.println("[OTA] Erro ao gravar no flash");
      break;
    }
    gravados += lido;

    if (gravados % 16384 == 0) {
      Serial.printf("[OTA] %d/%d bytes\n", gravados, tamanho);
    }
  }

  free(buf);
  Serial.printf("[OTA] Gravados: %d de %d bytes\n", gravados, tamanho);

  if (gravados != (size_t)tamanho) {
    Serial.println("[OTA] Download incompleto.");
    Update.end(true);
    http.end();
    ESP.wdtEnable(0);
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }

  if (!Update.end(true)) {
    Serial.printf("[OTA] Erro ao finalizar: %s\n", Update.getErrorString().c_str());
    http.end();
    ESP.wdtEnable(0);
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }

  Serial.println("[OTA] Sucesso! Reiniciando...");
  http.end();
  delay(500);
  ESP.restart();
}
