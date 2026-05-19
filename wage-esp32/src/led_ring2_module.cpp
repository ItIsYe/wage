#include "led_ring2_module.h"

#include <FastLED.h>

#include "config.h"
#include "types.h"

extern RuntimeConfig activeConfig;

static CRGB* ring2Leds = nullptr;
static bool ring2FrameDirty = true;
static uint8_t ring2BrightnessByte = 0;
static bool ring2BrightnessInitialized = false;
static uint32_t ring2TickMs = 0;
static uint8_t ring2PulseValue = 10;
static int8_t ring2PulseStep = 5;
static uint8_t ring2SpinnerHead = 0;
static uint8_t ring2BreathingValue = 10;
static int8_t ring2BreathingStep = 3;
static State ring2State = State::BOOT_MSG;

static inline bool ring2Ready() { return ring2Leds != nullptr; }

static inline CRGB scaleColor(const CRGB& color, uint8_t brightness) { CRGB out = color; out.nscale8_video(brightness); return out; }
static inline CRGB rgb(uint8_t r, uint8_t g, uint8_t b) { return CRGB(r, g, b); }
static inline uint8_t brightnessPercentToByte(uint8_t percent) { if (percent >= 100) return 255; return (uint8_t)((uint16_t)percent * 255u / 100u); }
static inline void ring2SetBrightnessPercent(uint8_t percent) {
  const uint8_t target = brightnessPercentToByte(percent);
  if (ring2BrightnessInitialized && ring2BrightnessByte == target) return;
  ring2BrightnessByte = target;
  ring2BrightnessInitialized = true;
  ring2FrameDirty = true;
}

static const char* ring2ModeLabel(uint8_t mode) {
  if (mode == 0) return "off";
  if (mode == 1) return "solid-blue";
  if (mode == 2) return "pulse-blue";
  if (mode == 3) return "breathing-white";
  if (mode == 4) return "slow-blue-spinner";
  return "unknown";
}
static uint8_t ring2ResolveModeFromState() {
  switch (ring2State) {
    case State::BOOT_MSG: return 1;
    case State::BOOT_TARE: return 1;
    case State::IDLE_WAIT_GLASS: return 3;
    case State::GLASS_DETECTED: return 1;
    case State::READY_FOR_TIMING: return 2;
    case State::TIMING: return 4;
    case State::SHOW_RESULT: return 2;
    case State::WAIT_EMPTY_AFTER_RESULT: return 1;
    case State::CHECK_RETARE: return 1;
    case State::STANDBY: return 3;
    case State::ERROR_RECOVER: return 0;
  }
  return 0;
}
static void ring2ResetPatternAnimation(uint8_t mode, uint32_t now) {
  if (mode == 2) {
    ring2PulseValue = 10;
    ring2PulseStep = 5;
    ring2TickMs = now;
  }
  if (mode == 3) {
    ring2BreathingValue = 10;
    ring2BreathingStep = 3;
    ring2TickMs = now;
  }
  if (mode == 4) {
    ring2SpinnerHead = 0;
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
  const uint8_t resolvedMode = activeConfig.ring2FollowState ? ring2ResolveModeFromState() : activeConfig.ring2PatternMode;
  if (MASTER_DEBUG_LOG) { static uint32_t lastHardEntryLogMs = 0; if (now - lastHardEntryLogMs >= 1000) { lastHardEntryLogMs = now; Serial.printf("[RING2 HARD ENTRY] now=%lu enabled=%u mode=%u debug=%u follow=%u state=%u b=%u sb=%u pin=%u\n", (unsigned long)now,(unsigned)activeConfig.ring2Enabled,(unsigned)resolvedMode,(unsigned)activeConfig.ring2DebugAllOn,(unsigned)activeConfig.ring2FollowState,(unsigned)ring2State,(unsigned)activeConfig.ring2BrightnessPercent,(unsigned)activeConfig.ring2StandbyBrightnessPercent,(unsigned)RING2_PIN); }}
  static uint32_t lastEntryLogMs = 0;
  if (MASTER_DEBUG_LOG && millis() - lastEntryLogMs >= 2000) { lastEntryLogMs = millis(); Serial.printf("[RING2 SERVICE] now=%lu enabled=%u mode=%u debug=%u follow=%u brightness=%u standbyBrightness=%u pin=%u\n", (unsigned long)now,(unsigned)activeConfig.ring2Enabled,(unsigned)resolvedMode,(unsigned)activeConfig.ring2DebugAllOn,(unsigned)activeConfig.ring2FollowState,(unsigned)activeConfig.ring2BrightnessPercent,(unsigned)activeConfig.ring2StandbyBrightnessPercent,(unsigned)RING2_PIN); }
  if (RING2_BOOT_TEST) return false;

  static uint8_t lastRenderedState = 255;
  auto logRenderedState = [&](uint8_t state, const char* label) { if (!MASTER_DEBUG_LOG) return; if (lastRenderedState != state) { Serial.printf("[RING2 PATTERN] state=%s mode=%u (%s) debug=%u enabled=%u follow=%u\n", label,(unsigned)resolvedMode,ring2ModeLabel(resolvedMode),(unsigned)activeConfig.ring2DebugAllOn,(unsigned)activeConfig.ring2Enabled,(unsigned)activeConfig.ring2FollowState); lastRenderedState = state; } };
  static bool lastRing2Enabled = false; static uint8_t lastResolvedMode = 255; static bool lastRing2DebugAllOn = false; static bool lastRing2FollowState = false; static uint8_t lastRing2BrightnessPercent = 255; static uint8_t lastRing2StandbyBrightnessPercent = 255;
  if (lastRing2Enabled != activeConfig.ring2Enabled || lastResolvedMode != resolvedMode || lastRing2DebugAllOn != activeConfig.ring2DebugAllOn || lastRing2FollowState != activeConfig.ring2FollowState || lastRing2BrightnessPercent != activeConfig.ring2BrightnessPercent || lastRing2StandbyBrightnessPercent != activeConfig.ring2StandbyBrightnessPercent) {
    ring2FrameDirty = true;
    if (MASTER_DEBUG_LOG) Serial.printf("[RING2 CONFIG] pin=%u enabled=%u mode=%u (%s) debug=%u follow=%u brightness=%u standbyBrightness=%u\n", (unsigned)RING2_PIN,(unsigned)activeConfig.ring2Enabled,(unsigned)resolvedMode,ring2ModeLabel(resolvedMode),(unsigned)activeConfig.ring2DebugAllOn,(unsigned)activeConfig.ring2FollowState,(unsigned)activeConfig.ring2BrightnessPercent,(unsigned)activeConfig.ring2StandbyBrightnessPercent);
    if (lastResolvedMode != resolvedMode) ring2ResetPatternAnimation(resolvedMode, now);
    lastRing2Enabled = activeConfig.ring2Enabled; lastResolvedMode = resolvedMode; lastRing2DebugAllOn = activeConfig.ring2DebugAllOn; lastRing2FollowState = activeConfig.ring2FollowState; lastRing2BrightnessPercent = activeConfig.ring2BrightnessPercent; lastRing2StandbyBrightnessPercent = activeConfig.ring2StandbyBrightnessPercent;
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
  if (mode == 2) ring2SetBrightnessPercent(activeConfig.ring2StandbyBrightnessPercent); else ring2SetBrightnessPercent(activeConfig.ring2BrightnessPercent);
  if (mode == 0) { logRenderedState(0, "off"); if (ring2FrameDirty) { ring2Clear(); ring2FrameDirty = false; return true; } return false; }
  if (mode == 1) { logRenderedState(1, "solid-blue"); if (ring2FrameDirty) { ring2Fill(rgb(0, 0, 64)); ring2FrameDirty = false; return true; } return false; }
  if (mode == 3) {
    logRenderedState(3, "breathing-white");
    if (now - ring2TickMs >= 80) {
      ring2TickMs = now;
      const int16_t next = (int16_t)ring2BreathingValue + ring2BreathingStep;
      if (next >= 140 || next <= 10) ring2BreathingStep = -ring2BreathingStep;
      ring2BreathingValue = (uint8_t)((int16_t)ring2BreathingValue + ring2BreathingStep);
      ring2Fill(rgb(ring2BreathingValue, ring2BreathingValue, ring2BreathingValue));
      ring2FrameDirty = true;
    }
    if (ring2FrameDirty) { ring2FrameDirty = false; return true; }
    return false;
  }
  if (mode == 4) {
    logRenderedState(4, "slow-blue-spinner");
    if (now - ring2TickMs >= 180) {
      ring2TickMs = now;
      ring2SpinnerHead = (uint8_t)((ring2SpinnerHead + 1) % RING2_PIXEL_COUNT);
      ring2FrameDirty = true;
    }
    if (ring2FrameDirty) {
      ring2Clear();
      ring2Leds[ring2SpinnerHead] = scaleColor(rgb(0, 0, 90), ring2BrightnessByte);
      if (RING2_PIXEL_COUNT > 1) ring2Leds[(ring2SpinnerHead + RING2_PIXEL_COUNT - 1) % RING2_PIXEL_COUNT] = scaleColor(rgb(0, 0, 25), ring2BrightnessByte);
      ring2FrameDirty = false;
      return true;
    }
    return false;
  }

  if (mode == 2) {
    logRenderedState(2, "pulse-blue");
    static uint32_t lastHeartbeatLogMs = 0;
    if (MASTER_DEBUG_LOG && now - lastHeartbeatLogMs >= 2000) { lastHeartbeatLogMs = now; Serial.printf("[RING2 HEARTBEAT] now=%lu mode=%u (%s) pulse=%u frameDirty=%u\n", (unsigned long)now,(unsigned)mode,ring2ModeLabel(mode),(unsigned)ring2PulseValue,(unsigned)ring2FrameDirty); }
    if (now - ring2TickMs >= 80) { ring2TickMs = now; const int16_t next = (int16_t)ring2PulseValue + ring2PulseStep; if (next >= 120 || next <= 10) ring2PulseStep = -ring2PulseStep; ring2PulseValue = (uint8_t)((int16_t)ring2PulseValue + ring2PulseStep); ring2Fill(rgb(0, 0, ring2PulseValue)); ring2FrameDirty = true; }
    if (ring2FrameDirty) { ring2FrameDirty = false; return true; }
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
