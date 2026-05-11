#include "scale_module.h"

#include <HX711.h>

#include "config.h"

extern RuntimeConfig activeConfig;
extern State state;

HX711 scale1;
HX711 scale2;

static float maBuf[MA_N];
static uint8_t maIdx = 0;
static bool maFilled = false;
static float maSum = 0.0f;

float w_raw1 = 0.0f;
float w_raw2 = 0.0f;
float w_avg = 0.0f;
float w_filt = 0.0f;

static float stabMin = 1e9f;
static float stabMax = -1e9f;
static uint32_t stabWindowStart = 0;
static uint32_t stableSince = 0;
bool isStable = false;

static uint32_t lastScaleReadMs = 0;
static uint32_t negativeCheckSuppressUntilMs = 0;
bool haveRead = false;
bool haveStableRead = false;
float absFilt = 0.0f;
bool objectMissingStable = false;
long raw1 = 0;
long raw2 = 0;

static float applyInvert(float g, bool inv) {
  return inv ? -g : g;
}

static void updateFilter(float newVal) {
  const uint8_t nBefore = maFilled ? MA_N : maIdx;
  maSum -= maBuf[maIdx];
  maBuf[maIdx] = newVal;
  maSum += newVal;
  maIdx = (maIdx + 1) % MA_N;
  if (maIdx == 0) maFilled = true;

  const uint8_t n = maFilled ? MA_N : (uint8_t)(nBefore + 1);
  w_filt = (n > 0) ? (maSum / (float)n) : newVal;
}

static void updateStability(float val) {
  const uint32_t now = millis();
  if (stabWindowStart == 0) {
    stabWindowStart = now;
    stabMin = val;
    stabMax = val;
    isStable = false;
    stableSince = 0;
    return;
  }

  stabMin = min(stabMin, val);
  stabMax = max(stabMax, val);

  if (now - stabWindowStart >= STABLE_WINDOW_MS) {
    const float band = stabMax - stabMin;
    const bool windowStable = (band <= STABLE_BAND_G);

    if (windowStable) {
      if (!isStable) {
        if (stableSince == 0) stableSince = now;
        if (now - stableSince >= STABLE_HOLD_MS) isStable = true;
      }
    } else {
      isStable = false;
      stableSince = 0;
    }

    stabWindowStart = now;
    stabMin = val;
    stabMax = val;
  }
}

static float sanitizeNegativeWeight(float val) {
  if (val < 0.0f && val >= NEGATIVE_CLAMP_G) return 0.0f;
  return val;
}

static inline float unitsFromRaw(long raw, HX711& s, bool invert) {
  const float scale = s.get_scale();
  if (scale == 0.0f) return 0.0f;
  const float value = ((float)raw - (float)s.get_offset()) / scale;
  return applyInvert(value, invert);
}

void scaleInit() {
  scale1.begin(HX1_DOUT, HX1_SCK);
  scale2.begin(HX2_DOUT, HX2_SCK);
  scale1.set_scale(DEFAULT_CAL1);
  scale2.set_scale(DEFAULT_CAL2);
}

bool readScalesOnce(long& outRaw1, long& outRaw2) {
  if (!scale1.is_ready() || !scale2.is_ready()) return false;

  outRaw1 = scale1.read_average(activeConfig.scaleReadSamples);
  outRaw2 = scale2.read_average(activeConfig.scaleReadSamples);
  raw1 = outRaw1;
  raw2 = outRaw2;

  w_raw1 = unitsFromRaw(outRaw1, scale1, INVERT1);
  w_raw2 = unitsFromRaw(outRaw2, scale2, INVERT2);
  w_avg = (w_raw1 + w_raw2) * 0.5f;

  updateFilter(w_avg);
  updateStability(w_filt);
  w_filt = sanitizeNegativeWeight(w_filt);

  return true;
}

void tareBoth() {
  scale1.tare(TARE_SAMPLES);
  scale2.tare(TARE_SAMPLES);

  for (uint8_t i = 0; i < MA_N; i++) maBuf[i] = 0.0f;
  maIdx = 0;
  maFilled = false;
  maSum = 0.0f;
  w_raw1 = w_raw2 = w_avg = w_filt = 0.0f;

  stabWindowStart = 0;
  stableSince = 0;
  isStable = false;
  haveRead = false;
  haveStableRead = false;
  absFilt = 0.0f;
  objectMissingStable = false;
  negativeCheckSuppressUntilMs = millis() + STABLE_WINDOW_MS;
}

bool isNegativeCheckSuppressed(uint32_t now) {
  return now < negativeCheckSuppressUntilMs;
}

bool isObjectPresentStable(float weight, bool stable) {
  return stable && (weight >= activeConfig.objectPresentG);
}


void scaleService(uint32_t now) {
  haveRead = false;
  haveStableRead = false;
  objectMissingStable = false;
  absFilt = fabsf(w_filt);

  const bool needScaleRead = (state != State::BOOT_MSG && state != State::BOOT_TARE && state != State::SHOW_RESULT);
  if (!needScaleRead) return;
  if ((now - lastScaleReadMs) < activeConfig.scaleReadIntervalMs) return;
  lastScaleReadMs = now;

  haveRead = readScalesOnce(raw1, raw2);
  haveStableRead = haveRead && isStable;
  absFilt = haveRead ? fabsf(w_filt) : absFilt;
  objectMissingStable = haveStableRead && (w_filt < activeConfig.objectPresentG * 0.7f);
}
