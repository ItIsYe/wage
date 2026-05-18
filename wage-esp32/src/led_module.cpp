#include "led_module.h"

#include <FastLED.h>

#include "config.h"
#include "led_ring1_module.h"
#include "led_ring2_module.h"

extern RuntimeConfig activeConfig;

static CRGB primaryLeds[PIXEL_COUNT];
#if RING2_ENABLED
static CRGB ring2Leds[RING2_PIXEL_COUNT];
#endif
static LedMode currentLedMode = LedMode::ALL_OFF;

static inline void logRing2ServiceDispatch(uint32_t now, bool ring2ForceTestActive) {
  if (!MASTER_DEBUG_LOG) return;
  static uint32_t lastRing2CallLogMs = 0;
  static uint32_t lastRing2SkipLogMs = 0;
  if (!ring2ForceTestActive && now - lastRing2CallLogMs >= 2000) {
    lastRing2CallLogMs = now;
    Serial.println("[LED FASTLED BUILD] early-call ring2Service");
  } else if (ring2ForceTestActive && now - lastRing2SkipLogMs >= 2000) {
    lastRing2SkipLogMs = now;
    Serial.println("[LED FASTLED BUILD] skipping ring2Service: force test active");
  }
}

void ledsInit() {
  FastLED.addLeds<WS2812B, LED_STRIP_PIN, GRB>(primaryLeds, PIXEL_COUNT);
  ring1Init(primaryLeds);
#if RING2_ENABLED
  FastLED.addLeds<WS2812B, RING2_PIN, GRB>(ring2Leds, RING2_PIXEL_COUNT);
  ring2Init(ring2Leds);
#endif
  FastLED.show();
}

void ledsSetMode(LedMode m) {
  if (currentLedMode == m) return;
  currentLedMode = m;
  ring1SetMode(m, millis());
}

void ledService(uint32_t now) {
  const bool ring2ForceTestActive =
#if RING2_ENABLED
    RING2_FORCE_INDEPENDENT_TEST;
#else
    false;
#endif

#if RING2_ENABLED
  if (MASTER_DEBUG_LOG) {
    static uint32_t lastInlineEntryLogMs = 0;
    if (now - lastInlineEntryLogMs >= 2000) {
      lastInlineEntryLogMs = now;
      Serial.printf("[RING2 FASTLED ENTRY] force=%u pin=%u pixels=%u now=%lu\n",
                    (unsigned)RING2_FORCE_INDEPENDENT_TEST,
                    (unsigned)RING2_PIN,
                    (unsigned)RING2_PIXEL_COUNT,
                    (unsigned long)now);
    }
  }
#endif

  if (MASTER_DEBUG_LOG) {
    static uint32_t lastLedDiagMs = 0;
    if (now - lastLedDiagMs >= 2000) {
      lastLedDiagMs = now;
      Serial.printf("[LED FASTLED BUILD] service mode=%u ring2Force=%u\n",
                    (unsigned)currentLedMode,
                    (unsigned)ring2ForceTestActive);
    }
  }

  bool showNeeded = false;

  static bool allOnApplied = false;
  if (activeConfig.pixelDebugAllOn) {
    if (!allOnApplied) {
      ring1FillDebugAllOn();
      showNeeded = true;
      allOnApplied = true;
    }
#if RING2_ENABLED
    if (!ring2ForceTestActive) {
      logRing2ServiceDispatch(now, false);
      showNeeded = ring2Service(now) || showNeeded;
    } else {
      showNeeded = ring2ForceTestService(now) || showNeeded;
      logRing2ServiceDispatch(now, true);
    }
#endif
    if (showNeeded) {
      FastLED.show();
#if RING2_ENABLED
      ring2LogWrite(now);
#endif
    }
    return;
  }
  allOnApplied = false;

  showNeeded = ring1Service(now) || showNeeded;
#if RING2_ENABLED
  if (!ring2ForceTestActive) {
    logRing2ServiceDispatch(now, false);
    showNeeded = ring2Service(now) || showNeeded;
  } else {
    showNeeded = ring2ForceTestService(now) || showNeeded;
    logRing2ServiceDispatch(now, true);
  }
#endif

  if (showNeeded) {
    FastLED.show();
#if RING2_ENABLED
    ring2LogWrite(now);
#endif
  }
}

void ledApplyBrightnessForCurrentMode() { ring1ApplyBrightnessForCurrentMode(); }

void ledMarkRing2Dirty() {
#if RING2_ENABLED
  ring2MarkDirty();
#endif
}

void ledMarkAllDirty() {
  ring1MarkDirty();
  ledMarkRing2Dirty();
}

void ledClear() {
  ring1Clear();
#if RING2_ENABLED
  ring2Clear();
#endif
}

void ledShow() { FastLED.show(); }
