#include "serial_debug_module.h"
#include "config.h"

namespace {
  uint32_t lastBaseMs    = 0;
  uint32_t lastReadyMs   = 0;
  uint32_t lastTimingMs  = 0;
  uint32_t lastRecoverMs = 0;
}

void serialDebugInit() {
  lastBaseMs = lastReadyMs = lastTimingMs = lastRecoverMs = 0;
}

void serialDebugPrintBase(uint32_t now, long raw1, long raw2,
                          float w_raw1, float w_raw2, float w_avg, float w_filt,
                          bool isStable, State state, bool objectPresent,
                          const char* errStr) {
  if (!MASTER_DEBUG_LOG) return;
  if ((now - lastBaseMs) < SERIAL_BASE_REFRESH_MS) return;
  lastBaseMs = now;

  Serial.print("RAW1:"); Serial.print(raw1);
  Serial.print(" g1:");  Serial.print(w_raw1, 2);
  Serial.print("  ||  RAW2:"); Serial.print(raw2);
  Serial.print(" g2:");  Serial.print(w_raw2, 2);
  Serial.print("  AVG:"); Serial.print(w_avg, 2);
  Serial.print("  FILT:"); Serial.print(w_filt, 2);
  Serial.print("  STB:"); Serial.print(isStable ? 1 : 0);
  Serial.print("  STATE:"); Serial.print((int)state);
  Serial.print("  OBJ:"); Serial.print(objectPresent ? 1 : 0);
  Serial.print("  ERR:"); Serial.println(errStr);
}

void serialDebugPrintReady(uint32_t now, float w, float referenceWeightG,
                           float startDropThresholdG, float stopRiseThresholdG,
                           float drop) {
  if (!MASTER_DEBUG_LOG) return;
  if ((now - lastReadyMs) < SERIAL_STATE_REFRESH_MS) return;
  lastReadyMs = now;

  Serial.print("[READY] w="); Serial.print(w, 2);
  Serial.print(" ref=");      Serial.print(referenceWeightG, 2);
  Serial.print(" startThr="); Serial.print(startDropThresholdG, 2);
  Serial.print(" stopThr=");  Serial.print(stopRiseThresholdG, 2);
  Serial.print(" drop=");     Serial.println(drop, 2);
}

void serialDebugPrintTiming(uint32_t now, float w, float minDuringTiming,
                            float stopThreshold, float rebound,
                            uint32_t stopHoldMs, bool stopCandidateActive) {
  if (!MASTER_DEBUG_LOG) return;
  if ((now - lastTimingMs) < SERIAL_STATE_REFRESH_MS) return;
  lastTimingMs = now;

  Serial.print("[TIMING] w=");      Serial.print(w, 2);
  Serial.print(" min=");            Serial.print(minDuringTiming, 2);
  Serial.print(" stopThr=");        Serial.print(stopThreshold, 2);
  Serial.print(" rebound=");        Serial.print(rebound, 2);
  Serial.print(" candHold=");       Serial.print(stopHoldMs);
  Serial.print("ms active=");       Serial.println(stopCandidateActive ? 1 : 0);
}

void serialDebugPrintRecover(uint32_t now, const char* msg) {
  if (!MASTER_DEBUG_LOG) return;
  if ((now - lastRecoverMs) < SERIAL_STATE_REFRESH_MS) return;
  lastRecoverMs = now;
  Serial.println(msg);
}
