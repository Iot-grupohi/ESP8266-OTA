#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESP8266Ping.h>
#include <Updater.h>

#ifndef UPDATE_SIZE_UNKNOWN
  #define UPDATE_SIZE_UNKNOWN 0xFFFFFFFF
#endif

#include "config.h"
#include "version.h"

#define GH_BASE      "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/latest/download"
#define VERSION_URL  GH_BASE "/version.txt"
#define FIRMWARE_URL GH_BASE "/firmware.bin"

// ── Configurações MQTT ──────────────────────────────
const char* mqttServer = "161.97.172.86";
const int   mqttPort   = 1883;
const char* mqttUser   = "lav60";
const char* mqttPass   = "lav60";
const char* storeName  = "pb05";

// ── IPs dos dispositivos locais ─────────────────────
const char* IP_WASH[] = { "192.168.50.100", "192.168.50.101", "192.168.50.102", "192.168.50.103" };
const char* ID_WASH[] = { "321", "432", "543", "654" };

const char* IP_DRY[]  = { "192.168.50.104", "192.168.50.105", "192.168.50.106", "192.168.50.107" };
const char* ID_DRY[]  = { "765", "876", "987", "210" };

const char* IP_AC     = "192.168.50.110";
const char* IP_DOS[]  = { "192.168.50.150", "192.168.50.151", "192.168.50.152", "192.168.50.153" };
const char* ID_DOS[]  = { "321", "432", "543", "654" };

// ── Hardware ─────────────────────────────────────────
const int LED_PIN = 2;
#define LED_ON  LOW
#define LED_OFF HIGH

WiFiClient   espClient;
PubSubClient client(espClient);
String BASE;

static uint8_t otaBuffer[OTA_CHUNK_SIZE];
static unsigned long lastOtaCheckMs = 0;

bool ledCommandOverride = false;
unsigned long lastBlink  = 0;
bool          blinkState = false;

void setLed(bool on) {
  digitalWrite(LED_PIN, on ? LED_ON : LED_OFF);
}

void updateStatusLed() {
  if (ledCommandOverride) return;

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool mqttOk = client.connected();

  if (wifiOk && mqttOk) {
    setLed(true);
    return;
  }

  unsigned long interval = wifiOk ? 800 : 200;
  unsigned long now = millis();
  if (now - lastBlink >= interval) {
    lastBlink  = now;
    blinkState = !blinkState;
    setLed(blinkState);
  }
}

bool httpGet(String url) {
  WiFiClient httpClient;
  HTTPClient http;
  http.begin(httpClient, url);
  http.setTimeout(10000);
  int code = http.GET();
  http.end();
  return (code >= 200 && code < 400) || code == 303;
}

bool icmpPing(const char* ip) {
  IPAddress addr;
  if (!addr.fromString(ip)) return false;
  return Ping.ping(addr, 2);
}

void publishDeviceStatus(String topic, const char* id, bool online) {
  String payload = "{\"id\":\"" + String(id) + "\",\"online\":" + (online ? "true" : "false") + "}";
  client.publish(topic.c_str(), payload.c_str());
}

int findIndex(const char** ids, int count, String id) {
  for (int i = 0; i < count; i++) {
    if (id == ids[i]) return i;
  }
  return -1;
}

// ── OTA ──────────────────────────────────────────────
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
    Serial.printf("[OTA] HTTP %d\n", code);
    http.end();
    return false;
  }

  out = http.getString();
  out.trim();
  http.end();
  return true;
}

void otaAbort(HTTPClient* http, bool cancelUpdate = false) {
  if (cancelUpdate && Update.isRunning()) Update.end(true);
  if (http) http->end();
  ESP.wdtEnable(0);
  setLed(false);
}

void applyUpdate(const char* url) {
  ledCommandOverride = true;
  setLed(true);
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
    ledCommandOverride = false;
    return;
  }

  const int tamanho = http.getSize();
  Serial.printf("[OTA] Tamanho: %d bytes\n", tamanho);
  if (tamanho <= 0) {
    Serial.println("[OTA] Tamanho inválido.");
    otaAbort(&http);
    ledCommandOverride = false;
    return;
  }

  if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    Serial.printf("[OTA] Sem espaço: %s\n", Update.getErrorString().c_str());
    otaAbort(&http);
    ledCommandOverride = false;
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
    ledCommandOverride = false;
    return;
  }

  if (!Update.end(true)) {
    Serial.printf("[OTA] Erro ao finalizar: %s\n", Update.getErrorString().c_str());
    otaAbort(&http);
    ledCommandOverride = false;
    return;
  }

  Serial.println("[OTA] Sucesso! Reiniciando...");
  http.end();
  delay(500);
  ESP.restart();
}

void checkForUpdate() {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("[OTA] Verificando versão...");
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

// ── Callback MQTT ─────────────────────────────────────
void callback(char* topicRaw, byte* payload, unsigned int length) {
  String topic = String(topicRaw);
  String msg   = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  Serial.printf("[MQTT] %s → %s\n", topic.c_str(), msg.c_str());

  if (topic.endsWith("/status")) return;

  String path = topic.substring(BASE.length() + 1);
  bool ok = false;

  if (path == "led") {
    ledCommandOverride = true;
    if (msg == "ON")  { setLed(true);  ok = true; }
    if (msg == "OFF") { setLed(false); ok = true; }
    if (!ok) ledCommandOverride = false;
    if (ok) client.publish((BASE + "/led/status").c_str(), msg.c_str());
    return;
  }

  if (path.startsWith("washer/")) {
    String maq = path.substring(7);
    int idx = findIndex(ID_WASH, 4, maq);
    if (idx >= 0) {
      ok = httpGet("http://" + String(IP_WASH[idx]) + "/lb");
      Serial.printf("[HTTP] washer %s → %s\n", maq.c_str(), ok ? "OK" : "FAIL");
    }
    client.publish((BASE + "/washer/" + maq + "/status").c_str(), ok ? "ok" : "error");
    return;
  }

  if (path.startsWith("dryer/")) {
    String maq = path.substring(6);
    int idx = findIndex(ID_DRY, 4, maq);
    int times = msg.toInt() / 15;
    if (idx < 0 || times < 1 || times > 3) {
      client.publish((BASE + "/dryer/" + maq + "/status").c_str(), "error");
      return;
    }
    client.publish((BASE + "/dryer/" + maq + "/status").c_str(), "ok");
    client.loop();
    for (int i = 0; i < times; i++) {
      ok = httpGet("http://" + String(IP_DRY[idx]) + "/lb");
      Serial.printf("[HTTP] dryer %s %s min call %d/%d → %s\n",
                    maq.c_str(), msg.c_str(), i + 1, times, ok ? "OK" : "FAIL");
      if (i < times - 1) delay(2000);
    }
    return;
  }

  if (path == "ac") {
    String route = "";
    if (msg == "18")  route = "airon1";
    if (msg == "22")  route = "airon2";
    if (msg == "off") route = "airon3";
    if (route.length() > 0) {
      ok = httpGet("http://" + String(IP_AC) + "/" + route);
      Serial.printf("[HTTP] ac %s → %s\n", msg.c_str(), ok ? "OK" : "FAIL");
    }
    client.publish((BASE + "/ac/status").c_str(), ok ? "ok" : "error");
    return;
  }

  if (path.startsWith("doser/")) {
    String maq = path.substring(6);
    int idx = findIndex(ID_DOS, 4, maq);
    if (idx >= 0) {
      ok = httpGet("http://" + String(IP_DOS[idx]) + "/" + msg);
      Serial.printf("[HTTP] doser %s/%s → %s\n", maq.c_str(), msg.c_str(), ok ? "OK" : "FAIL");
    }
    client.publish((BASE + "/doser/" + maq + "/status").c_str(), ok ? "ok" : "error");
    return;
  }

  if (path.startsWith("ping/")) {
    String sub = path.substring(5);

    if (sub == "all") {
      String json = "{";

      json += "\"washers\":{";
      for (int i = 0; i < 4; i++) {
        bool on = icmpPing(IP_WASH[i]);
        json += "\"" + String(ID_WASH[i]) + "\":" + (on ? "true" : "false");
        if (i < 3) json += ",";
        Serial.printf("[PING] washer %s → %s\n", ID_WASH[i], on ? "online" : "offline");
      }
      json += "},";

      json += "\"dryers\":{";
      for (int i = 0; i < 4; i++) {
        bool on = icmpPing(IP_DRY[i]);
        json += "\"" + String(ID_DRY[i]) + "\":" + (on ? "true" : "false");
        if (i < 3) json += ",";
        Serial.printf("[PING] dryer %s → %s\n", ID_DRY[i], on ? "online" : "offline");
      }
      json += ",";

      bool acOn = icmpPing(IP_AC);
      json += "\"ac\":" + String(acOn ? "true" : "false") + ",";
      Serial.printf("[PING] ac → %s\n", acOn ? "online" : "offline");

      json += "\"dosers\":{";
      for (int i = 0; i < 4; i++) {
        bool on = icmpPing(IP_DOS[i]);
        json += "\"" + String(ID_DOS[i]) + "\":" + (on ? "true" : "false");
        if (i < 3) json += ",";
        Serial.printf("[PING] doser %s → %s\n", ID_DOS[i], on ? "online" : "offline");
      }
      json += "}}";

      client.publish((BASE + "/ping/all/status").c_str(), json.c_str());
      return;
    }

    if (sub.startsWith("washer/")) {
      String maq = sub.substring(7);
      int idx = findIndex(ID_WASH, 4, maq);
      bool on = (idx >= 0) ? icmpPing(IP_WASH[idx]) : false;
      publishDeviceStatus(BASE + "/ping/washer/" + maq + "/status", maq.c_str(), on);
      Serial.printf("[PING] washer %s → %s\n", maq.c_str(), on ? "online" : "offline");
      return;
    }

    if (sub.startsWith("dryer/")) {
      String maq = sub.substring(6);
      int idx = findIndex(ID_DRY, 4, maq);
      bool on = (idx >= 0) ? icmpPing(IP_DRY[idx]) : false;
      publishDeviceStatus(BASE + "/ping/dryer/" + maq + "/status", maq.c_str(), on);
      Serial.printf("[PING] dryer %s → %s\n", maq.c_str(), on ? "online" : "offline");
      return;
    }

    if (sub == "ac") {
      bool on = icmpPing(IP_AC);
      publishDeviceStatus(BASE + "/ping/ac/status", "ac", on);
      Serial.printf("[PING] ac → %s\n", on ? "online" : "offline");
      return;
    }

    if (sub.startsWith("doser/")) {
      String maq = sub.substring(6);
      int idx = findIndex(ID_DOS, 4, maq);
      bool on = (idx >= 0) ? icmpPing(IP_DOS[idx]) : false;
      publishDeviceStatus(BASE + "/ping/doser/" + maq + "/status", maq.c_str(), on);
      Serial.printf("[PING] doser %s → %s\n", maq.c_str(), on ? "online" : "offline");
      return;
    }
  }

  Serial.printf("[MQTT] tópico não reconhecido: %s\n", path.c_str());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Conectando ao broker MQTT...");
    if (client.connect("ESP8266Client", mqttUser, mqttPass)) {
      Serial.println(" conectado!");
      String wildcard = BASE + "/#";
      client.subscribe(wildcard.c_str());
      Serial.println("Inscrito em: " + wildcard);
    } else {
      Serial.printf(" falhou (rc=%d), tentando em 5s...\n", client.state());
      for (int i = 0; i < 50; i++) {
        updateStatusLed();
        delay(100);
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  setLed(false);

  BASE = String(storeName);

  Serial.printf("\n=== MQTT Gateway | versão %s ===\n", FIRMWARE_VERSION);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Conectando ao WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    updateStatusLed();
    delay(100);
    Serial.print(".");
  }
  Serial.println("\nWiFi conectado! IP: " + WiFi.localIP().toString());

  checkForUpdate();
  lastOtaCheckMs = millis();

  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();
  updateStatusLed();

  const unsigned long now = millis();
  if (now - lastOtaCheckMs >= OTA_CHECK_INTERVAL_MS) {
    lastOtaCheckMs = now;
    checkForUpdate();
  }

  yield();
}
