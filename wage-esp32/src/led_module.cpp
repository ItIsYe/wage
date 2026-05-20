#include "led_module.h"

#include <FastLED.h>

#include "config.h"
#include "led_ring1_module.h"
#include "led_ring2_module.h"

extern RuntimeConfig activeConfig;

static CRGB primaryLeds[PIXEL_COUNT];
static CRGB ring2Leds[RING2_PIXEL_COUNT];
static LedMode currentLedMode = LedMode::ALL_OFF;
static bool sharedStandbyActive = false;
static uint32_t sharedStandbyFrameNextMs = 0;
static uint32_t sharedStandbyChangeNextMs = 0;
static constexpr uint16_t SHARED_PIXELS = PIXEL_COUNT + RING2_PIXEL_COUNT;
static bool sharedStandbyTwinkleOn[SHARED_PIXELS] = {};
static bool sharedStandbyRenderMask[SHARED_PIXELS] = {};
static uint16_t sharedStandbyHue[SHARED_PIXELS] = {};
static uint8_t sharedStandbyValue[SHARED_PIXELS] = {};
static uint8_t sharedStandbyTargetValue[SHARED_PIXELS] = {};
static uint16_t sharedStandbyOnCount = 0;
static uint16_t sharedStandbyCursor = 0;

static inline uint32_t sanitizeRangeMin(uint32_t minValue, uint32_t maxValue) { return (minValue > maxValue) ? maxValue : minValue; }
static inline uint32_t sanitizeStandbyFrameMs(uint32_t frameMs) {
  if (frameMs < 30U) return 30U;
  if (frameMs > 1000U) return 1000U;
  return frameMs;
}
static inline uint32_t sanitizeSharedStandbyFrameMs(uint32_t frameMs) {
  if (frameMs < 80U) return 80U;
  if (frameMs > 1000U) return 1000U;
  return frameMs;
}
static inline void sanitizeSharedStandbyChangeRange(uint32_t& minMs, uint32_t& maxMs) {
  if (minMs < 700U) minMs = 700U;
  if (maxMs < 900U) maxMs = 900U;
  if (maxMs > 4000U) maxMs = 4000U;
  if (maxMs < minMs) maxMs = minMs;
}
static inline uint32_t randomInclusiveU32(uint32_t minValue, uint32_t maxValue) {
  if (minValue > maxValue) { const uint32_t t = minValue; minValue = maxValue; maxValue = t; }
  return minValue + (uint32_t)random(0, (long)(maxValue - minValue + 1U));
}
static inline uint8_t randomInclusiveU8(uint8_t minValue, uint8_t maxValue) {
  if (minValue > maxValue) { const uint8_t t = minValue; minValue = maxValue; maxValue = t; }
  return (uint8_t)(minValue + (uint8_t)random(0, (int16_t)(maxValue - minValue + 1U)));
}
static uint8_t scaleSharedStandbyCount(uint8_t count) {
  if (PIXEL_COUNT == 0) return count;

  uint16_t scaled = ((uint16_t)count * SHARED_PIXELS + PIXEL_COUNT - 1U) / PIXEL_COUNT;
  if (scaled > SHARED_PIXELS) scaled = SHARED_PIXELS;
  return (uint8_t)scaled;
}
static uint8_t sharedStandbyBaseValue(uint8_t valueMin, uint8_t valueMax) {
  if (valueMax == 0) return 0;

  uint8_t base = valueMin / 2U;
  if (base < 12U) base = 12U;
  if (base > 42U) base = 42U;

  if (base > valueMax) base = valueMax;
  return base;
}
static void applySharedStandbyFrame() {
  ring1ApplySharedStandby(sharedStandbyRenderMask, sharedStandbyHue, sharedStandbyValue, SHARED_PIXELS);
  if (RING2_ENABLED) {
    ring2ApplySharedStandby(sharedStandbyRenderMask, sharedStandbyHue, sharedStandbyValue, SHARED_PIXELS);
  }
}
static void sharedStandbyInit(uint32_t now) {
  uint32_t changeMaxMs = activeConfig.standbyChangeMaxMs;
  uint32_t changeMinMs = sanitizeRangeMin(activeConfig.standbyChangeMinMs, changeMaxMs);
  sanitizeSharedStandbyChangeRange(changeMinMs, changeMaxMs);
  const uint8_t valueMax = activeConfig.standbyValueMax;
  const uint8_t valueMin = (activeConfig.standbyValueMin > valueMax) ? valueMax : activeConfig.standbyValueMin;
  const uint8_t valueBase = sharedStandbyBaseValue(valueMin, valueMax);
  const uint8_t onMax = scaleSharedStandbyCount(activeConfig.standbyOnMax);
  const uint8_t onMinRaw = scaleSharedStandbyCount(activeConfig.standbyOnMin);
  const uint8_t onMin = (onMinRaw > onMax) ? onMax : onMinRaw;
  sharedStandbyFrameNextMs = now + sanitizeSharedStandbyFrameMs(activeConfig.standbyFrameMs);
  sharedStandbyChangeNextMs = now + randomInclusiveU32(changeMinMs, changeMaxMs);
  sharedStandbyOnCount = 0;
  sharedStandbyCursor = (uint16_t)random(0, SHARED_PIXELS);
  for (uint16_t i = 0; i < SHARED_PIXELS; ++i) {
    sharedStandbyTwinkleOn[i] = false;
    sharedStandbyRenderMask[i] = true;
    sharedStandbyHue[i] = (uint16_t)random(0, 65536);
    sharedStandbyValue[i] = valueBase;
    sharedStandbyTargetValue[i] = valueBase;
  }
  const uint8_t initialOn = randomInclusiveU8(onMin, onMax);
  const uint16_t initialStride = (initialOn > 0U) ? (uint16_t)(SHARED_PIXELS / initialOn) : SHARED_PIXELS;
  const uint16_t startOffset = (uint16_t)random(0, SHARED_PIXELS);
  for (uint8_t i = 0; i < initialOn; ++i) {
    uint16_t idx = (uint16_t)((startOffset + (uint32_t)i * initialStride) % SHARED_PIXELS);
    for (uint16_t tries = 0; tries < SHARED_PIXELS && sharedStandbyTwinkleOn[idx]; ++tries) {
      idx = (uint16_t)((idx + 1U) % SHARED_PIXELS);
    }
    if (!sharedStandbyTwinkleOn[idx]) {
      sharedStandbyTwinkleOn[idx] = true;
      sharedStandbyTargetValue[idx] = randomInclusiveU8(valueMin, valueMax);
      ++sharedStandbyOnCount;
    }
  }
}
static bool sharedStandbyService(uint32_t now) {
  bool changed = false;
  if (now >= sharedStandbyChangeNextMs) {
    uint32_t changeMaxMs = activeConfig.standbyChangeMaxMs;
    uint32_t changeMinMs = sanitizeRangeMin(activeConfig.standbyChangeMinMs, changeMaxMs);
    sanitizeSharedStandbyChangeRange(changeMinMs, changeMaxMs);
    const uint8_t valueMax = activeConfig.standbyValueMax;
    const uint8_t valueMin = (activeConfig.standbyValueMin > valueMax) ? valueMax : activeConfig.standbyValueMin;
    const uint8_t valueBase = sharedStandbyBaseValue(valueMin, valueMax);
    const uint8_t onMax = scaleSharedStandbyCount(activeConfig.standbyOnMax);
    const uint8_t onMinRaw = scaleSharedStandbyCount(activeConfig.standbyOnMin);
    const uint8_t onMin = (onMinRaw > onMax) ? onMax : onMinRaw;
    sharedStandbyChangeNextMs = now + randomInclusiveU32(changeMinMs, changeMaxMs);
    const bool needMore = sharedStandbyOnCount < onMin;
    const bool needLess = sharedStandbyOnCount > onMax;
    const bool shouldToggle = !needMore && !needLess && (random(0, 100) < 20);
    if (needMore || needLess || shouldToggle) {
      const uint16_t stride = 1U;
      const uint16_t start = sharedStandbyCursor;
      for (uint16_t tries = 0; tries < SHARED_PIXELS; ++tries) {
        const uint16_t i = (uint16_t)((start + (uint32_t)tries * stride) % SHARED_PIXELS);
        if (needMore && !sharedStandbyTwinkleOn[i]) { sharedStandbyHue[i] = (uint16_t)random(0, 65536); sharedStandbyTwinkleOn[i] = true; sharedStandbyTargetValue[i] = randomInclusiveU8(valueMin, valueMax); ++sharedStandbyOnCount; sharedStandbyCursor = (uint16_t)((i + stride) % SHARED_PIXELS); changed = true; break; }
        if (needLess && sharedStandbyTwinkleOn[i]) { sharedStandbyTwinkleOn[i] = false; sharedStandbyTargetValue[i] = valueBase; --sharedStandbyOnCount; sharedStandbyCursor = (uint16_t)((i + stride) % SHARED_PIXELS); changed = true; break; }
        if (!needMore && !needLess) {
          if (!sharedStandbyTwinkleOn[i]) { sharedStandbyHue[i] = (uint16_t)random(0, 65536); sharedStandbyTwinkleOn[i] = true; sharedStandbyTargetValue[i] = randomInclusiveU8(valueMin, valueMax); ++sharedStandbyOnCount; }
          else { sharedStandbyTwinkleOn[i] = false; sharedStandbyTargetValue[i] = valueBase; --sharedStandbyOnCount; }
          sharedStandbyCursor = (uint16_t)((i + stride) % SHARED_PIXELS);
          changed = true;
          break;
        }
      }
    }
  }
  if (now >= sharedStandbyFrameNextMs) {
    const uint8_t valueMax = activeConfig.standbyValueMax;
    const uint8_t valueMin = (activeConfig.standbyValueMin > valueMax) ? valueMax : activeConfig.standbyValueMin;
    const uint8_t valueBase = sharedStandbyBaseValue(valueMin, valueMax);
    sharedStandbyFrameNextMs = now + sanitizeSharedStandbyFrameMs(activeConfig.standbyFrameMs);
    for (uint16_t i = 0; i < SHARED_PIXELS; ++i) {
      const uint8_t target = sharedStandbyTargetValue[i];
      if (sharedStandbyTwinkleOn[i]) {
        sharedStandbyHue[i] = (uint16_t)(sharedStandbyHue[i] + (int16_t)random(-1, 2));
      }
      int16_t dynamicTarget = target;
      if (sharedStandbyTwinkleOn[i]) {
        dynamicTarget += (int16_t)random(-1, 2);
        if (dynamicTarget < valueMin) dynamicTarget = valueMin;
        if (dynamicTarget > valueMax) dynamicTarget = valueMax;
      } else {
        dynamicTarget = valueBase;
        sharedStandbyTargetValue[i] = valueBase;
      }
      const int16_t current = sharedStandbyValue[i];
      const int16_t delta = dynamicTarget - current;
      if (delta != 0) {
        int16_t step = delta / 4;
        if (step == 0) step = (delta > 0) ? 1 : -1;
        sharedStandbyValue[i] = (uint8_t)(current + step);
      }
    }
    changed = true;
  }
  if (changed) {
    applySharedStandbyFrame();
  }
  return changed;
}

static inline void logRing2ServiceDispatch(uint32_t now, bool ring2ForceTestActive) {
  if (!MASTER_DEBUG_LOG || !ring2ForceTestActive) return;
  static uint32_t lastRing2SkipLogMs = 0;
  if (now - lastRing2SkipLogMs >= 2000) {
    lastRing2SkipLogMs = now;
    Serial.println("[LED FASTLED BUILD] skipping ring2Service: force test active");
  }
}

void ledsInit() {
  FastLED.addLeds<WS2812B, LED_STRIP_PIN, GRB>(primaryLeds, PIXEL_COUNT);
  ring1Init(primaryLeds);
  if (RING2_ENABLED) {
    FastLED.addLeds<WS2812B, RING2_PIN, GRB>(ring2Leds, RING2_PIXEL_COUNT);
    ring2Init(ring2Leds);
  }
  FastLED.show();
}

void ledsSetMode(LedMode m) {
  if (currentLedMode == m) return;
  currentLedMode = m;
  ring1SetMode(m, millis());
}

void ledSetState(State state) {
  if (RING2_ENABLED) ring2SetState(state);
}

void ledService(uint32_t now) {
  const bool ring2ForceTestActive = RING2_ENABLED && RING2_FORCE_INDEPENDENT_TEST;


  if (MASTER_DEBUG_LOG) {
    static uint32_t lastLedDiagMs = 0;
    if (now - lastLedDiagMs >= 2000) {
      lastLedDiagMs = now;
      Serial.printf("[LED FASTLED BUILD] service mode=%u ring2Force=%u\n",
                    (unsigned)currentLedMode,
                    (unsigned)ring2ForceTestActive);
    }
  }

  bool ring1Changed = false;
  bool ring2Changed = false;

  const bool sharedStandbyShouldRun = RING2_ENABLED && activeConfig.ring2Enabled && !ring2ForceTestActive && !activeConfig.pixelDebugAllOn && !activeConfig.ring2DebugAllOn && currentLedMode == LedMode::STANDBY_TWINKLE && ring2IsStandbyState();
  if (sharedStandbyShouldRun && !sharedStandbyActive) {
    sharedStandbyActive = true;
    sharedStandbyInit(now);
    applySharedStandbyFrame();
    ring1Changed = true;
    ring2Changed = true;
    if (MASTER_DEBUG_LOG) {
      Serial.printf("[LED SHARED STANDBY] active totalPixels=%u ring1=%u ring2=%u\n",
                    (unsigned)SHARED_PIXELS,
                    (unsigned)PIXEL_COUNT,
                    (unsigned)RING2_PIXEL_COUNT);
    }
  }
  if (!sharedStandbyShouldRun && sharedStandbyActive) sharedStandbyActive = false;

  if (sharedStandbyActive) {
    ring1Changed = sharedStandbyService(now);
    ring2Changed = ring1Changed;
  } else if (RING2_ENABLED && !ring2ForceTestActive) {
    if (MASTER_DEBUG_LOG) {
      static uint32_t lastEarlyRing2CallLogMs = 0;
      if (now - lastEarlyRing2CallLogMs >= 1000) {
        lastEarlyRing2CallLogMs = now;
        Serial.println("[LED FASTLED BUILD] early-call ring2Service");
      }
    }
    ring2Changed = ring2Service(now);
  } else if (RING2_ENABLED) {
    ring2Changed = ring2ForceTestService(now);
    logRing2ServiceDispatch(now, true);
  }

  static bool allOnApplied = false;
  if (activeConfig.pixelDebugAllOn) {
    if (!allOnApplied) {
      ring1FillDebugAllOn();
      ring1Changed = true;
      allOnApplied = true;
    }
  } else {
    allOnApplied = false;
    if (!sharedStandbyActive) {
      ring1Changed = ring1Service(now);
    }
  }

  if (ring1Changed || ring2Changed) {
    FastLED.show();
    if (RING2_ENABLED && ring2Changed) {
      ring2LogWrite(now);
    }
  }
}

void ledApplyBrightnessForCurrentMode() {
  ring1ApplyBrightnessForCurrentMode();
  if (sharedStandbyActive) {
    applySharedStandbyFrame();
  }
}

void ledMarkRing2Dirty() {
  if (RING2_ENABLED) ring2MarkDirty();
}

void ledMarkAllDirty() {
  ring1MarkDirty();
  ledMarkRing2Dirty();
}

void ledClear() {
  ring1Clear();
  if (RING2_ENABLED) ring2Clear();
}

void ledShow() { FastLED.show(); }
