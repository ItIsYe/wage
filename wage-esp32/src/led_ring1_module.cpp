#include "led_ring1_module.h"

#include <math.h>

#include "config.h"
#include "types.h"

extern RuntimeConfig activeConfig;

static CRGB* primaryLeds = nullptr;
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
static CRGB colorGreen = CRGB::Black;
static CRGB colorBlue = CRGB::Black;
static CRGB colorRed = CRGB::Black;
static CRGB colorCyan = CRGB::Black;

static inline bool ring1Ready() { return primaryLeds != nullptr; }

static inline uint8_t brightnessPercentToByte(uint8_t percent) {
  if (percent >= 100) return 255;
  return (uint8_t)((uint16_t)percent * 255u / 100u);
}
static inline CRGB scaleColor(const CRGB& color, uint8_t brightness) {
  CRGB out = color;
  out.nscale8_video(brightness);
  return out;
}
static inline CRGB rgb(uint8_t r, uint8_t g, uint8_t b) { return CRGB(r, g, b); }
static inline uint8_t ledGamma8(uint8_t value) {
  const float normalized = value / 255.0f;
  const float corrected = powf(normalized, 2.8f);
  return (uint8_t)(corrected * 255.0f + 0.5f);
}
static inline CRGB hsvGamma(uint16_t hue, uint8_t sat, uint8_t val) {
  CHSV hsv((uint8_t)(hue >> 8), sat, val);
  CRGB out;
  hsv2rgb_rainbow(hsv, out);
  out.r = ledGamma8(out.r);
  out.g = ledGamma8(out.g);
  out.b = ledGamma8(out.b);
  return out;
}
static inline void setStripBrightnessPercent(uint8_t percent) {
  const uint8_t target = brightnessPercentToByte(percent);
  if (brightnessInitialized && currentBrightnessByte == target) return;
  currentBrightnessByte = target;
  brightnessInitialized = true;
}
static inline void pixelsClear() { if (!ring1Ready()) return; fill_solid(primaryLeds, PIXEL_COUNT, CRGB::Black); }
static inline void pixelsFill(const CRGB& color) {
  if (!ring1Ready()) return;
  for (uint16_t i = 0; i < PIXEL_COUNT; ++i) primaryLeds[i] = scaleColor(color, currentBrightnessByte);
}
static inline void pixelsSet(const uint8_t* indices, uint8_t count, const CRGB& color) {
  if (!ring1Ready()) return;
  for (uint8_t i = 0; i < count; ++i) {
    if (indices[i] < PIXEL_COUNT) primaryLeds[indices[i]] = scaleColor(color, currentBrightnessByte);
  }
}
static inline void applyBrightnessForLedModeInternal() {
  const uint8_t percent = (ledMode == LedMode::STANDBY_TWINKLE)
    ? activeConfig.standbyBrightnessPercent
    : activeConfig.pixelBrightnessPercent;
  setStripBrightnessPercent(percent);
}
static inline uint32_t sanitizeRangeMin(uint32_t minValue, uint32_t maxValue) {
  return (minValue > maxValue) ? maxValue : minValue;
}
static inline uint32_t sanitizeStandbyFrameMs(uint32_t frameMs) {
  static constexpr uint32_t kStandbyFrameMinMs = 30U;
  static constexpr uint32_t kStandbyFrameMaxMs = 1000U;
  if (frameMs < kStandbyFrameMinMs) return kStandbyFrameMinMs;
  if (frameMs > kStandbyFrameMaxMs) return kStandbyFrameMaxMs;
  return frameMs;
}
static inline uint32_t randomInclusiveU32(uint32_t minValue, uint32_t maxValue) {
  if (minValue > maxValue) {
    const uint32_t tmp = minValue;
    minValue = maxValue;
    maxValue = tmp;
  }
  if (maxValue == UINT32_MAX) {
    return minValue + (uint32_t)random(0, (long)(maxValue - minValue));
  }
  return minValue + (uint32_t)random(0, (long)(maxValue - minValue + 1U));
}
static inline uint8_t randomInclusiveU8(uint8_t minValue, uint8_t maxValue) {
  if (minValue > maxValue) {
    const uint8_t tmp = minValue;
    minValue = maxValue;
    maxValue = tmp;
  }
  return (uint8_t)(minValue + (uint8_t)random(0, (int16_t)(maxValue - minValue + 1U)));
}

static constexpr uint8_t ALT_PATTERN_A[] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24};
static constexpr uint8_t ALT_PATTERN_B[] = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23};
static void standbyApplyOutputs() {
  if (!ring1Ready()) return;
  pixelsClear();
  for (uint8_t i = 0; i < PIXEL_COUNT; ++i) {
    if (standbyTwinkleOn[i]) {
      primaryLeds[i] = scaleColor(hsvGamma(standbyHue[i], activeConfig.standbySaturation, standbyValue[i]), currentBrightnessByte);
    }
  }
}

void ring1Init(CRGB* leds) {
  primaryLeds = leds;
  setStripBrightnessPercent(activeConfig.pixelBrightnessPercent);
  colorGreen = rgb(0, 90, 0);
  colorBlue = rgb(0, 0, 100);
  colorRed = rgb(100, 0, 0);
  colorCyan = rgb(0, 90, 90);
  pixelsClear();
  ledFrameDirty = true;
}
void ring1SetMode(LedMode m, uint32_t now) {
  if (ledMode == m) return;
  ledMode = m;
  ledTickMs = now;
  ledFlip = false;
  if (m == LedMode::READY_GREEN_BLINK) ledFlip = true;
  ledSpinIdx = 0;
  ledFrameDirty = true;
  if (m == LedMode::STANDBY_TWINKLE) {
    const uint32_t changeMaxMs = activeConfig.standbyChangeMaxMs;
    const uint32_t changeMinMs = sanitizeRangeMin(activeConfig.standbyChangeMinMs, changeMaxMs);
    const uint8_t valueMax = activeConfig.standbyValueMax;
    const uint8_t valueMin = (activeConfig.standbyValueMin > valueMax) ? valueMax : activeConfig.standbyValueMin;
    const uint8_t onMax = activeConfig.standbyOnMax;
    const uint8_t onMin = (activeConfig.standbyOnMin > onMax) ? onMax : activeConfig.standbyOnMin;

    standbyFrameNextMs = now + sanitizeStandbyFrameMs(activeConfig.standbyFrameMs);
    twinkleNextMs = now + randomInclusiveU32(changeMinMs, changeMaxMs);
    standbyOnCount = 0;
    for (uint8_t i = 0; i < PIXEL_COUNT; i++) {
      standbyTwinkleOn[i] = false;
      standbyHue[i] = (uint16_t)random(0, 65536);
      standbyValue[i] = randomInclusiveU8(valueMin, valueMax);
    }
    const uint8_t initialOn = randomInclusiveU8(onMin, onMax);
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

bool ring1Service(uint32_t now) {
  if (!ring1Ready()) return false;
  applyBrightnessForLedModeInternal();
  switch (ledMode) {
    case LedMode::ALL_OFF: if (ledFrameDirty) pixelsClear(); break;
    case LedMode::ERROR_BLINK_RED:
      if (now - ledTickMs >= 350) { ledTickMs = now; ledFlip = !ledFlip; ledFrameDirty = true; }
      if (ledFrameDirty) { pixelsClear(); if (ledFlip) pixelsFill(colorRed); }
      break;
    case LedMode::RED_SOLID: if (ledFrameDirty) pixelsFill(colorRed); break;
    case LedMode::OK_ALT_GB:
      if (now - ledTickMs >= 450) { ledTickMs = now; ledFlip = !ledFlip; ledFrameDirty = true; }
      if (ledFrameDirty) { pixelsClear(); if (ledFlip) pixelsSet(ALT_PATTERN_A, sizeof(ALT_PATTERN_A), colorGreen); else pixelsSet(ALT_PATTERN_B, sizeof(ALT_PATTERN_B), colorBlue); }
      break;
    case LedMode::READY_GREEN_BLINK:
      if (now - ledTickMs >= 450) { ledTickMs = now; ledFlip = !ledFlip; ledFrameDirty = true; }
      if (ledFrameDirty) { pixelsClear(); if (ledFlip) pixelsFill(colorGreen); }
      break;
    case LedMode::GLASS_GREEN_SOLID: if (ledFrameDirty) pixelsFill(colorGreen); break;
    case LedMode::TIMING_BLUE_SPINNER:
      if (now - ledTickMs >= 180) { ledTickMs = now; ledSpinIdx = (uint8_t)((ledSpinIdx + 1) % PIXEL_COUNT); ledFrameDirty = true; }
      if (ledFrameDirty) { pixelsClear(); primaryLeds[ledSpinIdx] = scaleColor(colorBlue, currentBrightnessByte); }
      break;
    case LedMode::RESULT_FLASH_GB_ONCE:
      if (now - ledTickMs < 200) { if (ledFrameDirty) pixelsFill(colorCyan); }
      else ring1SetMode(LedMode::GLASS_GREEN_SOLID, now);
      break;
    case LedMode::STANDBY_TWINKLE: {
      if (now >= twinkleNextMs) {
        const uint32_t changeMaxMs = activeConfig.standbyChangeMaxMs;
        const uint32_t changeMinMs = sanitizeRangeMin(activeConfig.standbyChangeMinMs, changeMaxMs);
        const uint8_t valueMax = activeConfig.standbyValueMax;
        const uint8_t valueMin = (activeConfig.standbyValueMin > valueMax) ? valueMax : activeConfig.standbyValueMin;
        const uint8_t onMax = activeConfig.standbyOnMax;
        const uint8_t onMin = (activeConfig.standbyOnMin > onMax) ? onMax : activeConfig.standbyOnMin;

        twinkleNextMs = now + randomInclusiveU32(changeMinMs, changeMaxMs);
        const bool needMore = standbyOnCount < onMin;
        const bool needLess = standbyOnCount > onMax;
        const bool shouldToggle = !needMore && !needLess && (random(0, 100) < 45);
        if (needMore || needLess || shouldToggle) {
          for (uint8_t tries = 0; tries < PIXEL_COUNT; ++tries) {
            const uint8_t i = (uint8_t)random(0, PIXEL_COUNT);
            if (needMore) {
              if (!standbyTwinkleOn[i]) { standbyTwinkleOn[i] = true; ++standbyOnCount; standbyHue[i] = (uint16_t)random(0, 65536); standbyValue[i] = randomInclusiveU8(valueMin, valueMax); ledFrameDirty = true; break; }
            } else if (needLess) {
              if (standbyTwinkleOn[i]) { standbyTwinkleOn[i] = false; --standbyOnCount; ledFrameDirty = true; break; }
            } else if (standbyTwinkleOn[i]) {
              standbyTwinkleOn[i] = false; --standbyOnCount; ledFrameDirty = true; break;
            } else {
              standbyTwinkleOn[i] = true; ++standbyOnCount; standbyHue[i] = (uint16_t)random(0, 65536); standbyValue[i] = randomInclusiveU8(valueMin, valueMax); ledFrameDirty = true; break;
            }
          }
        }
      }
      if (now >= standbyFrameNextMs) {
        const uint8_t valueMax = activeConfig.standbyValueMax;
        const uint8_t valueMin = (activeConfig.standbyValueMin > valueMax) ? valueMax : activeConfig.standbyValueMin;

        standbyFrameNextMs = now + sanitizeStandbyFrameMs(activeConfig.standbyFrameMs);
        for (uint8_t i = 0; i < PIXEL_COUNT; ++i) {
          if (!standbyTwinkleOn[i]) continue;
          const int16_t shift = (int16_t)random(-2, 3);
          standbyHue[i] = (uint16_t)(standbyHue[i] + shift);
          const int16_t delta = (int16_t)random(-4, 5);
          int16_t nextValue = (int16_t)standbyValue[i] + delta;
          if (nextValue < valueMin) nextValue = valueMin;
          if (nextValue > valueMax) nextValue = valueMax;
          standbyValue[i] = (uint8_t)nextValue;
        }
        ledFrameDirty = true;
      }
      if (ledFrameDirty) standbyApplyOutputs();
      break;
    }
  }
  const bool dirty = ledFrameDirty;
  ledFrameDirty = false;
  return dirty;
}
void ring1ApplyBrightnessForCurrentMode() { applyBrightnessForLedModeInternal(); }
void ring1MarkDirty() { ledFrameDirty = true; }
void ring1Clear() { if (!ring1Ready()) return; pixelsClear(); }
void ring1FillDebugAllOn() { if (!ring1Ready()) return; applyBrightnessForLedModeInternal(); pixelsFill(rgb(80, 80, 80)); ledFrameDirty = false; }
