#pragma once

#include <EEPROM.h>
#include <ESP8266WiFi.h>

#define CFG_MAGIC     0x4C415633UL  // "LAV3"
#define CFG_EEPROM_SZ 512
#define CFG_MACHINES  4

struct StoreConfig {
  uint32_t magic;
  char storeName[16];
  char staticIp[16];
  char gateway[16];
  char subnet[16];
  char mqttServer[32];
  char mqttUser[24];
  char mqttPass[24];
  uint16_t mqttPort;
  char ipWash[CFG_MACHINES][16];
  char idWash[CFG_MACHINES][8];
  char ipDry[CFG_MACHINES][16];
  char idDry[CFG_MACHINES][8];
  char ipAc[16];
  char ipDos[CFG_MACHINES][16];
  char idDos[CFG_MACHINES][8];
};

StoreConfig cfg;

static void cfgCopy(char* dst, size_t len, const char* src) {
  strncpy(dst, src, len - 1);
  dst[len - 1] = '\0';
}

void loadConfigDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = CFG_MAGIC;
  cfgCopy(cfg.storeName, sizeof(cfg.storeName), "pb05");
  cfgCopy(cfg.staticIp, sizeof(cfg.staticIp), "192.168.50.180");
  cfgCopy(cfg.gateway, sizeof(cfg.gateway), "192.168.50.1");
  cfgCopy(cfg.subnet, sizeof(cfg.subnet), "255.255.255.0");
  cfgCopy(cfg.mqttServer, sizeof(cfg.mqttServer), "161.97.172.86");
  cfgCopy(cfg.mqttUser, sizeof(cfg.mqttUser), "lav60");
  cfgCopy(cfg.mqttPass, sizeof(cfg.mqttPass), "lav60");
  cfg.mqttPort = 1883;

  const char* defWashIp[] = { "192.168.50.100", "192.168.50.101", "192.168.50.102", "192.168.50.103" };
  const char* defWashId[] = { "321", "432", "543", "654" };
  const char* defDryIp[]  = { "192.168.50.104", "192.168.50.105", "192.168.50.106", "192.168.50.107" };
  const char* defDryId[]  = { "765", "876", "987", "210" };
  const char* defDosIp[]  = { "192.168.50.150", "192.168.50.151", "192.168.50.152", "192.168.50.153" };
  const char* defDosId[]  = { "321", "432", "543", "654" };

  for (int i = 0; i < CFG_MACHINES; i++) {
    cfgCopy(cfg.ipWash[i], sizeof(cfg.ipWash[i]), defWashIp[i]);
    cfgCopy(cfg.idWash[i], sizeof(cfg.idWash[i]), defWashId[i]);
    cfgCopy(cfg.ipDry[i], sizeof(cfg.ipDry[i]), defDryIp[i]);
    cfgCopy(cfg.idDry[i], sizeof(cfg.idDry[i]), defDryId[i]);
    cfgCopy(cfg.ipDos[i], sizeof(cfg.ipDos[i]), defDosIp[i]);
    cfgCopy(cfg.idDos[i], sizeof(cfg.idDos[i]), defDosId[i]);
  }
  cfgCopy(cfg.ipAc, sizeof(cfg.ipAc), "192.168.50.110");
}

void saveConfig() {
  cfg.magic = CFG_MAGIC;
  EEPROM.begin(CFG_EEPROM_SZ);
  EEPROM.put(0, cfg);
  EEPROM.commit();
  EEPROM.end();
}

void loadConfig() {
  EEPROM.begin(CFG_EEPROM_SZ);
  EEPROM.get(0, cfg);
  EEPROM.end();

  if (cfg.magic != CFG_MAGIC || cfg.storeName[0] == '\0') {
    Serial.println("[CFG] EEPROM vazia ou inválida — usando padrões");
    loadConfigDefaults();
    saveConfig();
  } else {
    Serial.println("[CFG] Configuração carregada da EEPROM");
  }
}

void resetConfig() {
  loadConfigDefaults();
  saveConfig();
}

bool connectWiFiStatic() {
  IPAddress ip, gw, sn, dns;
  if (!ip.fromString(cfg.staticIp) || !gw.fromString(cfg.gateway) || !sn.fromString(cfg.subnet)) {
    Serial.println("[WiFi] IP inválido na config");
    return false;
  }
  dns = gw;

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.config(ip, gw, sn, dns);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Conectando ao WiFi (%s)...", cfg.staticIp);
  uint8_t tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 60) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi OK! IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("\nFalha ao conectar WiFi");
  return false;
}
