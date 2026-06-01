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

// ── Pagina de configuracao ───────────────────────────
static const char PAGE_STYLE[] PROGMEM =
  "*{box-sizing:border-box;margin:0;padding:0}"
  "body{font-family:system-ui,-apple-system,'Segoe UI',Roboto,Arial,sans-serif;"
  "background:linear-gradient(160deg,#0f172a 0%,#1e293b 50%,#334155 100%);"
  "min-height:100vh;color:#e2e8f0;padding:24px 16px 40px;-webkit-font-smoothing:antialiased}"
  ".wrap{max-width:480px;margin:0 auto}"
  ".brand{display:flex;align-items:center;gap:12px;margin-bottom:24px}"
  ".logo{width:44px;height:44px;border-radius:12px;background:linear-gradient(135deg,#3b82f6,#2563eb);"
  "display:flex;align-items:center;justify-content:center;font-size:1.25rem;font-weight:700;color:#fff;"
  "box-shadow:0 4px 14px rgba(37,99,235,.4)}"
  ".brand h1{font-size:1.125rem;font-weight:600;color:#f8fafc;letter-spacing:-.01em}"
  ".brand p{font-size:.75rem;color:#94a3b8;margin-top:2px}"
  ".store-card{background:rgba(255,255,255,.06);border:1px solid rgba(255,255,255,.1);"
  "border-radius:16px;padding:24px;margin-bottom:20px;backdrop-filter:blur(8px)}"
  ".store-label{font-size:.6875rem;font-weight:600;text-transform:uppercase;"
  "letter-spacing:.08em;color:#94a3b8;margin-bottom:8px}"
  ".store-name{font-size:2.25rem;font-weight:700;color:#fff;letter-spacing:.04em;"
  "font-variant-numeric:tabular-nums;line-height:1.1}"
  ".store-topic{font-size:.8125rem;color:#64748b;margin-top:10px;font-family:ui-monospace,monospace;"
  "background:rgba(0,0,0,.25);padding:8px 12px;border-radius:8px;display:inline-block}"
  ".status-row{display:flex;gap:10px;margin-top:20px;flex-wrap:wrap}"
  ".pill{display:inline-flex;align-items:center;gap:6px;font-size:.75rem;font-weight:500;"
  "padding:6px 12px;border-radius:999px;background:rgba(255,255,255,.08);color:#cbd5e1}"
  ".dot{width:7px;height:7px;border-radius:50%;background:#64748b}"
  ".dot.on{background:#22c55e;box-shadow:0 0 8px rgba(34,197,94,.6)}"
  ".panel{background:#fff;border-radius:16px;padding:24px;color:#1e293b;"
  "box-shadow:0 20px 50px rgba(0,0,0,.25)}"
  ".panel h2{font-size:1rem;font-weight:600;margin-bottom:4px;color:#0f172a}"
  ".panel .hint{font-size:.8125rem;color:#64748b;margin-bottom:20px}"
  "label{display:block;font-size:.8125rem;font-weight:600;margin-bottom:8px;color:#475569}"
  "input{width:100%;padding:14px 16px;font-size:1.0625rem;font-family:ui-monospace,monospace;"
  "border:2px solid #e2e8f0;border-radius:10px;background:#f8fafc;color:#0f172a;"
  "text-transform:lowercase;letter-spacing:.03em}"
  "input:focus{outline:none;border-color:#2563eb;background:#fff;"
  "box-shadow:0 0 0 4px rgba(37,99,235,.12)}"
  "button{width:100%;margin-top:20px;padding:14px;font-size:.9375rem;font-weight:600;"
  "font-family:inherit;border:none;border-radius:10px;cursor:pointer;"
  "background:linear-gradient(135deg,#2563eb,#1d4ed8);color:#fff;"
  "box-shadow:0 4px 14px rgba(37,99,235,.35);transition:transform .15s}"
  "button:active{transform:scale(.98)}"
  ".foot{margin-top:20px;text-align:center;font-size:.75rem;color:#64748b}"
  ".foot span{color:#94a3b8}"
  ".link-reset{display:block;text-align:center;margin-top:14px;font-size:.8125rem;"
  "color:#94a3b8;text-decoration:none}"
  ".link-reset:hover{color:#cbd5e1}"
  ".msg-page{display:flex;align-items:center;justify-content:center;min-height:100vh;padding:24px}"
  ".msg-box{text-align:center;background:#fff;border-radius:16px;padding:40px 32px;"
  "max-width:360px;box-shadow:0 20px 50px rgba(0,0,0,.3)}"
  ".msg-box h2{font-size:1.25rem;color:#0f172a;margin-bottom:8px}"
  ".msg-box p{color:#64748b;font-size:.9375rem}"
  ".spinner{width:32px;height:32px;border:3px solid #e2e8f0;border-top-color:#2563eb;"
  "border-radius:50%;margin:16px auto 0;animation:spin .8s linear infinite}"
  "@keyframes spin{to{transform:rotate(360deg)}}";

void sendHtmlPage(const String& body) {
  configServer.sendHeader("Cache-Control", "no-cache");
  configServer.send(200, "text/html; charset=iso-8859-1", body);
}

String htmlHead() {
  String h = F("<!DOCTYPE html><html lang='pt-BR'><head>"
               "<meta charset='iso-8859-1'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>Gateway MQTT</title><style>");
  h += FPSTR(PAGE_STYLE);
  h += F("</style></head>");
  return h;
}

void handleConfigPage() {
  const bool wifiOk = (WiFi.status() == WL_CONNECTED);
  const bool mqttOk = client.connected();

  String html = htmlHead();
  html += F("<body><div class='wrap'>"
            "<div class='brand'><div class='logo'>G</div>"
            "<div><h1>Gateway MQTT</h1>"
            "<p>LAV60 &middot; Painel de configura&ccedil;&atilde;o</p></div></div>");

  html += F("<div class='store-card'>"
            "<div class='store-label'>Loja configurada</div>"
            "<div class='store-name'>");
  html += cfg.storeName;
  html += F("</div><div class='store-topic'>");
  html += cfg.storeName;
  html += F("/#</div><div class='status-row'>"
            "<span class='pill'><span class='dot ");
  html += wifiOk ? F("on") : F("");
  html += F("'></span> Wi-Fi</span>"
            "<span class='pill'><span class='dot ");
  html += mqttOk ? F("on") : F("");
  html += F("'></span> MQTT</span></div></div>");

  html += F("<div class='panel'><h2>Alterar loja</h2>"
            "<p class='hint'>Identificador usado nos t&oacute;picos MQTT desta unidade.</p>"
            "<form method='POST' action='/save'>"
            "<label for='store'>C&oacute;digo da loja</label>"
            "<input id='store' name='store' value='");
  html += cfg.storeName;
  html += F("' maxlength='15' pattern='[a-zA-Z0-9_-]+' "
            "placeholder='ex: pb05' required autocapitalize='off' spellcheck='false'>"
            "<button type='submit'>Salvar configura&ccedil;&atilde;o</button>"
            "</form>"
            "<a class='link-reset' href='/reset' "
            "onclick=\"return confirm('Restaurar loja padrao pb05?')\">"
            "Restaurar padr&atilde;o</a></div>");

  html += F("<div class='foot'><span>IP </span>");
  html += WiFi.localIP().toString();
  html += F(" &middot; <span>FW </span>");
  html += FIRMWARE_VERSION;
  html += F("</div></div></body></html>");

  sendHtmlPage(html);
}

void handleConfigSave() {
  if (configServer.hasArg("store")) {
    cfgCopy(cfg.storeName, sizeof(cfg.storeName), configServer.arg("store").c_str());
  }
  saveConfig();
  String html = htmlHead();
  html += F("<body><div class='msg-page'><div class='msg-box'>"
            "<h2>Configura&ccedil;&atilde;o salva!</h2>"
            "<p>Loja <strong>");
  html += cfg.storeName;
  html += F("</strong> gravada na EEPROM.</p>"
            "<div class='spinner'></div>"
            "<p style='margin-top:12px;font-size:.8125rem'>Reiniciando...</p>"
            "</div></div></body></html>");
  sendHtmlPage(html);
  delay(1500);
  ESP.restart();
}

void handleConfigReset() {
  resetConfig();
  String html = htmlHead();
  html += F("<body><div class='msg-page'><div class='msg-box'>"
            "<h2>Padr&atilde;o restaurado</h2>"
            "<p>Loja resetada para <strong>pb05</strong>.</p>"
            "<div class='spinner'></div>"
            "<p style='margin-top:12px;font-size:.8125rem'>Reiniciando...</p>"
            "</div></div></body></html>");
  sendHtmlPage(html);
  delay(1500);
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
