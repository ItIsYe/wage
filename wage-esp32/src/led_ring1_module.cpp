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
static uint16_t ledSpinIdx = 0;
static bool ledFrameDirty = true;
static bool standbyTwinkleOn[PIXEL_COUNT] = {};
static uint16_t standbyOnCount = 0;
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
// Gamma-LUT: einmal beim Start befüllt, danach O(1) statt powf() pro Pixel.
static uint8_t gammaLut[256] = {};
static bool gammaLutReady = false;

static void buildGammaLut() {
  if (gammaLutReady) return;
  for (uint16_t i = 0; i < 256; ++i) {
    const float normalized = i / 255.0f;
    const float corrected = powf(normalized, 2.8f);
    gammaLut[i] = (uint8_t)(corrected * 255.0f + 0.5f);
  }
  gammaLutReady = true;
}

static inline uint8_t ledGamma8(uint8_t value) {
  return gammaLut[value];
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
static inline void pixelsSet(const uint8_t* indices, uint16_t count, const CRGB& color) {
  if (!ring1Ready()) return;
  for (uint16_t i = 0; i < count; ++i) {
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

static constexpr uint8_t ALT_PATTERN_A[] = {0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,66,68,70,72,74,76,78,80,82,84,86,88,90,92,94,96,98,100,102,104,106,108,110,112,114,116,118,120,122,124,126,128,130,132,134,136,138,140,142,144,146,148,150,152,154,156,158,160,162,164,166,168,170,172,174,176,178,180,182,184,186,188,190,192,194,196,198,200,202,204,206,208,210,212,214,216,218,220,222,224,226,228,230,232,234,236,238,240,242,244,246,248};
static constexpr uint8_t ALT_PATTERN_B[] = {1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,49,51,53,55,57,59,61,63,65,67,69,71,73,75,77,79,81,83,85,87,89,91,93,95,97,99,101,103,105,107,109,111,113,115,117,119,121,123,125,127,129,131,133,135,137,139,141,143,145,147,149,151,153,155,157,159,161,163,165,167,169,171,173,175,177,179,181,183,185,187,189,191,193,195,197,199,201,203,205,207,209,211,213,215,217,219,221,223,225,227,229,231,233,235,237,239,241,243,245,247,249};
static void standbyApplyOutputs() {
  if (!ring1Ready()) return;
  pixelsClear();
  for (uint16_t i = 0; i < PIXEL_COUNT; ++i) {
    if (standbyTwinkleOn[i]) {
      primaryLeds[i] = scaleColor(hsvGamma(standbyHue[i], activeConfig.standbySaturation, standbyValue[i]), currentBrightnessByte);
    }
  }
}

void ring1Init(CRGB* leds) {
  buildGammaLut();
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
    for (uint16_t i = 0; i < PIXEL_COUNT; i++) {
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
      if (now - ledTickMs >= 180) { ledTickMs = now; ledSpinIdx = (uint16_t)((ledSpinIdx + 1) % PIXEL_COUNT); ledFrameDirty = true; }
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
          for (uint16_t tries = 0; tries < PIXEL_COUNT; ++tries) {
            const uint16_t i = (uint16_t)random(0, PIXEL_COUNT);
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
        for (uint16_t i = 0; i < PIXEL_COUNT; ++i) {
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
void ring1ApplySharedStandby(const bool* on, const uint16_t* hue, const uint8_t* value, uint16_t count) {
  if (!ring1Ready()) return;
  setStripBrightnessPercent(activeConfig.standbyBrightnessPercent);
  pixelsClear();
  const uint16_t limit = (count < PIXEL_COUNT) ? count : PIXEL_COUNT;
  for (uint16_t i = 0; i < limit; ++i) {
    if (!on[i]) continue;
    primaryLeds[i] = scaleColor(hsvGamma(hue[i], activeConfig.standbySaturation, value[i]), currentBrightnessByte);
  }
}
