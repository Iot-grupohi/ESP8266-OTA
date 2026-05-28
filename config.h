#pragma once

// ============================================================
//  Configurações do projeto — edite antes de compilar
// ============================================================

// --- Wi-Fi ---
#define WIFI_SSID       "FN2014"
#define WIFI_PASSWORD   "@1L2S3C@"

// --- Repositório GitHub ---
#define GITHUB_USER     "Iot-grupohi"
#define GITHUB_REPO     "ESP8266-OTA"

// --- OTA ---
#define OTA_CHECK_INTERVAL_MS   (30UL * 60UL * 1000UL)  // 30 min
#define OTA_HTTP_TIMEOUT_MS     30000
#define OTA_DOWNLOAD_TIMEOUT_MS 30000
#define OTA_CHUNK_SIZE          1024
#define OTA_PROGRESS_BYTES      32768

// --- LED (active low) ---
#define LED_BLINK_INTERVAL_MS   5000
