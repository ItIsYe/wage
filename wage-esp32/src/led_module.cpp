#include "led_module.h"

#include <Adafruit_NeoPixel.h>

#include "config.h"
#include "types.h"

extern RuntimeConfig activeConfig;

// Ring 1 / Haupt-LED-Ring
static Adafruit_NeoPixel ledStrip(PIXEL_COUNT, LED_STRIP_PIN, NEO_GRB + NEO_KHZ800);
static LedRingContext primaryLedRing{&ledStrip, PIXEL_COUNT};
#if RING2_ENABLED
// Ring 2 / Zusatz-LED-Ring
static Adafruit_NeoPixel ring2Strip(RING2_PIXEL_COUNT, RING2_PIN, NEO_GRB + NEO_KHZ800);
static LedRingContext secondaryLedRing{&ring2Strip, RING2_PIXEL_COUNT};
#endif

static LedMode ledMode = LedMode::ALL_OFF;
static uint32_t ledTickMs = 0;
static bool ledFlip = false;
static uint32_t twinkleNextMs = 0;
static uint32_t standbyFrameNextMs = 0;
static uint8_t ledSpinIdx = 0;
static bool ledFrameDirty = true;

static bool standbyTwinkleOn[PIXEL_COUNT] = {};
static uint8_t standbyOnCount = 0;
static uint8_t currentBrightnessByte = 0;
static bool brightnessInitialized = false;
static uint16_t standbyHue[PIXEL_COUNT] = {};
static uint8_t standbyValue[PIXEL_COUNT] = {};
static uint32_t colorGreen = 0;
static uint32_t colorBlue = 0;
static uint32_t colorRed = 0;
static uint32_t colorCyan = 0;
#if RING2_ENABLED
static bool ring2FrameDirty = true;
static uint8_t ring2BrightnessByte = 0;
static bool ring2BrightnessInitialized = false;
static uint32_t ring2TickMs = 0;
static uint8_t ring2PulseValue = 10;
static int8_t ring2PulseStep = 5;
#endif

static inline uint8_t brightnessPercentToByte(uint8_t percent) {
  if (percent >= 100) return 255;
  return (uint8_t)((uint16_t)percent * 255u / 100u);
}

// Basis-Helfer Ring 1
static inline void pixelsClear() { primaryLedRing.strip->clear(); }
static inline void pixelsShow() { primaryLedRing.strip->show(); }

static inline void setStripBrightnessPercent(uint8_t percent) {
  const uint8_t target = brightnessPercentToByte(percent);
  if (brightnessInitialized && currentBrightnessByte == target) return;
  primaryLedRing.strip->setBrightness(target);
  currentBrightnessByte = target;
  brightnessInitialized = true;
}

#if RING2_ENABLED
// Basis-Helfer Ring 2
static inline void ring2SetBrightnessPercent(uint8_t percent) {
  const uint8_t target = brightnessPercentToByte(percent);
  if (ring2BrightnessInitialized && ring2BrightnessByte == target) return;
  secondaryLedRing.strip->setBrightness(target);
  ring2BrightnessByte = target;
  ring2BrightnessInitialized = true;
  ring2FrameDirty = true;
}

static inline void ring2Clear() { secondaryLedRing.strip->clear(); }
static inline void ring2Fill(uint32_t color) {
  for (uint16_t i = 0; i < RING2_PIXEL_COUNT; ++i) secondaryLedRing.strip->setPixelColor(i, color);
}

static void ring2Service(uint32_t now) {
  static bool ring2AllOnApplied = false;
  static bool lastRing2Enabled = false;
  static uint8_t lastRing2PatternMode = 255;
  static bool lastRing2DebugAllOn = false;
  static uint8_t lastRing2BrightnessPercent = 255;
  static uint8_t lastRing2StandbyBrightnessPercent = 255;

  if (lastRing2Enabled != activeConfig.ring2Enabled ||
      lastRing2PatternMode != activeConfig.ring2PatternMode ||
      lastRing2DebugAllOn != activeConfig.ring2DebugAllOn ||
      lastRing2BrightnessPercent != activeConfig.ring2BrightnessPercent ||
      lastRing2StandbyBrightnessPercent != activeConfig.ring2StandbyBrightnessPercent) {
    ring2FrameDirty = true;
    if (MASTER_DEBUG_LOG) {
      Serial.printf("[RING2] refresh en=%u mode=%u dbg=%u b=%u sb=%u\n",
                    (unsigned)activeConfig.ring2Enabled,
                    (unsigned)activeConfig.ring2PatternMode,
                    (unsigned)activeConfig.ring2DebugAllOn,
                    (unsigned)activeConfig.ring2BrightnessPercent,
                    (unsigned)activeConfig.ring2StandbyBrightnessPercent);
    }
    if (lastRing2PatternMode != activeConfig.ring2PatternMode && activeConfig.ring2PatternMode == 2) {
      ring2PulseValue = 10;
      ring2PulseStep = 5;
      ring2TickMs = now;
    }
    lastRing2Enabled = activeConfig.ring2Enabled;
    lastRing2PatternMode = activeConfig.ring2PatternMode;
    lastRing2DebugAllOn = activeConfig.ring2DebugAllOn;
    lastRing2BrightnessPercent = activeConfig.ring2BrightnessPercent;
    lastRing2StandbyBrightnessPercent = activeConfig.ring2StandbyBrightnessPercent;
  }

  if (!activeConfig.ring2Enabled) {
    ring2AllOnApplied = false;
    if (ring2FrameDirty) { ring2Clear(); secondaryLedRing.strip->show(); ring2FrameDirty = false; }
    return;
  }
  if (activeConfig.ring2DebugAllOn) {
    ring2SetBrightnessPercent(activeConfig.ring2BrightnessPercent);
    if (!ring2AllOnApplied || ring2FrameDirty) {
      ring2Fill(secondaryLedRing.strip->Color(80, 80, 80));
      secondaryLedRing.strip->show();
      ring2AllOnApplied = true;
      ring2FrameDirty = false;
    }
    return;
  }
  ring2AllOnApplied = false;

  const uint8_t mode = activeConfig.ring2PatternMode;
  const uint8_t brightnessPercent = (mode == 2) ? activeConfig.ring2StandbyBrightnessPercent : activeConfig.ring2BrightnessPercent;
  ring2SetBrightnessPercent(brightnessPercent);
  if (mode == 0) {
    if (ring2FrameDirty) ring2Clear();
  } else if (mode == 1) {
    if (ring2FrameDirty) ring2Fill(secondaryLedRing.strip->Color(0, 0, 64));
  } else {
    if (now - ring2TickMs >= 80) {
      ring2TickMs = now;
      const int16_t next = (int16_t)ring2PulseValue + ring2PulseStep;
      if (next >= 120 || next <= 10) ring2PulseStep = -ring2PulseStep;
      ring2PulseValue = (uint8_t)((int16_t)ring2PulseValue + ring2PulseStep);
      ring2Fill(secondaryLedRing.strip->Color(0, 0, ring2PulseValue));
      ring2FrameDirty = true;
    }
  }
  if (ring2FrameDirty) { secondaryLedRing.strip->show(); ring2FrameDirty = false; }
}
#else
static void ring2Service(uint32_t) {}
#endif

static inline void applyBrightnessForLedModeInternal() {
  const uint8_t percent = (ledMode == LedMode::STANDBY_TWINKLE)
    ? activeConfig.standbyBrightnessPercent
    : activeConfig.pixelBrightnessPercent;
  setStripBrightnessPercent(percent);
}

static inline void pixelsFill(uint32_t color) {
  for (uint16_t i = 0; i < PIXEL_COUNT; ++i) primaryLedRing.strip->setPixelColor(i, color);
}

static inline void pixelsSet(const uint8_t* indices, uint8_t count, uint32_t color) {
  for (uint8_t i = 0; i < count; ++i) {
    if (indices[i] < PIXEL_COUNT) primaryLedRing.strip->setPixelColor(indices[i], color);
  }
}

static constexpr uint8_t ALT_PATTERN_A[] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24};
static constexpr uint8_t ALT_PATTERN_B[] = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23};

static void standbyApplyOutputs() {
  pixelsClear();
  for (uint8_t i = 0; i < PIXEL_COUNT; ++i) {
    if (standbyTwinkleOn[i]) {
      const uint32_t raw = primaryLedRing.strip->ColorHSV(standbyHue[i], STANDBY_SATURATION, standbyValue[i]);
      primaryLedRing.strip->setPixelColor(i, primaryLedRing.strip->gamma32(raw));
    }
  }
}

void ledsInit() {
  primaryLedRing.strip->begin();
  setStripBrightnessPercent(activeConfig.pixelBrightnessPercent);
  colorGreen = primaryLedRing.strip->Color(0, 90, 0);
  colorBlue = primaryLedRing.strip->Color(0, 0, 100);
  colorRed = primaryLedRing.strip->Color(100, 0, 0);
  colorCyan = primaryLedRing.strip->Color(0, 90, 90);
  pixelsClear();
  pixelsShow();
  ledFrameDirty = false;
  #if RING2_ENABLED
  secondaryLedRing.strip->begin();
  ring2SetBrightnessPercent(activeConfig.ring2BrightnessPercent);
  ring2Clear();
  secondaryLedRing.strip->show();
  ring2FrameDirty = true;
  if (MASTER_DEBUG_LOG) Serial.println("[RING2] init done, frame dirty");
  #endif
}

void ledsSetMode(LedMode m) {
  if (ledMode == m) return;
  ledMode = m;
  const uint32_t now = millis();
  ledTickMs = now;
  ledFlip = false;
  if (m == LedMode::READY_GREEN_BLINK) ledFlip = true;
  ledSpinIdx = 0;
  ledFrameDirty = true;
  if (m == LedMode::STANDBY_TWINKLE) {
    standbyFrameNextMs = now + STANDBY_FRAME_MS;
    twinkleNextMs = now + random(STANDBY_CHANGE_MIN_MS, STANDBY_CHANGE_MAX_MS);
    standbyOnCount = 0;
    for (uint8_t i = 0; i < PIXEL_COUNT; i++) {
      standbyTwinkleOn[i] = false;
      standbyHue[i] = (uint16_t)random(0, 65536);
      standbyValue[i] = (uint8_t)random(STANDBY_VALUE_MIN, STANDBY_VALUE_MAX + 1);
    }
    const uint8_t initialOn = (uint8_t)random(STANDBY_ON_MIN, STANDBY_ON_MAX + 1);
    for (uint8_t i = 0; i < initialOn; ++i) {
      const uint8_t idx = (uint8_t)random(0, PIXEL_COUNT);
      if (!standbyTwinkleOn[idx]) {
        standbyTwinkleOn[idx] = true;
        ++standbyOnCount;
      }
      ledFrameDirty = true;
    }
  }
}

void ledService(uint32_t now) {
  static bool allOnApplied = false;
  if (activeConfig.pixelDebugAllOn) {
    setStripBrightnessPercent(activeConfig.pixelBrightnessPercent);
    if (!allOnApplied) {
      pixelsFill(primaryLedRing.strip->Color(80, 80, 80));
      pixelsShow();
      allOnApplied = true;
    }
    ring2Service(now);
    return;
  }
  allOnApplied = false;

  applyBrightnessForLedModeInternal();

  switch (ledMode) {
    case LedMode::ALL_OFF:
      if (ledFrameDirty) pixelsClear();
      break;
    case LedMode::ERROR_BLINK_RED:
      if (now - ledTickMs >= 350) {
        ledTickMs = now; ledFlip = !ledFlip; ledFrameDirty = true;
      }
      if (ledFrameDirty) { pixelsClear(); if (ledFlip) pixelsFill(colorRed); }
      break;
    case LedMode::RED_SOLID:
      if (ledFrameDirty) pixelsFill(colorRed);
      break;
    case LedMode::OK_ALT_GB:
      if (now - ledTickMs >= 450) { ledTickMs = now; ledFlip = !ledFlip; ledFrameDirty = true; }
      if (ledFrameDirty) { pixelsClear(); if (ledFlip) pixelsSet(ALT_PATTERN_A, sizeof(ALT_PATTERN_A), colorGreen); else pixelsSet(ALT_PATTERN_B, sizeof(ALT_PATTERN_B), colorBlue); }
      break;
    case LedMode::READY_GREEN_BLINK:
      if (now - ledTickMs >= 450) { ledTickMs = now; ledFlip = !ledFlip; ledFrameDirty = true; }
      if (ledFrameDirty) { pixelsClear(); if (ledFlip) pixelsFill(colorGreen); }
      break;
    case LedMode::GLASS_GREEN_SOLID:
      if (ledFrameDirty) pixelsFill(colorGreen);
      break;
    case LedMode::TIMING_BLUE_SPINNER:
      if (now - ledTickMs >= 180) { ledTickMs = now; ledSpinIdx = (uint8_t)((ledSpinIdx + 1) % PIXEL_COUNT); ledFrameDirty = true; }
      if (ledFrameDirty) { pixelsClear(); primaryLedRing.strip->setPixelColor(ledSpinIdx, colorBlue); }
      break;
    case LedMode::RESULT_FLASH_GB_ONCE:
      if (now - ledTickMs < 200) { if (ledFrameDirty) pixelsFill(colorCyan); }
      else ledsSetMode(LedMode::GLASS_GREEN_SOLID);
      break;
    case LedMode::STANDBY_TWINKLE: {
      if (now >= twinkleNextMs) {
        twinkleNextMs = now + random(STANDBY_CHANGE_MIN_MS, STANDBY_CHANGE_MAX_MS);
        const bool needMore = standbyOnCount < STANDBY_ON_MIN;
        const bool needLess = standbyOnCount > STANDBY_ON_MAX;
        const bool shouldToggle = !needMore && !needLess && (random(0, 100) < 45);
        if (needMore || needLess || shouldToggle) {
          for (uint8_t tries = 0; tries < PIXEL_COUNT; ++tries) {
            const uint8_t i = (uint8_t)random(0, PIXEL_COUNT);
            if (needMore) {
              if (!standbyTwinkleOn[i]) { standbyTwinkleOn[i] = true; ++standbyOnCount; standbyHue[i] = (uint16_t)random(0, 65536); standbyValue[i] = (uint8_t)random(STANDBY_VALUE_MIN, STANDBY_VALUE_MAX + 1); ledFrameDirty = true; break; }
            } else if (needLess) {
              if (standbyTwinkleOn[i]) { standbyTwinkleOn[i] = false; --standbyOnCount; ledFrameDirty = true; break; }
            } else if (standbyTwinkleOn[i]) {
              standbyTwinkleOn[i] = false; --standbyOnCount; ledFrameDirty = true; break;
            } else {
              standbyTwinkleOn[i] = true; ++standbyOnCount; standbyHue[i] = (uint16_t)random(0, 65536); standbyValue[i] = (uint8_t)random(STANDBY_VALUE_MIN, STANDBY_VALUE_MAX + 1); ledFrameDirty = true; break;
            }
          }
        }
      }
      if (now >= standbyFrameNextMs) {
        standbyFrameNextMs = now + STANDBY_FRAME_MS;
        for (uint8_t i = 0; i < PIXEL_COUNT; ++i) {
          if (!standbyTwinkleOn[i]) continue;
          const int16_t shift = (int16_t)random(-2, 3);
          standbyHue[i] = (uint16_t)(standbyHue[i] + shift);
          const int16_t delta = (int16_t)random(-4, 5);
          int16_t nextValue = (int16_t)standbyValue[i] + delta;
          if (nextValue < STANDBY_VALUE_MIN) nextValue = STANDBY_VALUE_MIN;
          if (nextValue > STANDBY_VALUE_MAX) nextValue = STANDBY_VALUE_MAX;
          standbyValue[i] = (uint8_t)nextValue;
        }
        ledFrameDirty = true;
      }
      if (ledFrameDirty) standbyApplyOutputs();
      break;
    }
  }

  if (ledFrameDirty) { pixelsShow(); ledFrameDirty = false; }
  ring2Service(now);
}

void ledApplyBrightnessForCurrentMode() {
  applyBrightnessForLedModeInternal();
}

void ledClear() {
  pixelsClear();
#if RING2_ENABLED
  ring2Clear();
#endif
}

void ledShow() {
  pixelsShow();
#if RING2_ENABLED
  secondaryLedRing.strip->show();
#endif
}
