#include "external_interface_module.h"

#include <HTTPClient.h>

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
ExternalTransportConfig transportCfg{};

bool hasValidTarget() {
  return transportCfg.enabled && transportCfg.host[0] != '\0' && transportCfg.apiPath[0] != '\0';
}

void setStatus(const char* text) {
  strncpy(lastStatus, text, sizeof(lastStatus) - 1);
  lastStatus[sizeof(lastStatus) - 1] = '\0';
}

String buildPayload(const RunDataSnapshot& run) {
  String payload;
  payload.reserve(360);
  payload += F("{\"protocol_version\":1");
  payload += F(",\"event_id\":\""); payload += run.eventId; payload += '"';
  payload += F(",\"device_id\":\""); payload += run.deviceId; payload += '"';
  payload += F(",\"boot_id\":"); payload += String(run.bootId);
  payload += F(",\"run_number\":"); payload += String(run.runNumber);
  payload += F(",\"time_ms\":"); payload += String(run.durationMs);
  payload += F(",\"start_weight_g\":"); payload += String(run.referenceWeightG, 3);
  payload += F(",\"min_weight_g\":"); payload += String(run.minWeightG, 3);
  payload += F(",\"start_drop_threshold_g\":"); payload += String(run.startDropThresholdG, 3);
  payload += F(",\"stop_rise_threshold_g\":"); payload += String(run.stopRiseThresholdG, 3);
  payload += F(",\"firmware_version\":\""); payload += firmwareVersion; payload += '"';
  payload += F(",\"queue_depth\":"); payload += String(queueCount);
  payload += '}';
  return payload;
}

bool trySendHead() {
  if (queueCount == 0 || !hasValidTarget()) return false;
  const RunDataSnapshot& run = queueBuf[queueHead];
  String url = String("http://") + transportCfg.host + ":" + String(transportCfg.port) + transportCfg.apiPath;

  HTTPClient http;
  if (!http.begin(url)) {
    setStatus("http begin failed");
    return false;
  }

  http.setTimeout(EXTERNAL_SEND_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  if (transportCfg.apiKey[0] != '\0') {
    http.addHeader("X-API-Key", transportCfg.apiKey);
  }

  const String payload = buildPayload(run);
  const int code = http.POST((uint8_t*)payload.c_str(), payload.length());
  http.end();

  if (code >= 200 && code < 300) {
    queueHead = (queueHead + 1) % EXTERNAL_QUEUE_MAX;
    queueCount--;
    setStatus("send ok");
    return true;
  }

  char msg[64];
  snprintf(msg, sizeof(msg), "send fail http=%d", code);
  setStatus(msg);
  return false;
}
}  // namespace

void externalInterfaceInit(const RuntimeConfig& cfg, const char* fwVersion) {
  memset(queueBuf, 0, sizeof(queueBuf));
  queueHead = 0;
  queueCount = 0;
  lastSendAttemptMs = 0;
  externalInterfaceUpdateConfig(cfg);
  if (fwVersion != nullptr) {
    strncpy(firmwareVersion, fwVersion, sizeof(firmwareVersion) - 1);
    firmwareVersion[sizeof(firmwareVersion) - 1] = '\0';
  }
  setStatus("ready");
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
  if (queueCount >= EXTERNAL_QUEUE_MAX) {
    setStatus("queue full");
    return false;
  }
  const size_t pos = (queueHead + queueCount) % EXTERNAL_QUEUE_MAX;
  queueBuf[pos] = snapshot;
  queueCount++;
  setStatus("queued");
  return true;
}

void externalInterfaceService(uint32_t now, bool safeToRetry) {
  if (queueCount == 0) return;
  if (!hasValidTarget()) {
    setStatus("disabled/no target");
    return;
  }
  if (!safeToRetry) return;
  if (lastSendAttemptMs != 0 && (now - lastSendAttemptMs) < EXTERNAL_RETRY_INTERVAL_MS) return;
  lastSendAttemptMs = now;
  (void)trySendHead();
}

size_t externalInterfaceQueueDepth() { return queueCount; }

const char* externalInterfaceLastStatus() { return lastStatus; }
