#include "ota_module.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include <cstring>
#include <cstdio>

#include "display_module.h"

namespace {

OtaTargetConfig targetCfg{};
char currentVersion[32] = "unknown";
char availableVersion[32] = "";
char availableSha256[65] = "";  // 64 Hex-Zeichen aus dem Manifest
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

// Sehr simple JSON-Wert-Extraktion ohne externe Lib, ausreichend für das
// kleine, kontrollierte manifest.json (kein Escaping/Nesting zu erwarten).
// Leerzeichen zwischen Schlüssel, ':' und Wert sind erlaubt.
bool extractJsonString(const String& json, const char* key, char* out, size_t outSize) {
  String pattern = String("\"") + key + "\"";
  int pos = json.indexOf(pattern);
  if (pos < 0) return false;
  pos += pattern.length();
  while (pos < (int)json.length() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) ++pos;
  if (pos >= (int)json.length() || json[pos] != ':') return false;
  ++pos;
  while (pos < (int)json.length() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) ++pos;
  if (pos >= (int)json.length() || json[pos] != '"') return false;
  ++pos;
  int end = json.indexOf('"', pos);
  if (end < 0) return false;
  String value = json.substring(pos, end);
  strncpy(out, value.c_str(), outSize - 1);
  out[outSize - 1] = '\0';
  return true;
}

// Holt manifest.json vom Pi und liest Version und SHA256 aus.
// Rückgabe: 0 = ok, 1 = Verbindung/HTTP-Fehler, 2 = Manifest ungültig.
int fetchManifest(char* version, size_t versionSize, char* sha256, size_t shaSize, int* httpCode) {
  HTTPClient http;
  String url = buildUrl("/api/v1/esp-firmware/manifest");
  if (!http.begin(url)) { *httpCode = -1; return 1; }
  http.setTimeout(8000);
  *httpCode = http.GET();
  if (*httpCode != 200) { http.end(); return 1; }
  String body = http.getString();
  http.end();
  if (!extractJsonString(body, "version", version, versionSize)) return 2;
  sha256[0] = '\0';
  extractJsonString(body, "sha256", sha256, shaSize);  // fehlt sie, bleibt der Wert leer
  return 0;
}

// Gleichheit zweier Hex-Strings ohne Beachtung der Groß-/Kleinschreibung.
bool hexEqualsIgnoreCase(const char* a, const char* b) {
  if (strlen(a) != strlen(b)) return false;
  for (size_t i = 0; a[i]; ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'F') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'F') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
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

  char version[32] = "";
  char sha[65] = "";
  int httpCode = 0;
  const int mr = fetchManifest(version, sizeof(version), sha, sizeof(sha), &httpCode);
  if (mr == 1) {
    state = OtaState::ERROR;
    if (httpCode <= 0) {
      setStatus("Verbindung zum Pi fehlgeschlagen");
    } else {
      char buf[96];
      snprintf(buf, sizeof(buf), "Manifest-Abruf fehlgeschlagen (HTTP %d)", httpCode);
      setStatus(buf);
    }
    return false;
  }
  if (mr == 2) {
    state = OtaState::ERROR;
    setStatus("Manifest ungültig (keine Version gefunden)");
    return false;
  }

  strncpy(availableVersion, version, sizeof(availableVersion) - 1);
  availableVersion[sizeof(availableVersion) - 1] = '\0';
  strncpy(availableSha256, sha, sizeof(availableSha256) - 1);
  availableSha256[sizeof(availableSha256) - 1] = '\0';

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

  // Der Sync kann eine neuere Firmware in den Cache geladen haben als beim Check.
  // Prüfsumme deshalb jetzt frisch aus dem Manifest holen. Ohne Prüfsumme wird
  // nicht geflasht.
  {
    char version[32] = "";
    char sha[65] = "";
    int httpCode = 0;
    if (fetchManifest(version, sizeof(version), sha, sizeof(sha), &httpCode) != 0) {
      state = OtaState::ERROR;
      setStatus("Manifest nach dem Sync nicht lesbar");
      return false;
    }
    if (strlen(sha) != 64) {
      state = OtaState::ERROR;
      setStatus("Manifest ohne gültige Prüfsumme - Update abgebrochen");
      return false;
    }
    strncpy(availableVersion, version, sizeof(availableVersion) - 1);
    availableVersion[sizeof(availableVersion) - 1] = '\0';
    strncpy(availableSha256, sha, sizeof(availableSha256) - 1);
    availableSha256[sizeof(availableSha256) - 1] = '\0';
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
  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts_ret(&shaCtx, 0);  // 0 = SHA-256 (nicht SHA-224)
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
      mbedtls_sha256_free(&shaCtx);
      return false;
    }
    mbedtls_sha256_update_ret(&shaCtx, buf, readBytes);
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

  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&shaCtx, digest);
  mbedtls_sha256_free(&shaCtx);

  if (written != (size_t)contentLength) {
    state = OtaState::ERROR;
    setStatus("Download unvollständig");
    Update.abort();
    return false;
  }

  // Prüfsumme vergleichen, BEVOR das Update aktiviert wird.
  char digestHex[65];
  for (int i = 0; i < 32; ++i) snprintf(digestHex + 2 * i, 3, "%02x", digest[i]);
  if (!hexEqualsIgnoreCase(digestHex, availableSha256)) {
    state = OtaState::ERROR;
    setStatus("Prüfsumme stimmt nicht - Update verworfen");
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
