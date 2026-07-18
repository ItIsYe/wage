#include "external_interface_module.h"

#include <cstdio>
#include <HTTPClient.h>
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
static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 30000;

bool hasValidTarget() {
  return transportCfg.enabled && transportCfg.host[0] != '\0' && transportCfg.apiPath[0] != '\0';
}

void setStatus(const char* text) {
  strncpy(lastStatus, text, sizeof(lastStatus) - 1);
  lastStatus[sizeof(lastStatus) - 1] = '\0';
}

void setSendErrorState(bool hasError) {
  if (lastSendError != hasError) {
    lastSendError = hasError;
    sendErrorEventCounter++;
  }
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
  payload.reserve(360);
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

  if ((code >= 200 && code < 300) || bodySignalsAccepted(responseBody)) {
    queueHead = (queueHead + 1) % EXTERNAL_QUEUE_MAX;
    queueCount--;
    char statusBuf[96];
    snprintf(statusBuf, sizeof(statusBuf), "send ok %d", code);
    setStatus(statusBuf);
    setSendErrorState(false);
    return true;
  }

  char statusBuf[96];
  snprintf(statusBuf, sizeof(statusBuf), code > 0 ? "send fail http %d" : "send fail code %d", code);
  setStatus(statusBuf);
  setSendErrorState(true);
  return false;
}
}  // namespace

void externalInterfaceInit(const RuntimeConfig& cfg, const char* fwVersion) {
  memset(queueBuf, 0, sizeof(queueBuf));
  queueHead = 0;
  queueCount = 0;
  lastSendAttemptMs = 0;
  lastSendError = false;
  sendErrorEventCounter = 0;
  externalInterfaceUpdateConfig(cfg);
  strncpy(deviceId, cfg.deviceId, sizeof(deviceId) - 1);
  deviceId[sizeof(deviceId) - 1] = '\0';
  if (fwVersion != nullptr) {
    strncpy(firmwareVersion, fwVersion, sizeof(firmwareVersion) - 1);
    firmwareVersion[sizeof(firmwareVersion) - 1] = '\0';
  }
  setStatus("ready");
  setSendErrorState(false);
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
}

bool externalInterfaceEnqueueRun(const RunDataSnapshot& snapshot) {
  bool droppedOldest = false;
  if (queueCount >= EXTERNAL_QUEUE_MAX) {
    queueHead = (queueHead + 1) % EXTERNAL_QUEUE_MAX;
    queueCount--;
    droppedOldest = true;
  }
  const size_t pos = (queueHead + queueCount) % EXTERNAL_QUEUE_MAX;
  queueBuf[pos] = snapshot;
  queueCount++;
  setStatus(droppedOldest ? "queue full dropped oldest" : "queued");
  return true;
}

void externalInterfaceService(uint32_t now, bool safeToRetry) {
  if (!transportCfg.enabled) {
    if (queueCount != 0) {
      queueHead = 0;
      queueCount = 0;
    }
    setStatus("disabled");
    setSendErrorState(false);
    return;
  }
  // Heartbeat nur im sicheren Zustand (nicht waehrend GLASS_DETECTED/TIMING)
  if (safeToRetry && hasValidTarget() && (lastHeartbeatMs == 0 || (now - lastHeartbeatMs) >= HEARTBEAT_INTERVAL_MS)) {
    lastHeartbeatMs = now;
    String url = buildHeartbeatUrl();
    String payload = String("{\"device_id\":\"") + jsonEscape(deviceId) + "\"";
    payload += String(",\"firmware_version\":\"") + jsonEscape(firmwareVersion) + "\"";
    payload += String(",\"queue_depth\":") + String(queueCount) + "}";
    HTTPClient http;
    if (http.begin(url)) {
      http.setTimeout(800);  // Heartbeat: kurzer Timeout, Pi ist im LAN
      http.addHeader("Content-Type", "application/json");
      if (transportCfg.apiKey[0] != '\0') http.addHeader("X-API-Key", transportCfg.apiKey);
      http.POST((uint8_t*)payload.c_str(), payload.length());
      http.end();
    }
  }

  if (queueCount == 0) return;
  if (!hasValidTarget()) {
    setStatus("no target");
    setSendErrorState(true);
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
