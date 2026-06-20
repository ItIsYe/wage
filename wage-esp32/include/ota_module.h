#pragma once

#include <Arduino.h>

// OTA-Update-Modul: prüft beim Pi ob eine neue Firmware verfügbar ist und
// flasht sich ggf. selbst neu. Wird ausschließlich manuell über das
// Webinterface ausgelöst, nie automatisch, und nie während eines aktiven
// Wiege-Vorgangs (siehe otaIsBusy() / Guard im aufrufenden Code).

struct OtaTargetConfig {
  char host[64];
  uint16_t port;
};

enum class OtaState : uint8_t {
  IDLE = 0,
  CHECKING,
  UP_TO_DATE,
  UPDATE_AVAILABLE,
  DOWNLOADING,
  FLASHING,
  SUCCESS_PENDING_REBOOT,
  ERROR,
};

void otaInit(const char* currentFirmwareVersion);
void otaSetTarget(const char* host, uint16_t port);

// Nicht-blockierender Status-Check (HTTP GET manifest.json über den Pi).
// Setzt otaState auf UPDATE_AVAILABLE oder UP_TO_DATE.
bool otaCheckForUpdate();

// Blockierender Download+Flash-Vorgang. Nur aufrufen wenn otaCheckForUpdate()
// vorher UPDATE_AVAILABLE ergeben hat und der Aufrufer sichergestellt hat,
// dass kein aktiver Wiege-Vorgang läuft. Reboot bei Erfolg automatisch.
bool otaPerformUpdate();

OtaState otaGetState();
const char* otaGetStatusText();
const char* otaGetAvailableVersion();
uint8_t otaGetDownloadProgressPercent();
bool otaIsBusy();
