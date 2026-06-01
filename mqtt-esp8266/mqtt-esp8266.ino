#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
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
#include "store_config.h"

#define GH_BASE      "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/latest/download"
#define VERSION_URL  GH_BASE "/version.txt"
#define FIRMWARE_URL GH_BASE "/firmware.bin"

const int LED_PIN = 2;
#define LED_ON  LOW
#define LED_OFF HIGH

WiFiClient       espClient;
PubSubClient     client(espClient);
ESP8266WebServer configServer(80);
String           BASE;

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

  const bool wifiOk = (WiFi.status() == WL_CONNECTED);
  const bool mqttOk = client.connected();

  unsigned long interval;
  if (wifiOk && mqttOk) {
    interval = LED_BLINK_CONNECTED_MS;
  } else if (wifiOk) {
    interval = LED_BLINK_WIFI_ONLY_MS;
  } else {
    interval = LED_BLINK_NO_WIFI_MS;
  }

  const unsigned long now = millis();
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

int findIndex(char ids[][8], int count, String id) {
  for (int i = 0; i < count; i++) {
    if (id == ids[i]) return i;
  }
  return -1;
}

// ── Página de configuração (somente nome da loja) ────
static const char PAGE_STYLE[] PROGMEM =
  "body{font-family:system-ui,-apple-system,'Segoe UI',Roboto,Arial,sans-serif;"
  "background:#f0f4f8;color:#1e293b;margin:0;padding:24px 16px;line-height:1.5;"
  "-webkit-font-smoothing:antialiased}"
  ".card{max-width:420px;margin:0 auto;background:#fff;border-radius:12px;"
  "padding:28px 24px;box-shadow:0 4px 24px rgba(0,0,0,.08)}"
  "h1{font-size:1.5rem;font-weight:600;margin:0 0 8px;letter-spacing:-.02em}"
  ".sub{font-size:.875rem;color:#64748b;margin-bottom:24px}"
  "label{display:block;font-size:.875rem;font-weight:500;margin-bottom:6px;color:#334155}"
  "input{width:100%;padding:12px 14px;font-size:1rem;font-family:inherit;"
  "border:1px solid #cbd5e1;border-radius:8px;box-sizing:border-box}"
  "input:focus{outline:none;border-color:#2563eb;box-shadow:0 0 0 3px rgba(37,99,235,.15)}"
  "button{width:100%;margin-top:20px;padding:12px;font-size:1rem;font-weight:600;"
  "font-family:inherit;border:none;border-radius:8px;cursor:pointer;background:#2563eb;color:#fff}"
  ".link{display:block;text-align:center;margin-top:16px;font-size:.875rem;color:#64748b;"
  "text-decoration:none}"
  ".msg{text-align:center;padding:48px 24px;font-family:system-ui,-apple-system,'Segoe UI',Roboto,Arial,sans-serif}"
  ".msg h2{font-size:1.25rem;margin-bottom:8px}";

void sendHtmlPage(const String& body) {
  configServer.sendHeader("Cache-Control", "no-cache");
  configServer.send(200, "text/html; charset=iso-8859-1", body);
}

void handleConfigPage() {
  String html = F("<!DOCTYPE html><html lang='pt-BR'><head>"
                "<meta charset='iso-8859-1'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Config Loja</title><style>");
  html += FPSTR(PAGE_STYLE);
  html += F("</style></head><body><div class='card'>"
            "<h1>Configura&ccedil;&atilde;o da Loja</h1>"
            "<p class='sub'>Firmware ");
  html += FIRMWARE_VERSION;
  html += F(" &middot; IP ");
  html += WiFi.localIP().toString();
  html += F("</p><form method='POST' action='/save'>"
            "<label for='store'>Nome da loja (t&oacute;pico MQTT)</label>"
            "<input id='store' name='store' value='");
  html += cfg.storeName;
  html += F("' maxlength='15' required>"
            "<button type='submit'>Salvar</button>"
            "</form>"
            "<a class='link' href='/reset' "
            "onclick=\"return confirm('Restaurar nome padrao (pb05)?')\">"
            "Restaurar padr&atilde;o</a>"
            "</div></body></html>");

  sendHtmlPage(html);
}

void handleConfigSave() {
  if (configServer.hasArg("store")) {
    cfgCopy(cfg.storeName, sizeof(cfg.storeName), configServer.arg("store").c_str());
  }
  saveConfig();
  sendHtmlPage(
    F("<html lang='pt-BR'><head><meta charset='iso-8859-1'><style>") +
    String(FPSTR(PAGE_STYLE)) +
    F("</style></head><body><div class='msg'>"
      "<h2>Configura&ccedil;&atilde;o salva!</h2>"
      "<p>Reiniciando...</p></div></body></html>"));
  delay(1000);
  ESP.restart();
}

void handleConfigReset() {
  resetConfig();
  sendHtmlPage(
    F("<html lang='pt-BR'><head><meta charset='iso-8859-1'><style>") +
    String(FPSTR(PAGE_STYLE)) +
    F("</style></head><body><div class='msg'>"
      "<h2>Padr&atilde;o restaurado!</h2>"
      "<p>Reiniciando...</p></div></body></html>"));
  delay(1000);
  ESP.restart();
}

void startConfigServer() {
  configServer.on("/", HTTP_GET, handleConfigPage);
  configServer.on("/save", HTTP_POST, handleConfigSave);
  configServer.on("/reset", HTTP_GET, handleConfigReset);
  configServer.begin();
  Serial.println("[CFG] Página: http://" + WiFi.localIP().toString() + "/");
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
    int idx = findIndex(cfg.idWash, CFG_MACHINES, maq);
    if (idx >= 0) {
      ok = httpGet("http://" + String(cfg.ipWash[idx]) + "/lb");
      Serial.printf("[HTTP] washer %s → %s\n", maq.c_str(), ok ? "OK" : "FAIL");
    }
    client.publish((BASE + "/washer/" + maq + "/status").c_str(), ok ? "ok" : "error");
    return;
  }

  if (path.startsWith("dryer/")) {
    String maq = path.substring(6);
    int idx = findIndex(cfg.idDry, CFG_MACHINES, maq);
    int times = msg.toInt() / 15;
    if (idx < 0 || times < 1 || times > 3) {
      client.publish((BASE + "/dryer/" + maq + "/status").c_str(), "error");
      return;
    }
    client.publish((BASE + "/dryer/" + maq + "/status").c_str(), "ok");
    client.loop();
    for (int i = 0; i < times; i++) {
      ok = httpGet("http://" + String(cfg.ipDry[idx]) + "/lb");
      Serial.printf("[HTTP] dryer %s → %s\n", maq.c_str(), ok ? "OK" : "FAIL");
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
      ok = httpGet("http://" + String(cfg.ipAc) + "/" + route);
      Serial.printf("[HTTP] ac %s → %s\n", msg.c_str(), ok ? "OK" : "FAIL");
    }
    client.publish((BASE + "/ac/status").c_str(), ok ? "ok" : "error");
    return;
  }

  if (path.startsWith("doser/")) {
    String maq = path.substring(6);
    int idx = findIndex(cfg.idDos, CFG_MACHINES, maq);
    if (idx >= 0) {
      ok = httpGet("http://" + String(cfg.ipDos[idx]) + "/" + msg);
      Serial.printf("[HTTP] doser %s → %s\n", maq.c_str(), ok ? "OK" : "FAIL");
    }
    client.publish((BASE + "/doser/" + maq + "/status").c_str(), ok ? "ok" : "error");
    return;
  }

  if (path.startsWith("ping/")) {
    String sub = path.substring(5);

    if (sub == "all") {
      String json = "{";

      json += "\"washers\":{";
      for (int i = 0; i < CFG_MACHINES; i++) {
        bool on = icmpPing(cfg.ipWash[i]);
        json += "\"" + String(cfg.idWash[i]) + "\":" + (on ? "true" : "false");
        if (i < CFG_MACHINES - 1) json += ",";
      }
      json += "},\"dryers\":{";
      for (int i = 0; i < CFG_MACHINES; i++) {
        bool on = icmpPing(cfg.ipDry[i]);
        json += "\"" + String(cfg.idDry[i]) + "\":" + (on ? "true" : "false");
        if (i < CFG_MACHINES - 1) json += ",";
      }
      json += "},\"ac\":" + String(icmpPing(cfg.ipAc) ? "true" : "false") + ",\"dosers\":{";
      for (int i = 0; i < CFG_MACHINES; i++) {
        bool on = icmpPing(cfg.ipDos[i]);
        json += "\"" + String(cfg.idDos[i]) + "\":" + (on ? "true" : "false");
        if (i < CFG_MACHINES - 1) json += ",";
      }
      json += "}}";

      client.publish((BASE + "/ping/all/status").c_str(), json.c_str());
      return;
    }

    if (sub.startsWith("washer/")) {
      String maq = sub.substring(7);
      int idx = findIndex(cfg.idWash, CFG_MACHINES, maq);
      bool on = (idx >= 0) ? icmpPing(cfg.ipWash[idx]) : false;
      publishDeviceStatus(BASE + "/ping/washer/" + maq + "/status", maq.c_str(), on);
      return;
    }

    if (sub.startsWith("dryer/")) {
      String maq = sub.substring(6);
      int idx = findIndex(cfg.idDry, CFG_MACHINES, maq);
      bool on = (idx >= 0) ? icmpPing(cfg.ipDry[idx]) : false;
      publishDeviceStatus(BASE + "/ping/dryer/" + maq + "/status", maq.c_str(), on);
      return;
    }

    if (sub == "ac") {
      publishDeviceStatus(BASE + "/ping/ac/status", "ac", icmpPing(cfg.ipAc));
      return;
    }

    if (sub.startsWith("doser/")) {
      String maq = sub.substring(6);
      int idx = findIndex(cfg.idDos, CFG_MACHINES, maq);
      bool on = (idx >= 0) ? icmpPing(cfg.ipDos[idx]) : false;
      publishDeviceStatus(BASE + "/ping/doser/" + maq + "/status", maq.c_str(), on);
      return;
    }
  }

  Serial.printf("[MQTT] tópico não reconhecido: %s\n", path.c_str());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Conectando ao broker MQTT...");
    if (client.connect("ESP8266Client", cfg.mqttUser, cfg.mqttPass)) {
      Serial.println(" conectado!");
      String wildcard = BASE + "/#";
      client.subscribe(wildcard.c_str());
      Serial.println("Inscrito em: " + wildcard);
    } else {
      Serial.printf(" falhou (rc=%d), tentando em 5s...\n", client.state());
      for (int i = 0; i < 50; i++) {
        updateStatusLed();
        configServer.handleClient();
        delay(100);
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  setLed(false);

  Serial.printf("\n=== MQTT Gateway | versão %s ===\n", FIRMWARE_VERSION);

  loadConfig();
  BASE = String(cfg.storeName);

  if (!connectWiFiStatic()) {
    Serial.println("Reiniciando em 5s...");
    delay(5000);
    ESP.restart();
  }

  startConfigServer();

  checkForUpdate();
  lastOtaCheckMs = millis();

  client.setServer(cfg.mqttServer, cfg.mqttPort);
  client.setCallback(callback);
}

void loop() {
  configServer.handleClient();

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
