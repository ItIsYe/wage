#include "led_ring2_module.h"

#include <FastLED.h>
#include <math.h>

#include "config.h"
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
static State ring2State = State::BOOT_MSG;

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
    case State::STANDBY: return RING2_MODE_OFF;
    case State::ERROR_RECOVER: return RING2_MODE_ERROR_SOLID_RED;
  }
  return RING2_MODE_OFF;
}
static void ring2ResetPatternAnimation(uint8_t mode, uint32_t now) {
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
void ring2LogWrite(uint32_t now) { if (!MASTER_DEBUG_LOG) return; static uint32_t lastWriteLogMs = 0; if (now - lastWriteLogMs >= 2000) { lastWriteLogMs = now; Serial.println("[RING2 WRITE] show applied"); } }
static inline void ring2Fill(const CRGB& color) { if (!ring2Ready()) return; for (uint16_t i = 0; i < RING2_PIXEL_COUNT; ++i) ring2Leds[i] = scaleColor(color, ring2BrightnessByte); }

void ring2Init(CRGB* leds) {
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
  if (MASTER_DEBUG_LOG) { static uint32_t lastHardEntryLogMs = 0; if (now - lastHardEntryLogMs >= 1000) { lastHardEntryLogMs = now; Serial.printf("[RING2 HARD ENTRY] now=%lu enabled=%u mode=%u debug=%u state=%u b=%u sb=%u pin=%u\n", (unsigned long)now,(unsigned)activeConfig.ring2Enabled,(unsigned)resolvedMode,(unsigned)activeConfig.ring2DebugAllOn,(unsigned)ring2State,(unsigned)activeConfig.ring2BrightnessPercent,(unsigned)activeConfig.ring2StandbyBrightnessPercent,(unsigned)RING2_PIN); }}
  static uint32_t lastEntryLogMs = 0;
  if (MASTER_DEBUG_LOG && millis() - lastEntryLogMs >= 2000) { lastEntryLogMs = millis(); Serial.printf("[RING2 SERVICE] now=%lu enabled=%u mode=%u debug=%u brightness=%u standbyBrightness=%u pin=%u\n", (unsigned long)now,(unsigned)activeConfig.ring2Enabled,(unsigned)resolvedMode,(unsigned)activeConfig.ring2DebugAllOn,(unsigned)activeConfig.ring2BrightnessPercent,(unsigned)activeConfig.ring2StandbyBrightnessPercent,(unsigned)RING2_PIN); }
  if (RING2_BOOT_TEST) return false;

  static uint8_t lastRenderedState = 255;
  auto logRenderedState = [&](uint8_t state, const char* label) { if (!MASTER_DEBUG_LOG) return; if (lastRenderedState != state) { Serial.printf("[RING2 PATTERN] state=%s mode=%u (%s) debug=%u enabled=%u \n", label,(unsigned)resolvedMode,ring2ModeLabel(resolvedMode),(unsigned)activeConfig.ring2DebugAllOn,(unsigned)activeConfig.ring2Enabled); lastRenderedState = state; } };
  static bool lastRing2Enabled = false; static uint8_t lastResolvedMode = 255; static bool lastRing2DebugAllOn = false; static uint8_t lastRing2BrightnessPercent = 255; static uint8_t lastRing2StandbyBrightnessPercent = 255;
  if (lastRing2Enabled != activeConfig.ring2Enabled || lastResolvedMode != resolvedMode || lastRing2DebugAllOn != activeConfig.ring2DebugAllOn || lastRing2BrightnessPercent != activeConfig.ring2BrightnessPercent || lastRing2StandbyBrightnessPercent != activeConfig.ring2StandbyBrightnessPercent) {
    ring2FrameDirty = true;
    if (MASTER_DEBUG_LOG) Serial.printf("[RING2 CONFIG] pin=%u enabled=%u mode=%u (%s) debug=%u brightness=%u standbyBrightness=%u\n", (unsigned)RING2_PIN,(unsigned)activeConfig.ring2Enabled,(unsigned)resolvedMode,ring2ModeLabel(resolvedMode),(unsigned)activeConfig.ring2DebugAllOn,(unsigned)activeConfig.ring2BrightnessPercent,(unsigned)activeConfig.ring2StandbyBrightnessPercent);
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
  ring2SetBrightnessPercent(activeConfig.ring2BrightnessPercent);
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
    ring2SetBrightnessPercent(10);
    if (ring2FrameDirty) { ring2Fill(rgb(64, 80, 80)); ring2FrameDirty = false; return true; }
    return false;
  }

  if (mode == RING2_MODE_WAIT_EMPTY_RED_BREATHE_5) {
    logRenderedState(RING2_MODE_WAIT_EMPTY_RED_BREATHE_5, "wait-empty-red-breathe-5");
    ring2SetBrightnessPercent(5);
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
void ring2ApplySharedStandby(const bool* on, const uint16_t* hue, const uint8_t* value, uint16_t count) {
  if (!ring2Ready()) return;
  ring2SetBrightnessPercent(activeConfig.ring2StandbyBrightnessPercent);
  ring2Clear();
  const uint16_t base = PIXEL_COUNT;
  for (uint16_t i = 0; i < RING2_PIXEL_COUNT; ++i) {
    const uint16_t idx = base + i;
    if (idx >= count || !on[idx]) continue;
    CHSV hsv((uint8_t)(hue[idx] >> 8), activeConfig.standbySaturation, value[idx]);
    CRGB c;
    hsv2rgb_rainbow(hsv, c);
    const float nr = c.r / 255.0f;
    const float ng = c.g / 255.0f;
    const float nb = c.b / 255.0f;
    c.r = (uint8_t)(powf(nr, 2.8f) * 255.0f + 0.5f);
    c.g = (uint8_t)(powf(ng, 2.8f) * 255.0f + 0.5f);
    c.b = (uint8_t)(powf(nb, 2.8f) * 255.0f + 0.5f);
    ring2Leds[i] = scaleColor(c, ring2BrightnessByte);
  }
}
