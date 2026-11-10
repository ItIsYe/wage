#include "led_ring2_module.h"

#include <FastLED.h>
#include <math.h>

#include "config.h"
#include "led_gamma.h"
#include "types.h"

extern RuntimeConfig activeConfig;

static CRGB* ring2Leds = nullptr;
static bool ring2FrameDirty = true;
static uint8_t ring2BrightnessByte = 0;
static bool ring2BrightnessInitialized = false;
static uint32_t ring2TickMs = 0;
static uint8_t ring2PulseValue = 0;
static uint8_t ring2BreathingValue = 10;
static int8_t ring2BreathingStep = 3;
static uint8_t ring2IdleRunnerPos = 0;

static constexpr uint8_t RING2_MODE_OFF = 0;
static constexpr uint8_t RING2_MODE_SHORT_GREEN_FLASH = 1;
static constexpr uint8_t RING2_MODE_TIMING_STATIC_10 = 2;
static constexpr uint8_t RING2_MODE_WAIT_EMPTY_RED_BREATHE_5 = 3;
static constexpr uint8_t RING2_MODE_ERROR_SOLID_RED = 4;
static constexpr uint8_t RING2_MODE_IDLE_RING_CYAN_GREEN = 5;
static constexpr uint8_t RING2_MODE_STANDBY_TWINKLE = 6;
static State ring2State = State::BOOT_MSG;

// Standby-Sternenhimmel (ein Stern = ein physischer Pixel). Gleiche Einstellungen wie
// bei Ring 1 (Wechselintervall, Helligkeitsbereich, Saettigung); die Anzahl leuchtender
// Sterne ist fuer Ring 1 (PIXEL_GROUPS Gruppen) konfiguriert und wird hier im Verhaeltnis
// der Pixelzahlen umgerechnet.
struct Ring2Star { bool on; uint16_t hue; uint8_t value; };
static Ring2Star ring2Stars[RING2_PIXEL_COUNT];
static uint16_t ring2StarsOn = 0;
static uint32_t ring2TwinkleChangeAtMs = 0;
static uint32_t ring2TwinkleFrameAtMs = 0;

static inline bool ring2Ready() { return ring2Leds != nullptr; }

static inline CRGB scaleColor(const CRGB& color, uint8_t brightness) { CRGB out = color; out.nscale8_video(brightness); return out; }
static inline CRGB rgb(uint8_t r, uint8_t g, uint8_t b) { return CRGB(r, g, b); }
static inline uint8_t brightnessPercentToByte(uint8_t percent) {
  if (percent >= 100) return 255;
  return (uint8_t)(((uint16_t)percent * 255u + 50u) / 100u);
}
static inline void ring2SetBrightnessPercent(uint8_t percent) {
  const uint8_t target = brightnessPercentToByte(percent);
  if (ring2BrightnessInitialized && ring2BrightnessByte == target) return;
  ring2BrightnessByte = target;
  ring2BrightnessInitialized = true;
  ring2FrameDirty = true;
}

static const char* ring2ModeLabel(uint8_t mode) {
  if (mode == RING2_MODE_OFF) return "off";
  if (mode == RING2_MODE_SHORT_GREEN_FLASH) return "short-green-flash";
  if (mode == RING2_MODE_TIMING_STATIC_10) return "timing-static-10";
  if (mode == RING2_MODE_WAIT_EMPTY_RED_BREATHE_5) return "wait-empty-red-breathe-5";
  if (mode == RING2_MODE_ERROR_SOLID_RED) return "error-solid-red";
  if (mode == RING2_MODE_IDLE_RING_CYAN_GREEN) return "idle-ring-cyan-green";
  if (mode == RING2_MODE_STANDBY_TWINKLE) return "standby-twinkle";
  return "unknown";
}
static uint8_t ring2ResolveModeFromState() {
  switch (ring2State) {
    case State::BOOT_MSG: return RING2_MODE_OFF;
    case State::BOOT_TARE: return RING2_MODE_OFF;
    case State::IDLE_WAIT_GLASS: return RING2_MODE_IDLE_RING_CYAN_GREEN;
    case State::GLASS_DETECTED: return RING2_MODE_SHORT_GREEN_FLASH;
    case State::READY_FOR_TIMING: return RING2_MODE_OFF;
    case State::TIMING: return RING2_MODE_TIMING_STATIC_10;
    case State::SHOW_RESULT: return RING2_MODE_OFF;
    case State::WAIT_EMPTY_AFTER_RESULT: return RING2_MODE_WAIT_EMPTY_RED_BREATHE_5;
    case State::CHECK_RETARE: return RING2_MODE_OFF;
    case State::STANDBY: return RING2_MODE_STANDBY_TWINKLE;
    case State::ERROR_RECOVER: return RING2_MODE_ERROR_SOLID_RED;
  }
  return RING2_MODE_OFF;
}
static inline uint32_t ring2RandU32(uint32_t lo, uint32_t hi) {
  if (lo > hi) { const uint32_t t = lo; lo = hi; hi = t; }
  return lo + (uint32_t)random(0, (long)(hi - lo + 1U));
}
static inline uint8_t ring2RandU8(uint8_t lo, uint8_t hi) {
  if (lo > hi) { const uint8_t t = lo; lo = hi; hi = t; }
  return (uint8_t)(lo + (uint8_t)random(0, (int16_t)(hi - lo + 1U)));
}
static inline uint32_t ring2FrameMs() {
  const uint32_t f = activeConfig.standbyFrameMs;
  return f < 30U ? 30U : (f > 1000U ? 1000U : f);
}
// Anzahl leuchtender Sterne von Ring 1 (Gruppen) auf Ring 2 (Pixel) umrechnen.
// Aufgerundet, damit ein konfigurierter Wert > 0 nie auf 0 faellt.
static inline uint16_t ring2ScaleCount(uint8_t ring1Count) {
  const uint32_t scaled = ((uint32_t)ring1Count * RING2_PIXEL_COUNT + PIXEL_GROUPS - 1U) / PIXEL_GROUPS;
  return (uint16_t)(scaled > RING2_PIXEL_COUNT ? RING2_PIXEL_COUNT : scaled);
}
static inline CRGB ring2StarColor(const Ring2Star& st) {
  CHSV hsv((uint8_t)(st.hue >> 8), activeConfig.standbySaturation, st.value);
  CRGB out;
  hsv2rgb_rainbow(hsv, out);
  out.r = LedGamma::apply(out.r);
  out.g = LedGamma::apply(out.g);
  out.b = LedGamma::apply(out.b);
  return scaleColor(out, ring2BrightnessByte);
}
static void ring2TwinkleBounds(uint16_t& onMin, uint16_t& onMax, uint8_t& vMin, uint8_t& vMax) {
  onMax = ring2ScaleCount(activeConfig.standbyOnMax);
  onMin = ring2ScaleCount(activeConfig.standbyOnMin);
  if (onMin > onMax) onMin = onMax;
  vMax = activeConfig.standbyValueMax;
  vMin = activeConfig.standbyValueMin > vMax ? vMax : activeConfig.standbyValueMin;
}
static void ring2TwinkleInit(uint32_t now) {
  uint16_t onMin, onMax; uint8_t vMin, vMax;
  ring2TwinkleBounds(onMin, onMax, vMin, vMax);
  const uint32_t cMax = activeConfig.standbyChangeMaxMs;
  const uint32_t cMin = activeConfig.standbyChangeMinMs > cMax ? cMax : activeConfig.standbyChangeMinMs;
  ring2TwinkleFrameAtMs = now + ring2FrameMs();
  ring2TwinkleChangeAtMs = now + ring2RandU32(cMin, cMax);
  ring2StarsOn = 0;
  for (uint16_t i = 0; i < RING2_PIXEL_COUNT; ++i) {
    ring2Stars[i].on = false;
    ring2Stars[i].hue = (uint16_t)random(0, 65536);
    ring2Stars[i].value = ring2RandU8(vMin, vMax);
  }
  const uint16_t initialOn = (uint16_t)ring2RandU32(onMin, onMax);
  for (uint16_t n = 0; n < initialOn; ++n) {
    const uint16_t idx = (uint16_t)random(0, RING2_PIXEL_COUNT);
    if (!ring2Stars[idx].on) { ring2Stars[idx].on = true; ++ring2StarsOn; }
  }
}
// Rueckgabe: true, wenn sich das Bild geaendert hat.
static bool ring2TwinkleUpdate(uint32_t now) {
  bool changed = false;
  uint16_t onMin, onMax; uint8_t vMin, vMax;
  ring2TwinkleBounds(onMin, onMax, vMin, vMax);

  if ((int32_t)(now - ring2TwinkleChangeAtMs) >= 0) {
    const uint32_t cMax = activeConfig.standbyChangeMaxMs;
    const uint32_t cMin = activeConfig.standbyChangeMinMs > cMax ? cMax : activeConfig.standbyChangeMinMs;
    ring2TwinkleChangeAtMs = now + ring2RandU32(cMin, cMax);
    const bool needMore = ring2StarsOn < onMin;
    const bool needLess = ring2StarsOn > onMax;
    const bool toggle = !needMore && !needLess && (random(0, 100) < 45);
    if (needMore || needLess || toggle) {
      for (uint16_t tries = 0; tries < RING2_PIXEL_COUNT; ++tries) {
        const uint16_t i = (uint16_t)random(0, RING2_PIXEL_COUNT);
        const bool wantOn = needMore || (!needLess && !ring2Stars[i].on);
        if (wantOn && !ring2Stars[i].on) {
          if (!needMore && ring2StarsOn >= onMax) continue;  // Obergrenze einhalten
          ring2Stars[i].on = true; ++ring2StarsOn;
          ring2Stars[i].hue = (uint16_t)random(0, 65536);
          ring2Stars[i].value = ring2RandU8(vMin, vMax);
          changed = true; break;
        }
        if (!wantOn && ring2Stars[i].on) {
          if (!needLess && ring2StarsOn <= onMin) continue;  // Untergrenze einhalten
          ring2Stars[i].on = false; --ring2StarsOn;
          changed = true; break;
        }
      }
    }
  }

  if ((int32_t)(now - ring2TwinkleFrameAtMs) >= 0) {
    ring2TwinkleFrameAtMs = now + ring2FrameMs();
    for (uint16_t i = 0; i < RING2_PIXEL_COUNT; ++i) {
      if (!ring2Stars[i].on) continue;
      ring2Stars[i].hue = (uint16_t)(ring2Stars[i].hue + (int16_t)random(-2, 3));
      int16_t v = (int16_t)ring2Stars[i].value + (int16_t)random(-4, 5);
      if (v < vMin) v = vMin;
      if (v > vMax) v = vMax;
      ring2Stars[i].value = (uint8_t)v;
    }
    changed = true;
  }
  return changed;
}

static void ring2ResetPatternAnimation(uint8_t mode, uint32_t now) {
  if (mode == RING2_MODE_STANDBY_TWINKLE) {
    ring2TwinkleInit(now);
  }
  if (mode == RING2_MODE_SHORT_GREEN_FLASH) {
    ring2PulseValue = 1;
    ring2TickMs = now;
  }
  if (mode == RING2_MODE_WAIT_EMPTY_RED_BREATHE_5) {
    ring2BreathingValue = 10;
    ring2BreathingStep = 3;
    ring2TickMs = now;
  }
  if (mode == RING2_MODE_IDLE_RING_CYAN_GREEN) {
    ring2IdleRunnerPos = 0;
    ring2TickMs = now;
  }
}
void ring2LogWrite(uint32_t now) {
  (void)now;
}
static inline void ring2Fill(const CRGB& color) { if (!ring2Ready()) return; for (uint16_t i = 0; i < RING2_PIXEL_COUNT; ++i) ring2Leds[i] = scaleColor(color, ring2BrightnessByte); }

void ring2Init(CRGB* leds) {
  LedGamma::build();
  ring2Leds = leds;
  ring2SetBrightnessPercent(activeConfig.ring2BrightnessPercent);
  if (RING2_BOOT_TEST) {
    for (uint16_t i = 0; i < RING2_PIXEL_COUNT; ++i) ring2Leds[i] = scaleColor(rgb(80, 80, 80), ring2BrightnessByte);
    ring2FrameDirty = false;
    if (MASTER_DEBUG_LOG) Serial.printf("[RING2] boot test GPIO=%u pixels=%u\n", (unsigned)RING2_PIN, (unsigned)RING2_PIXEL_COUNT);
  } else {
    ring2Clear();
    ring2FrameDirty = true;
  }
}

bool ring2Service(uint32_t now) {
  if (!ring2Ready()) {
    if (MASTER_DEBUG_LOG) {
      static uint32_t lastNotReadyLogMs = 0;
      if (now - lastNotReadyLogMs >= 2000) {
        lastNotReadyLogMs = now;
        Serial.println("[RING2 ERROR] ring2Leds not initialized");
      }
    }
    return false;
  }
  const uint8_t resolvedMode = ring2ResolveModeFromState();
  if (RING2_BOOT_TEST) return false;

  static uint8_t lastRenderedState = 255;
  auto logRenderedState = [&](uint8_t state, const char* label) { if (!MASTER_DEBUG_LOG) return; if (lastRenderedState != state) { Serial.printf("[RING2 PATTERN] state=%s mode=%u (%s) debug=%u enabled=%u \n", label,(unsigned)resolvedMode,ring2ModeLabel(resolvedMode),(unsigned)activeConfig.ring2DebugAllOn,(unsigned)activeConfig.ring2Enabled); lastRenderedState = state; } };
  static bool lastRing2Enabled = false; static uint8_t lastResolvedMode = 255; static bool lastRing2DebugAllOn = false; static uint8_t lastRing2BrightnessPercent = 255; static uint8_t lastRing2StandbyBrightnessPercent = 255;
  if (lastRing2Enabled != activeConfig.ring2Enabled || lastResolvedMode != resolvedMode || lastRing2DebugAllOn != activeConfig.ring2DebugAllOn || lastRing2BrightnessPercent != activeConfig.ring2BrightnessPercent || lastRing2StandbyBrightnessPercent != activeConfig.ring2StandbyBrightnessPercent) {
    ring2FrameDirty = true;
    if (MASTER_DEBUG_LOG && lastResolvedMode != resolvedMode) {
      Serial.printf("[RING2 MODE] %s\n", ring2ModeLabel(resolvedMode));
    }
    if (lastResolvedMode != resolvedMode) ring2ResetPatternAnimation(resolvedMode, now);
    lastRing2Enabled = activeConfig.ring2Enabled; lastResolvedMode = resolvedMode; lastRing2DebugAllOn = activeConfig.ring2DebugAllOn; lastRing2BrightnessPercent = activeConfig.ring2BrightnessPercent; lastRing2StandbyBrightnessPercent = activeConfig.ring2StandbyBrightnessPercent;
  }
  if (!activeConfig.ring2Enabled) { logRenderedState(0, "off"); if (ring2FrameDirty) { ring2Clear(); ring2FrameDirty = false; return true; } return false; }
  if (activeConfig.ring2DebugAllOn) {
    logRenderedState(250, "debug-white-all-on");
    ring2SetBrightnessPercent(activeConfig.ring2BrightnessPercent);
    if (ring2FrameDirty) {
      // Bewusst neutrales Weiß für Hardware-/Verdrahtungsprüfung, nicht für Normalbetrieb.
      ring2Fill(rgb(80, 80, 80));
      ring2FrameDirty = false;
      return true;
    }
    return false;
  }

  const uint8_t mode = resolvedMode;
  // Helligkeit genau einmal pro Durchlauf setzen. Zwei Modi haben feste Werte.
  // Frueher wurde hier erst der Config-Wert und im Modus-Zweig erneut 10 % bzw.
  // 5 % gesetzt - jeder Wechsel markiert den Frame als geaendert, wodurch in
  // TIMING und WAIT_EMPTY in jedem Loop-Durchlauf FastLED.show() lief.
  uint8_t modeBrightnessPercent = activeConfig.ring2BrightnessPercent;
  if (mode == RING2_MODE_TIMING_STATIC_10) modeBrightnessPercent = 10;
  else if (mode == RING2_MODE_WAIT_EMPTY_RED_BREATHE_5) modeBrightnessPercent = 5;
  else if (mode == RING2_MODE_STANDBY_TWINKLE) modeBrightnessPercent = activeConfig.ring2StandbyBrightnessPercent;
  ring2SetBrightnessPercent(modeBrightnessPercent);
  if (mode == RING2_MODE_OFF) {
    logRenderedState(mode, "off");
    if (ring2FrameDirty) { ring2Clear(); ring2FrameDirty = false; return true; }
    return false;
  }

  if (mode == RING2_MODE_IDLE_RING_CYAN_GREEN) {
    logRenderedState(RING2_MODE_IDLE_RING_CYAN_GREEN, "idle-ring-cyan-green");
    if (now - ring2TickMs >= 90) {
      ring2TickMs = now;
      ring2IdleRunnerPos = (uint8_t)((ring2IdleRunnerPos + 1) % RING2_PIXEL_COUNT);
      ring2FrameDirty = true;
    }
    if (ring2FrameDirty) {
      ring2Clear();
      for (uint16_t i = 0; i < RING2_PIXEL_COUNT; ++i) {
        const uint8_t distance = (uint8_t)((i + RING2_PIXEL_COUNT - ring2IdleRunnerPos) % RING2_PIXEL_COUNT);
        if (distance == 0) ring2Leds[i] = scaleColor(rgb(0, 70, 45), ring2BrightnessByte);
        else if (distance == 1 || distance == RING2_PIXEL_COUNT - 1) ring2Leds[i] = scaleColor(rgb(0, 35, 60), ring2BrightnessByte);
      }
      ring2FrameDirty = false;
      return true;
    }
    return false;
  }

  if (mode == RING2_MODE_STANDBY_TWINKLE) {
    logRenderedState(RING2_MODE_STANDBY_TWINKLE, "standby-twinkle");
    if (ring2TwinkleUpdate(now)) ring2FrameDirty = true;
    if (ring2FrameDirty) {
      ring2Clear();
      for (uint16_t i = 0; i < RING2_PIXEL_COUNT; ++i) {
        if (ring2Stars[i].on) ring2Leds[i] = ring2StarColor(ring2Stars[i]);
      }
      ring2FrameDirty = false;
      return true;
    }
    return false;
  }

  if (mode == RING2_MODE_SHORT_GREEN_FLASH) {
    logRenderedState(RING2_MODE_SHORT_GREEN_FLASH, "short-green-flash");
    if (ring2PulseValue != 0 && now - ring2TickMs >= 120) {
      ring2PulseValue = 0;
      ring2FrameDirty = true;
    }
    if (ring2FrameDirty) {
      if (ring2PulseValue != 0) ring2Fill(rgb(0, 80, 0));
      else ring2Clear();
      ring2FrameDirty = false;
      return true;
    }
    return false;
  }

  if (mode == RING2_MODE_TIMING_STATIC_10) {
    logRenderedState(RING2_MODE_TIMING_STATIC_10, "timing-static-10");
    if (ring2FrameDirty) { ring2Fill(rgb(64, 80, 80)); ring2FrameDirty = false; return true; }
    return false;
  }

  if (mode == RING2_MODE_WAIT_EMPTY_RED_BREATHE_5) {
    logRenderedState(RING2_MODE_WAIT_EMPTY_RED_BREATHE_5, "wait-empty-red-breathe-5");
    if (now - ring2TickMs >= 110) {
      ring2TickMs = now;
      const int16_t next = (int16_t)ring2BreathingValue + ring2BreathingStep;
      if (next >= 60 || next <= 8) ring2BreathingStep = -ring2BreathingStep;
      ring2BreathingValue = (uint8_t)((int16_t)ring2BreathingValue + ring2BreathingStep);
      ring2Fill(rgb(ring2BreathingValue, 0, 0));
      ring2FrameDirty = true;
    }
    if (ring2FrameDirty) { ring2FrameDirty = false; return true; }
    return false;
  }

  if (mode == RING2_MODE_ERROR_SOLID_RED) {
    logRenderedState(RING2_MODE_ERROR_SOLID_RED, "error-solid-red");
    if (ring2FrameDirty) { ring2Fill(rgb(90, 0, 0)); ring2FrameDirty = false; return true; }
    return false;
  }

  logRenderedState(251, "unknown-off");
  if (ring2FrameDirty) {
    ring2Clear();
    ring2FrameDirty = false;
    return true;
  }
  return false;
}
void ring2MarkDirty() { ring2FrameDirty = true; }
bool ring2IsStandbyState() { return ring2State == State::STANDBY; }
void ring2SetState(State s) {
  if (ring2State == s) return;
  if (MASTER_DEBUG_LOG) Serial.printf("[RING2 STATE] %u -> %u\n", (unsigned)ring2State, (unsigned)s);
  ring2State = s;
  ring2FrameDirty = true;
}
void ring2Clear() { if (!ring2Ready()) return; fill_solid(ring2Leds, RING2_PIXEL_COUNT, CRGB::Black); }
bool ring2ForceTestService(uint32_t now) {
  if (!ring2Ready()) {
    if (MASTER_DEBUG_LOG) {
      static uint32_t lastNotReadyLogMs = 0;
      if (now - lastNotReadyLogMs >= 2000) {
        lastNotReadyLogMs = now;
        Serial.println("[RING2 ERROR] ring2Leds not initialized");
      }
    }
    return false;
  }
  static uint32_t lastInlineWriteMs = 0;
  if (now - lastInlineWriteMs < 250) return false;
  lastInlineWriteMs = now;
  ring2BrightnessByte = 255;
  ring2Clear();
  if (RING2_PIXEL_COUNT > 0) ring2Leds[0] = rgb(255, 0, 0);
  if (RING2_PIXEL_COUNT > 1) ring2Leds[1] = rgb(0, 255, 0);
  if (RING2_PIXEL_COUNT > 2) ring2Leds[2] = rgb(0, 0, 255);
  if (RING2_PIXEL_COUNT > 3) ring2Leds[3] = rgb(255, 255, 255);
  if (MASTER_DEBUG_LOG) Serial.printf("[RING2 FASTLED TEST] wrote RGBW pin=%u pixels=%u\n", (unsigned)RING2_PIN, (unsigned)RING2_PIXEL_COUNT);
  return true;
}
