#pragma once

// --- Wi-Fi ---
#define WIFI_SSID       "FN2014"
#define WIFI_PASSWORD   "@1L2S3C@"

// --- Repositório GitHub (releases OTA) ---
#define GITHUB_USER     "Iot-grupohi"
#define GITHUB_REPO     "ESP8266-OTA"

// --- OTA ---
#define OTA_CHECK_INTERVAL_MS    (30UL * 60UL * 1000UL)
#define OTA_HTTP_TIMEOUT_MS      30000
#define OTA_DOWNLOAD_TIMEOUT_MS  30000
#define OTA_CHUNK_SIZE           1024
#define OTA_PROGRESS_BYTES       32768

// --- LED de status (ms) ---
#define LED_BLINK_CONNECTED_MS   100   // Wi-Fi + MQTT OK
#define LED_BLINK_WIFI_ONLY_MS   800   // Wi-Fi OK, sem MQTT
#define LED_BLINK_NO_WIFI_MS     200   // sem Wi-Fi
