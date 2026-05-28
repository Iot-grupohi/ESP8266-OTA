#pragma once

// ============================================================
//  Configurações do projeto — edite antes de compilar
// ============================================================

// --- Wi-Fi ---
#define WIFI_SSID       "FN2014"
#define WIFI_PASSWORD   "@1L2S3C@"

// --- Repositório GitHub ---
//   O firmware.bin e version.txt devem ser assets do release
//   Exemplo: https://github.com/joao/ESP8266-OTA/releases/latest/download/firmware.bin
#define GITHUB_USER     "Iot-grupohi"
#define GITHUB_REPO     "ESP8266-OTA"

// --- Intervalo de verificação de atualização ---
//   Padrão: 30 minutos (em milissegundos)
#define OTA_CHECK_INTERVAL_MS  (30UL * 60UL * 1000UL)
