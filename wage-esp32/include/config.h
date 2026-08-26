#pragma once

#include <Arduino.h>
#include <IPAddress.h>

// =========================================================
// Debug / Logging
// =========================================================
static constexpr bool MASTER_DEBUG_LOG = false;   // Haupt-Debug-Log ein/aus (Serial)
static constexpr bool PERFORMANCE_DEBUG = false;  // Timing-Messungen ausgeben

// =========================================================
// WLAN / Webinterface
// =========================================================
static constexpr bool WEB_CONFIG_ENABLED = true;  // ESP32 Webinterface aktiv
static constexpr bool WIFI_STA_ENABLED = false;   // Haus-WLAN Client (normalerweise über Pi gesteuert)
static const char* WIFI_SSID = "YOUR_WIFI_SSID";
static const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 12000; // Timeout für WLAN-Verbindung
static constexpr bool WIFI_USE_STATIC_IP = true;  // Statische IP statt DHCP
static const IPAddress WIFI_LOCAL_IP(192, 168, 178, 60);
static const IPAddress WIFI_GATEWAY(192, 168, 178, 1);
static const IPAddress WIFI_SUBNET(255, 255, 255, 0);
static const IPAddress WIFI_DNS1(192, 168, 178, 1);
static const IPAddress WIFI_DNS2(8, 8, 8, 8);
static const char* CONFIG_AP_SSID = "Waage-Config";
static const char* CONFIG_AP_PASSWORD = "waagecfg1";
static constexpr uint16_t WEB_SERVER_PORT = 80;   // HTTP-Port des Webinterfaces
static constexpr uint32_t HEARTBEAT_INTERVAL_MS    = 30000;  // Heartbeat alle 30s
static constexpr uint32_t OFFLINE_PROBE_INTERVAL_MS = 60000;  // Probe alle 60s wenn Pi offline
static constexpr uint32_t WEB_SERVICE_INTERVAL_IDLE_MS = 500;  // Webserver-Poll im Leerlauf
static constexpr uint32_t WEB_SERVICE_INTERVAL_BUSY_MS = 1000; // Webserver-Poll während Messung

// =========================================================
// Externe Schnittstelle
// =========================================================
static constexpr bool EXTERNAL_INTERFACE_ENABLED_DEFAULT = false; // Pi-Kommunikation aktiv
static const char* EXTERNAL_TARGET_HOST_DEFAULT = "";
static constexpr uint16_t EXTERNAL_TARGET_PORT_DEFAULT = 80;
static const char* EXTERNAL_API_PATH_DEFAULT = "/api/v1/runs";
static const char* EXTERNAL_API_KEY_DEFAULT = "";
static constexpr uint32_t EXTERNAL_SEND_TIMEOUT_MS = 2000;
static constexpr uint32_t EXTERNAL_RETRY_INTERVAL_MS = 5000;
static constexpr size_t EXTERNAL_QUEUE_MAX = 50;  // Max. gepufferte Läufe wenn Pi offline

// =========================================================
// OLED
// =========================================================
static const char* OLED_SCALE_CONFIG = "1,5";
static constexpr uint32_t DEFAULT_OLED_TIMING_REFRESH_MS = 200;
static constexpr uint32_t OLED_DEBUG_REFRESH_MS = 120;
static constexpr uint32_t OLED_PATTERN_REFRESH_MS = 50;
static constexpr uint32_t SERIAL_BASE_REFRESH_MS = 150;
static constexpr uint32_t SERIAL_STATE_REFRESH_MS = 150;
static constexpr uint32_t DEFAULT_OLED_I2C_CLOCK_HZ = 1000000;
static constexpr uint8_t DEFAULT_OLED_ROTATION = 0;
static constexpr uint8_t I2C_SDA = 21;
static constexpr uint8_t I2C_SCL = 22;
static constexpr uint8_t OLED_ADDR = 0x3C;
static constexpr int SCREEN_W = 128;
static constexpr int SCREEN_H = 64;

// =========================================================
// Ring 1 / Haupt-LED-Ring
// =========================================================
// GPIO5: ESP32 nutzbarer Output, aber Strapping-Pin. Externe Beschaltung darf Boot nicht stören.
static constexpr uint8_t LED_STRIP_PIN = 5;
static constexpr uint16_t PIXEL_COUNT = 160;
static constexpr uint8_t PIXEL_GROUP_SIZE = 3;                        // je 3 physische Pixel = 1 logische Gruppe
static constexpr uint16_t PIXEL_GROUPS = PIXEL_COUNT / PIXEL_GROUP_SIZE; // 53 Gruppen bei 160 Pixeln
static constexpr uint8_t DEFAULT_PIXEL_BRIGHTNESS_PERCENT = 50; // Ring1 Helligkeit in %
static constexpr uint8_t DEFAULT_STANDBY_BRIGHTNESS_PERCENT = 90; // Ring1 Helligkeit im Standby in %
static constexpr bool DEFAULT_PIXEL_DEBUG_ALL_ON = false; // Debug: alle Pixel einschalten

// =========================================================
// Ring 2 / Zusatz-LED-Ring
// =========================================================
static constexpr bool RING2_ENABLED = true;
// GPIO14: ESP32 nutzbarer Output, teilt sich aber JTAG-Funktion. Bei JTAG-Nutzung Konflikt möglich.
static constexpr uint8_t RING2_PIN = 14;
static constexpr uint16_t RING2_PIXEL_COUNT = 24;
static constexpr bool RING2_BOOT_TEST = false;             // Ring2 Boot-Selbsttest
static constexpr bool RING2_FORCE_INDEPENDENT_TEST = false; // Ring2 immer unabhängig testen
static constexpr bool DEFAULT_RING2_ENABLED = true;
static constexpr uint8_t DEFAULT_RING2_BRIGHTNESS_PERCENT = 35;
static constexpr uint8_t DEFAULT_RING2_STANDBY_BRIGHTNESS_PERCENT = 25;
static constexpr bool DEFAULT_RING2_DEBUG_ALL_ON = false;

// =========================================================
// HX711 / Waagen-Pins
// =========================================================
static constexpr uint8_t HX1_DOUT = 32;
static constexpr uint8_t HX1_SCK = 33;
static constexpr uint8_t HX2_DOUT = 25;
static constexpr uint8_t HX2_SCK = 26;

// =========================================================
// Kalibrierung
// =========================================================
static constexpr float DEFAULT_CAL1 = -235.15f;
static constexpr float DEFAULT_CAL2 = -235.15f;
static constexpr bool INVERT1 = true;
static constexpr bool INVERT2 = true;

// =========================================================
// Messlogik / Schwellwerte
// =========================================================
static constexpr float DEFAULT_OBJECT_PRESENT_G = 100.0f;
static constexpr float DEFAULT_START_DROP_PERCENT = 5.0f;
static constexpr float DEFAULT_STOP_RISE_PERCENT = 5.0f;
static constexpr float MIN_DYNAMIC_THRESHOLD_G = 2.0f;
static constexpr uint32_t DROP_HOLD_MS = 150;
static constexpr uint32_t STOP_HOLD_MS = 220;
static constexpr float STOP_RESET_HYST_G = 0.8f;
static constexpr float START_RESET_HYST_G = 1.2f;
static constexpr uint32_t READY_AFTER_DETECT_MS = 1200;
static constexpr uint32_t SHOW_RESULT_MS = 6000;
static constexpr float DEFAULT_EMPTY_THRESHOLD_G = 10.0f;
static constexpr float DEFAULT_RETARE_TOL_G = 0.6f;

// =========================================================
// Timing / State Machine
// =========================================================
static constexpr uint32_t BOOT_MSG_MS = 1200;
static constexpr uint16_t TARE_SAMPLES = 25;
static constexpr uint32_t DEFAULT_SCALE_READ_INTERVAL_MS = 40;
static constexpr uint8_t DEFAULT_SCALE_READ_SAMPLES = 1;

// =========================================================
// Standby
// =========================================================
static constexpr uint32_t STANDBY_AFTER_MS = 25000;
static constexpr float DEFAULT_STANDBY_WAKE_THRESHOLD_G = 3.0f;
static constexpr uint32_t STANDBY_FRAME_MS = 120;
static constexpr uint32_t STANDBY_CHANGE_MIN_MS = 500;
static constexpr uint32_t STANDBY_CHANGE_MAX_MS = 1200;
static constexpr uint8_t STANDBY_SATURATION = 180;
static constexpr uint8_t STANDBY_VALUE_MIN = 52;
static constexpr uint8_t STANDBY_VALUE_MAX = 112;
static constexpr uint8_t STANDBY_ON_MIN = 2;
static constexpr uint8_t STANDBY_ON_MAX = 9;

// =========================================================
// Filter / Stabilität
// =========================================================
static constexpr uint8_t MA_N = 10;        // Moving-Average Fenstergröße
static constexpr float STABLE_BAND_G = 2.0f;     // Toleranzband für Stabilitätserkennung (g)
static constexpr uint32_t STABLE_WINDOW_MS = 800;
static constexpr uint32_t STABLE_HOLD_MS = 800;

// =========================================================
// Fehler / Recovery
// =========================================================
static constexpr float NEGATIVE_CLAMP_G = -3.0f;  // Negativwerte bis hier toleriert
static constexpr float NEGATIVE_ERROR_G = -15.0f; // Unterhalb: Fehler-Recovery auslösen
static constexpr uint32_t RECOVER_WAIT_MS = 1200; // Wartezeit nach Fehler-Recovery
