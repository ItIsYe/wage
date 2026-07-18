#include "run_module.h"
#include "config.h"

#include <cstring>
#include <cstdio>
#include <algorithm>

namespace {
  uint32_t g_bootId = 0;
  uint32_t g_runCounter = 0;
  char     g_deviceId[32] = {};

  float g_referenceWeightG   = 0.0f;
  float g_startDropThresholdG = 0.0f;
  float g_stopRiseThresholdG  = 0.0f;
  float g_minDuringTiming     = 1e9f;
}

void runModuleInit(uint32_t bootId, const char* deviceId) {
  g_bootId     = bootId;
  g_runCounter = 0;
  strncpy(g_deviceId, deviceId, sizeof(g_deviceId) - 1);
  g_deviceId[sizeof(g_deviceId) - 1] = '\0';
}

static float percentOfReference(float reference, float percent) {
  return reference * (percent / 100.0f);
}

void runModuleSetReference(float referenceWeightG, const RuntimeConfig& cfg) {
  g_referenceWeightG = std::max(referenceWeightG, cfg.objectPresentG);
  g_startDropThresholdG = std::max(MIN_DYNAMIC_THRESHOLD_G,
    percentOfReference(g_referenceWeightG, cfg.startDropPercent));
  g_stopRiseThresholdG  = std::max(MIN_DYNAMIC_THRESHOLD_G,
    percentOfReference(g_referenceWeightG, cfg.stopRisePercent));

  if (MASTER_DEBUG_LOG) {
    Serial.print("[RUN] ref=");
    Serial.print(g_referenceWeightG, 2);
    Serial.print(" startDrop=");
    Serial.print(g_startDropThresholdG, 2);
    Serial.print(" stopRise=");
    Serial.println(g_stopRiseThresholdG, 2);
  }
}

void runModuleUpdateMin(float weightG) {
  if (weightG < g_minDuringTiming) g_minDuringTiming = weightG;
}

float runModuleGetMin()                { return g_minDuringTiming; }
float runModuleGetStartDropThreshold() { return g_startDropThresholdG; }
float runModuleGetStopRiseThreshold()  { return g_stopRiseThresholdG; }
float runModuleGetReference()          { return g_referenceWeightG; }

RunDataSnapshot runModuleBuildSnapshot(uint32_t finishedAtMs, uint32_t durationMs) {
  RunDataSnapshot snap{};
  snap.bootId        = g_bootId;
  snap.runNumber     = ++g_runCounter;
  snap.finishedAtMs  = finishedAtMs;
  snap.durationMs    = durationMs;
  snap.referenceWeightG   = g_referenceWeightG;
  snap.minWeightG         = g_minDuringTiming;
  snap.startDropThresholdG = g_startDropThresholdG;
  snap.stopRiseThresholdG  = g_stopRiseThresholdG;
  strncpy(snap.deviceId, g_deviceId, sizeof(snap.deviceId) - 1);
  snap.deviceId[sizeof(snap.deviceId) - 1] = '\0';
  snprintf(snap.eventId, sizeof(snap.eventId), "%s-%lu-%lu",
           snap.deviceId,
           (unsigned long)snap.bootId,
           (unsigned long)snap.runNumber);
  return snap;
}

void runModuleReset() {
  g_referenceWeightG    = 0.0f;
  g_startDropThresholdG = 0.0f;
  g_stopRiseThresholdG  = 0.0f;
  g_minDuringTiming     = 1e9f;
}
