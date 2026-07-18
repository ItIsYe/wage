#pragma once

#include <Arduino.h>
#include "types.h"

// Initialisiert Rate-Limiter.
void serialDebugInit();

// Gibt alle Rohwerte und Zustandsinfos aus (RAW1/RAW2/AVG etc.).
void serialDebugPrintBase(uint32_t now, long raw1, long raw2,
                          float w_raw1, float w_raw2, float w_avg, float w_filt,
                          bool isStable, State state, bool objectPresent,
                          const char* errStr);

// Gibt READY-State Werte aus (Schwellen, Drop etc.).
void serialDebugPrintReady(uint32_t now, float w, float referenceWeightG,
                           float startDropThresholdG, float stopRiseThresholdG,
                           float drop);

// Gibt TIMING-State Werte aus.
void serialDebugPrintTiming(uint32_t now, float w, float minDuringTiming,
                            float stopThreshold, float rebound,
                            uint32_t stopHoldMs, bool stopCandidateActive);

// Gibt RECOVER-State Werte aus.
void serialDebugPrintRecover(uint32_t now, const char* msg);
