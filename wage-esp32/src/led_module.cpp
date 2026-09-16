#include "led_module.h"

#include <FastLED.h>

#include "config.h"
#include "led_ring1_module.h"
#include "led_ring2_module.h"

extern RuntimeConfig activeConfig;

// Ring1 und Ring2 laufen unabhaengig voneinander. Es gab frueher einen
// "Shared Standby" Mechanismus der beide Ringe ueber einen gemeinsamen
// Pixel-Index-Raum (PIXEL_COUNT + RING2_PIXEL_COUNT) synchron gerendert
// hat - das war fehleranfaellig und schwer nachvollziehbar. Jetzt hat
// jeder Ring sein eigenes, in sich abgeschlossenes Standby-Twinkle.

static CRGB primaryLeds[PIXEL_COUNT];
static CRGB ring2Leds[RING2_PIXEL_COUNT];
static LedMode currentLedMode = LedMode::ALL_OFF;

void ledsInit() {
  FastLED.addLeds<WS2812B, LED_STRIP_PIN, GRB>(primaryLeds, PIXEL_COUNT);
  ring1Init(primaryLeds);
  if (RING2_ENABLED) {
    FastLED.addLeds<WS2812B, RING2_PIN, GRB>(ring2Leds, RING2_PIXEL_COUNT);
    ring2Init(ring2Leds);
  }
  // Strip braucht kurz Zeit die Versorgungsspannung zu stabilisieren,
  // bevor das erste Signal zuverlaessig ankommt.
  delay(300);
  FastLED.show();
  delay(50);
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

  bool ring1Changed = false;
  bool ring2Changed = false;

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

  if (RING2_ENABLED) {
    if (ring2ForceTestActive) {
      ring2Changed = ring2ForceTestService(now);
    } else {
      ring2Changed = ring2Service(now);
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
}

void ledMarkAllDirty() {
  ring1MarkDirty();
  if (RING2_ENABLED) ring2MarkDirty();
}

void ledClear() {
  ring1Clear();
  if (RING2_ENABLED) ring2Clear();
}

void ledShow() { FastLED.show(); }

void ledRing1DiagnosticStart() {
  ring1DiagnosticStart();
}
