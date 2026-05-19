#include "led_module.h"

#include <FastLED.h>

#include "config.h"
#include "led_ring1_module.h"
#include "led_ring2_module.h"

extern RuntimeConfig activeConfig;

static CRGB primaryLeds[PIXEL_COUNT];
static CRGB ring2Leds[RING2_PIXEL_COUNT];
static LedMode currentLedMode = LedMode::ALL_OFF;

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

  if (RING2_ENABLED && !ring2ForceTestActive) {
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
    ring1Changed = ring1Service(now);
  }

  if (ring1Changed || ring2Changed) {
    FastLED.show();
    if (RING2_ENABLED && ring2Changed) {
      ring2LogWrite(now);
    }
  }
}

void ledApplyBrightnessForCurrentMode() { ring1ApplyBrightnessForCurrentMode(); }

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
