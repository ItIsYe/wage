#include "external_interface_module.h"

#include <cstdio>
#include <HTTPClient.h>
#include <Preferences.h>
#include <cstring>

#include "config.h"

namespace {
struct ExternalTransportConfig {
  bool enabled;
  char host[64];
  uint16_t port;
  char apiPath[64];
  char apiKey[64];
};

RunDataSnapshot queueBuf[EXTERNAL_QUEUE_MAX];
size_t queueHead = 0;
size_t queueCount = 0;
uint32_t lastSendAttemptMs = 0;
char lastStatus[96] = "idle";
char firmwareVersion[32] = "unknown";
char deviceId[64] = "";
ExternalTransportConfig transportCfg{};
bool lastSendError = false;
uint32_t sendErrorEventCounter = 0;
uint32_t lastHeartbeatMs = 0;
uint32_t currentBootId = 0;
static uint8_t consecutiveFailures = 0;
static bool piOffline = false;
static constexpr uint8_t OFFLINE_THRESHOLD = 3;       // nach 3 Verbindungsfehlern -> offline
// Ohne Timeout blockiert connect() bis zu 5 s die Hauptschleife (Waage reagiert nicht, LEDs stehen).
static constexpr uint16_t HTTP_CONNECT_TIMEOUT_MS = 1500;
static constexpr uint16_t HTTP_CONNECT_TIMEOUT_QUIET_MS = 300;  // Heartbeat bei bereitstehendem Glas

// Queue im Flash sichern, damit Laeufe einen Neustart oder ein OTA-Update ueberleben.
// Gesichert werden die neuesten QUEUE_PERSIST_MAX Laeufe.
static constexpr size_t QUEUE_PERSIST_MAX = 20;
static constexpr uint32_t QUEUE_BLOB_MAGIC = 0x57514532;  // "WQE2"
struct QueueBlobHeader { uint32_t magic; uint32_t runSize; uint32_t count; };
static constexpr size_t QUEUE_BLOB_MAX = sizeof(QueueBlobHeader) + QUEUE_PERSIST_MAX * sizeof(RunDataSnapshot);

bool hasValidTarget() {
  return transportCfg.enabled && transportCfg.host[0] != '\0' && transportCfg.apiPath[0] != '\0';
}

void setStatus(const char* text) {
  strncpy(lastStatus, text, sizeof(lastStatus) - 1);
  lastStatus[sizeof(lastStatus) - 1] = '\0';
}

// Verbindungsebene: Pi nicht erreichbar (Timeout/Connect-Fehler). Zaehlt fuer den Offline-Modus,
// egal ob der Fehler beim Senden eines Laufs oder beim Heartbeat auftrat.
void noteLinkFailure() {
  if (consecutiveFailures < OFFLINE_THRESHOLD) consecutiveFailures++;
  if (consecutiveFailures >= OFFLINE_THRESHOLD && !piOffline) {
    piOffline = true;
    Serial.println("[NET] Pi offline erkannt, wechsle in Probe-Modus (60s Intervall)");
  }
}

// Der Pi hat geantwortet (egal mit welchem HTTP-Status): erreichbar.
void noteLinkOk() {
  if (piOffline) {
    piOffline = false;
    Serial.println("[NET] Pi wieder erreichbar, normaler Betrieb");
  }
  consecutiveFailures = 0;
}

// Anzeige-Ebene (OLED-Hinweis "Sendefehler").
void setSendError(bool hasError) {
  if (lastSendError != hasError) {
    lastSendError = hasError;
    sendErrorEventCounter++;
  }
}

void persistQueue() {
  Preferences p;
  if (!p.begin("runq", false)) return;
  if (queueCount == 0) {
    p.remove("q");
    p.end();
    return;
  }
  const size_t keep = queueCount < QUEUE_PERSIST_MAX ? queueCount : QUEUE_PERSIST_MAX;
  const size_t skip = queueCount - keep;
  static uint8_t blob[QUEUE_BLOB_MAX];
  QueueBlobHeader hdr{QUEUE_BLOB_MAGIC, (uint32_t)sizeof(RunDataSnapshot), (uint32_t)keep};
  memcpy(blob, &hdr, sizeof(hdr));
  for (size_t i = 0; i < keep; ++i) {
    const RunDataSnapshot& r = queueBuf[(queueHead + skip + i) % EXTERNAL_QUEUE_MAX];
    memcpy(blob + sizeof(hdr) + i * sizeof(RunDataSnapshot), &r, sizeof(RunDataSnapshot));
  }
  p.putBytes("q", blob, sizeof(hdr) + keep * sizeof(RunDataSnapshot));
  p.end();
}

void restoreQueue() {
  Preferences p;
  if (!p.begin("runq", true)) return;
  const size_t len = p.getBytesLength("q");
  if (len >= sizeof(QueueBlobHeader) && len <= QUEUE_BLOB_MAX) {
    static uint8_t blob[QUEUE_BLOB_MAX];
    p.getBytes("q", blob, len);
    QueueBlobHeader hdr;
    memcpy(&hdr, blob, sizeof(hdr));
    // Nach einem Firmware-Update kann sich RunDataSnapshot geaendert haben: Groesse pruefen.
    if (hdr.magic == QUEUE_BLOB_MAGIC && hdr.runSize == sizeof(RunDataSnapshot) &&
        hdr.count <= QUEUE_PERSIST_MAX && len == sizeof(hdr) + hdr.count * sizeof(RunDataSnapshot)) {
      for (uint32_t i = 0; i < hdr.count; ++i) {
        RunDataSnapshot r;
        memcpy(&r, blob + sizeof(hdr) + i * sizeof(RunDataSnapshot), sizeof(r));
        r.ageKnown = false;  // millis()-Bezug ist nach dem Neustart ungueltig
        queueBuf[queueCount++] = r;
      }
      Serial.printf("[NET] %u Laeufe aus Flash-Queue wiederhergestellt\n", (unsigned)queueCount);
    }
  }
  p.end();
}

bool bodySignalsAccepted(const String& body) {
  if (body.length() == 0) return false;
  String compact;
  compact.reserve(body.length());
  for (size_t i = 0; i < body.length(); ++i) {
    const char c = body.charAt(i);
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') compact += c;
  }
  return compact.indexOf("\"accepted\":true") >= 0 || compact.indexOf("\"duplicate\":true") >= 0;
}


String jsonEscape(const char* text) {
  String out;
  if (text == nullptr) return out;
  out.reserve(strlen(text) + 8);
  for (const char* p = text; *p; ++p) {
    const char c = *p;
    switch (c) {
      case '\\': out += F("\\\\"); break;
      case '"': out += F("\\\""); break;
      case '\n': out += F("\\n"); break;
      case '\r': out += F("\\r"); break;
      case '\t': out += F("\\t"); break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
          out += buf;
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

String buildHeartbeatUrl() {
  // Heartbeat-URL: apiPath-Basis + /heartbeat
  // z.B. /api/v1/runs -> http://host:port/api/v1/runs/heartbeat
  String url = String("http://") + transportCfg.host + ":" + String(transportCfg.port);
  const bool pathHasLeadingSlash = transportCfg.apiPath[0] == '/';
  if (!pathHasLeadingSlash) url += '/';
  url += transportCfg.apiPath;
  if (url.charAt(url.length() - 1) != '/') url += '/';
  url += "heartbeat";
  return url;
}

String buildTargetUrl() {
  String url = String("http://") + transportCfg.host + ":" + String(transportCfg.port);
  const bool pathHasLeadingSlash = transportCfg.apiPath[0] == '/';
  if (!pathHasLeadingSlash) url += '/';
  url += transportCfg.apiPath;
  return url;
}
String buildPayload(const RunDataSnapshot& run) {
  String payload;
  payload.reserve(400);
  payload += F("{\"protocol_version\":1");
  payload += F(",\"event_id\":\""); payload += jsonEscape(run.eventId); payload += '"';
  payload += F(",\"device_id\":\""); payload += jsonEscape(run.deviceId); payload += '"';
  payload += F(",\"boot_id\":"); payload += String(run.bootId);
  payload += F(",\"run_number\":"); payload += String(run.runNumber);
  payload += F(",\"time_ms\":"); payload += String(run.durationMs);
  payload += F(",\"start_weight_g\":"); payload += String(run.referenceWeightG, 3);
  payload += F(",\"min_weight_g\":"); payload += String(run.minWeightG, 3);
  payload += F(",\"start_drop_threshold_g\":"); payload += String(run.startDropThresholdG, 3);
  payload += F(",\"stop_rise_threshold_g\":"); payload += String(run.stopRiseThresholdG, 3);
  // Alter des Laufs: der Pi ordnet ihn der Person zu, die zur Messzeit aktiv war (nicht beim Eintreffen)
  if (run.ageKnown) { payload += F(",\"age_ms\":"); payload += String((uint32_t)(millis() - run.finishedAtMs)); }
  payload += F(",\"status\":\"ok\"");
  payload += F(",\"firmware_version\":\""); payload += jsonEscape(firmwareVersion); payload += '"';
  payload += F(",\"queue_depth\":"); payload += String(queueCount);
  payload += '}';
  return payload;
}

bool trySendHead() {
  if (queueCount == 0 || !hasValidTarget()) return false;
  const RunDataSnapshot& run = queueBuf[queueHead];
  String url = buildTargetUrl();

  HTTPClient http;
  if (!http.begin(url)) {
    setStatus("send fail begin");
    return false;
  }

  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(EXTERNAL_SEND_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  if (transportCfg.apiKey[0] != '\0') {
    http.addHeader("X-API-Key", transportCfg.apiKey);
  }

  const String payload = buildPayload(run);
  const int code = http.POST((uint8_t*)payload.c_str(), payload.length());
  String responseBody;
  if (code > 0) responseBody = http.getString();
  http.end();

  char statusBuf[96];
  if ((code >= 200 && code < 300) || bodySignalsAccepted(responseBody)) {
    queueHead = (queueHead + 1) % EXTERNAL_QUEUE_MAX;
    queueCount--;
    persistQueue();
    snprintf(statusBuf, sizeof(statusBuf), "send ok %d", code);
    setStatus(statusBuf);
    noteLinkOk();
    setSendError(false);
    return true;
  }

  if (code <= 0) {
    // Pi nicht erreichbar: spaeter erneut versuchen, zaehlt fuer die Offline-Erkennung
    snprintf(statusBuf, sizeof(statusBuf), "send fail code %d", code);
    setStatus(statusBuf);
    noteLinkFailure();
    setSendError(true);
    return false;
  }

  noteLinkOk();  // der Pi hat geantwortet
  const bool permanentReject = (code >= 400 && code < 500 && code != 408 && code != 429);
  if (permanentReject) {
    // z.B. 422: erneutes Senden hilft nie. Lauf verwerfen, damit er die Queue nicht blockiert.
    Serial.printf("[NET] Lauf %s verworfen (HTTP %d)\n", run.eventId, code);
    queueHead = (queueHead + 1) % EXTERNAL_QUEUE_MAX;
    queueCount--;
    persistQueue();
    snprintf(statusBuf, sizeof(statusBuf), "dropped http %d", code);
  } else {
    snprintf(statusBuf, sizeof(statusBuf), "send fail http %d", code);
  }
  setStatus(statusBuf);
  setSendError(true);
  return false;
}
}  // namespace

void externalInterfaceInit(const RuntimeConfig& cfg, const char* fwVersion, uint32_t bootId) {
  memset(queueBuf, 0, sizeof(queueBuf));
  queueHead = 0;
  queueCount = 0;
  lastSendAttemptMs = 0;
  lastHeartbeatMs = 0;  // direkt nach dem Start einen Heartbeat senden
  lastSendError = false;
  sendErrorEventCounter = 0;
  currentBootId = bootId;
  restoreQueue();
  externalInterfaceUpdateConfig(cfg);
  if (fwVersion != nullptr) {
    strncpy(firmwareVersion, fwVersion, sizeof(firmwareVersion) - 1);
    firmwareVersion[sizeof(firmwareVersion) - 1] = '\0';
  }
  setStatus("ready");
  noteLinkOk();
  setSendError(false);
}

void externalInterfaceUpdateConfig(const RuntimeConfig& cfg) {
  transportCfg.enabled = cfg.externalEnabled;
  strncpy(transportCfg.host, cfg.externalHost, sizeof(transportCfg.host) - 1);
  transportCfg.host[sizeof(transportCfg.host) - 1] = '\0';
  transportCfg.port = cfg.externalPort;
  strncpy(transportCfg.apiPath, cfg.externalApiPath, sizeof(transportCfg.apiPath) - 1);
  transportCfg.apiPath[sizeof(transportCfg.apiPath) - 1] = '\0';
  strncpy(transportCfg.apiKey, cfg.externalApiKey, sizeof(transportCfg.apiKey) - 1);
  transportCfg.apiKey[sizeof(transportCfg.apiKey) - 1] = '\0';
  // Geraete-Name fuer den Heartbeat; wirkt ohne Neustart
  strncpy(deviceId, cfg.deviceId, sizeof(deviceId) - 1);
  deviceId[sizeof(deviceId) - 1] = '\0';
}

bool externalInterfaceEnqueueRun(const RunDataSnapshot& snapshot) {
  // Schnittstelle aus: kein Ziel, Lauf wird nicht gepuffert
  if (!transportCfg.enabled) return false;
  bool droppedOldest = false;
  if (queueCount >= EXTERNAL_QUEUE_MAX) {
    queueHead = (queueHead + 1) % EXTERNAL_QUEUE_MAX;
    queueCount--;
    droppedOldest = true;
  }
  const size_t pos = (queueHead + queueCount) % EXTERNAL_QUEUE_MAX;
  queueBuf[pos] = snapshot;
  queueCount++;
  persistQueue();
  setStatus(droppedOldest ? "queue full dropped oldest" : "queued");
  return true;
}

void externalInterfaceService(uint32_t now, bool safeToRetry, bool quietHeartbeat) {
  if (!transportCfg.enabled) {
    // Bereits gepufferte Laeufe bleiben erhalten und werden nach dem Wiedereinschalten gesendet
    // (frueher wurden sie beim Ausschalten kommentarlos geloescht).
    setStatus("disabled");
    setSendError(false);
    return;
  }
  // Im Offline-Modus: nur alle 60s einen kurzen Probe-Heartbeat senden
  // Im Online-Modus: normaler Heartbeat alle 30s
  const uint32_t heartbeatInterval = piOffline ? OFFLINE_PROBE_INTERVAL_MS : HEARTBEAT_INTERVAL_MS;
  const bool heartbeatAllowed = safeToRetry || (quietHeartbeat && !piOffline);
  if (heartbeatAllowed && hasValidTarget() && (lastHeartbeatMs == 0 || (now - lastHeartbeatMs) >= heartbeatInterval)) {
    lastHeartbeatMs = now;
    String url = buildHeartbeatUrl();
    String payload = String("{\"device_id\":\"") + jsonEscape(deviceId) + "\"";
    payload += String(",\"boot_id\":") + String(currentBootId);
    payload += String(",\"firmware_version\":\"") + jsonEscape(firmwareVersion) + "\"";
    payload += String(",\"queue_depth\":") + String(queueCount) + "}";
    HTTPClient http;
    if (http.begin(url)) {
      http.setConnectTimeout(safeToRetry ? HTTP_CONNECT_TIMEOUT_MS : HTTP_CONNECT_TIMEOUT_QUIET_MS);
      http.setTimeout(800);
      http.addHeader("Content-Type", "application/json");
      if (transportCfg.apiKey[0] != '\0') http.addHeader("X-API-Key", transportCfg.apiKey);
      const int code = http.POST((uint8_t*)payload.c_str(), payload.length());
      http.end();
      if (code > 0) {
        noteLinkOk();  // der Pi hat geantwortet -> erreichbar
        if (code < 300) setSendError(false);
      } else {
        noteLinkFailure();  // zaehlt auch im Probe-Modus fuer die Offline-Erkennung
      }
    } else {
      noteLinkFailure();
    }
  }

  // Im Offline-Modus: Queue nicht abarbeiten (würde nur blockieren)
  if (piOffline) {
    setStatus("offline - probe alle 60s");
    return;
  }

  if (queueCount == 0) return;
  if (!hasValidTarget()) {
    setStatus("no target");
    setSendError(true);
    return;
  }
  if (!safeToRetry) return;
  if (lastSendAttemptMs != 0 && (now - lastSendAttemptMs) < EXTERNAL_RETRY_INTERVAL_MS) return;
  lastSendAttemptMs = now;
  (void)trySendHead();
}

size_t externalInterfaceQueueDepth() { return queueCount; }

const char* externalInterfaceLastStatus() { return lastStatus; }
bool externalInterfaceHasSendError() { return lastSendError; }
uint32_t externalInterfaceErrorEventCounter() { return sendErrorEventCounter; }
