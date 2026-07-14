#include "ota_module.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <cstring>
#include <cstdio>

#include "display_module.h"

namespace {

OtaTargetConfig targetCfg{};
char currentVersion[32] = "unknown";
char availableVersion[32] = "";
char statusText[128] = "Kein Update geprüft";
OtaState state = OtaState::IDLE;
uint8_t downloadProgressPercent = 0;

void setStatus(const char* text) {
  strncpy(statusText, text, sizeof(statusText) - 1);
  statusText[sizeof(statusText) - 1] = '\0';
  oledMsg2("Firmware-Update", text);
}

bool hasValidTarget() {
  return targetCfg.host[0] != '\0';
}

String buildUrl(const char* path) {
  String url = String("http://") + targetCfg.host + ":" + String(targetCfg.port) + path;
  return url;
}

// Sehr einfache Versions-Vergleichsfunktion: die Builds heißen
// "beta-<kurzer-git-sha>", ein echter Semver-Vergleich ist daher nicht
// sinnvoll möglich. Wir behandeln jede abweichende Version als "neu" -
// Downgrade-Schutz ist dadurch bewusst NICHT gegeben (siehe Konzept,
// Downgrade kann später bei Bedarf separat ergänzt werden).
bool versionDiffers(const char* a, const char* b) {
  return strcmp(a, b) != 0;
}

// Sehr simples JSON-Wert-Extraktion ohne externe Lib, ausreichend für das
// kleine, kontrollierte manifest.json (kein Escaping/Nesting zu erwarten).
bool extractJsonString(const String& json, const char* key, char* out, size_t outSize) {
  String pattern = String("\"") + key + "\":\"";
  int start = json.indexOf(pattern);
  if (start < 0) return false;
  start += pattern.length();
  int end = json.indexOf('"', start);
  if (end < 0) return false;
  String value = json.substring(start, end);
  strncpy(out, value.c_str(), outSize - 1);
  out[outSize - 1] = '\0';
  return true;
}

}  // namespace

void otaInit(const char* currentFirmwareVersion) {
  strncpy(currentVersion, currentFirmwareVersion, sizeof(currentVersion) - 1);
  currentVersion[sizeof(currentVersion) - 1] = '\0';
  state = OtaState::IDLE;
  setStatus("Kein Update geprüft");
}

void otaSetTarget(const char* host, uint16_t port) {
  strncpy(targetCfg.host, host, sizeof(targetCfg.host) - 1);
  targetCfg.host[sizeof(targetCfg.host) - 1] = '\0';
  targetCfg.port = port;
}

bool otaCheckForUpdate() {
  if (!hasValidTarget()) {
    state = OtaState::ERROR;
    setStatus("Kein Pi-Ziel konfiguriert");
    return false;
  }

  state = OtaState::CHECKING;
  setStatus("Prüfe auf neue Firmware...");

  HTTPClient http;
  String url = buildUrl("/api/v1/esp-firmware/manifest");
  if (!http.begin(url)) {
    state = OtaState::ERROR;
    setStatus("Verbindung zum Pi fehlgeschlagen");
    return false;
  }
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) {
    char buf[96];
    snprintf(buf, sizeof(buf), "Manifest-Abruf fehlgeschlagen (HTTP %d)", code);
    setStatus(buf);
    state = OtaState::ERROR;
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  char version[32] = "";
  if (!extractJsonString(body, "version", version, sizeof(version))) {
    state = OtaState::ERROR;
    setStatus("Manifest ungültig (keine Version gefunden)");
    return false;
  }

  strncpy(availableVersion, version, sizeof(availableVersion) - 1);
  availableVersion[sizeof(availableVersion) - 1] = '\0';

  if (versionDiffers(currentVersion, availableVersion)) {
    state = OtaState::UPDATE_AVAILABLE;
    char buf[128];
    snprintf(buf, sizeof(buf), "Update verfügbar: %s -> %s", currentVersion, availableVersion);
    setStatus(buf);
  } else {
    state = OtaState::UP_TO_DATE;
    setStatus("Firmware ist aktuell");
  }
  return true;
}

bool otaPerformUpdate() {
  if (state != OtaState::UPDATE_AVAILABLE) {
    setStatus("Kein verfügbares Update zum Installieren");
    return false;
  }
  if (!hasValidTarget()) {
    state = OtaState::ERROR;
    setStatus("Kein Pi-Ziel konfiguriert");
    return false;
  }

  // Pi anweisen, die Firmware frisch von GitHub Releases in seinen Cache
  // zu laden, bevor wir sie selbst abrufen.
  state = OtaState::DOWNLOADING;
  setStatus("Pi lädt Firmware von GitHub...");
  downloadProgressPercent = 0;
  {
    HTTPClient syncHttp;
    String syncUrl = buildUrl("/api/v1/esp-firmware/sync");
    if (!syncHttp.begin(syncUrl)) {
      state = OtaState::ERROR;
      setStatus("Sync-Anfrage an Pi fehlgeschlagen");
      return false;
    }
    syncHttp.setTimeout(60000);  // Pi->GitHub Download kann etwas dauern
    int syncCode = syncHttp.POST("");
    syncHttp.end();
    if (syncCode != 200) {
      char buf[96];
      snprintf(buf, sizeof(buf), "Pi-Sync fehlgeschlagen (HTTP %d)", syncCode);
      setStatus(buf);
      state = OtaState::ERROR;
      return false;
    }
  }

  setStatus("Lade Firmware vom Pi...");
  HTTPClient http;
  String url = buildUrl("/api/v1/esp-firmware/latest.bin");
  if (!http.begin(url)) {
    state = OtaState::ERROR;
    setStatus("Verbindung zum Pi fehlgeschlagen");
    return false;
  }
  http.setTimeout(30000);
  http.useHTTP10(true);
  int code = http.GET();
  if (code != 200) {
    char buf[96];
    snprintf(buf, sizeof(buf), "Firmware-Download fehlgeschlagen (HTTP %d)", code);
    setStatus(buf);
    state = OtaState::ERROR;
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    state = OtaState::ERROR;
    setStatus("Ungültige Firmware-Größe vom Pi erhalten");
    http.end();
    return false;
  }

  if (!Update.begin(contentLength)) {
    state = OtaState::ERROR;
    setStatus("Nicht genug Platz für Update-Partition");
    http.end();
    return false;
  }

  state = OtaState::FLASHING;
  setStatus("Flashe neue Firmware...");

  WiFiClient* stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buf[1024];
  while (http.connected() && written < (size_t)contentLength) {
    size_t avail = stream->available();
    if (avail == 0) {
      delay(5);
      continue;
    }
    size_t toRead = avail > sizeof(buf) ? sizeof(buf) : avail;
    size_t readBytes = stream->readBytes(buf, toRead);
    if (readBytes == 0) break;
    size_t wroteNow = Update.write(buf, readBytes);
    if (wroteNow != readBytes) {
      state = OtaState::ERROR;
      setStatus("Schreibfehler während des Flashens");
      http.end();
      Update.abort();
      return false;
    }
    written += wroteNow;
    downloadProgressPercent = (uint8_t)((written * 100UL) / (size_t)contentLength);
    // OLED alle ~5% aktualisieren um I2C nicht zu überlasten
    static uint8_t lastOledPercent = 255;
    if (downloadProgressPercent / 5 != lastOledPercent / 5) {
      lastOledPercent = downloadProgressPercent;
      char buf[20];
      snprintf(buf, sizeof(buf), "Flashe: %u%%", (unsigned)downloadProgressPercent);
      oledMsg2("Firmware-Update", buf);
    }
  }
  http.end();

  if (written != (size_t)contentLength) {
    state = OtaState::ERROR;
    setStatus("Download unvollständig");
    Update.abort();
    return false;
  }

  if (!Update.end(true)) {
    char buf[96];
    snprintf(buf, sizeof(buf), "Update.end() fehlgeschlagen: %s", Update.errorString());
    setStatus(buf);
    state = OtaState::ERROR;
    return false;
  }

  state = OtaState::SUCCESS_PENDING_REBOOT;
  setStatus("Update erfolgreich, starte neu...");
  delay(500);
  ESP.restart();
  return true;  // wird wegen restart() nicht mehr erreicht
}

OtaState otaGetState() { return state; }
const char* otaGetStatusText() { return statusText; }
const char* otaGetAvailableVersion() { return availableVersion; }
uint8_t otaGetDownloadProgressPercent() { return downloadProgressPercent; }

bool otaIsBusy() {
  return state == OtaState::CHECKING || state == OtaState::DOWNLOADING || state == OtaState::FLASHING;
}
