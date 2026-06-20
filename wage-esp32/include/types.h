#pragma once

#include <Arduino.h>

class Adafruit_NeoPixel;

enum class ErrCode : uint8_t {
  OK = 0,
  NEGATIVE,
  UNSTABLE
};

enum class State : uint8_t {
  BOOT_MSG = 0,
  BOOT_TARE,
  IDLE_WAIT_GLASS,
  GLASS_DETECTED,
  READY_FOR_TIMING,
  TIMING,
  SHOW_RESULT,
  WAIT_EMPTY_AFTER_RESULT,
  CHECK_RETARE,
  STANDBY,
  ERROR_RECOVER
};

enum class LedMode : uint8_t {
  ALL_OFF = 0,
  ERROR_BLINK_RED,
  RED_SOLID,
  OK_ALT_GB,
  READY_GREEN_BLINK,
  GLASS_GREEN_SOLID,
  TIMING_BLUE_SPINNER,
  RESULT_FLASH_GB_ONCE,
  STANDBY_TWINKLE
};

struct RuntimeConfig {
  float startDropPercent;
  float stopRisePercent;
  float objectPresentG;
  float emptyThresholdG;
  float retareTolG;
  float standbyWakeThresholdG;
  uint32_t standbyAfterMs;
  uint32_t standbyFrameMs;
  uint32_t standbyChangeMinMs;
  uint32_t standbyChangeMaxMs;
  uint8_t standbySaturation;
  uint8_t standbyValueMin;
  uint8_t standbyValueMax;
  uint8_t standbyOnMin;
  uint8_t standbyOnMax;
  uint32_t oledTimingRefreshMs;
  uint32_t scaleReadIntervalMs;
  uint8_t scaleReadSamples;
  uint32_t oledI2cClockHz;
  uint8_t oledRotation;
  float oledScaleValue;
  bool debugMode;
  bool oledDebugMode;
  uint8_t pixelBrightnessPercent;
  uint8_t standbyBrightnessPercent;
  bool pixelDebugAllOn;
  bool ring2Enabled;
  uint8_t ring2BrightnessPercent;
  uint8_t ring2StandbyBrightnessPercent;
  bool ring2DebugAllOn;
  char deviceId[32];
  bool wifiStaEnabled;
  char wifiSsid[32];
  char wifiPassword[64];
  uint32_t wifiConnectTimeoutMs;
  bool wifiUseStaticIp;
  char wifiLocalIp[16];
  char wifiGateway[16];
  char wifiSubnet[16];
  char wifiDns1[16];
  char wifiDns2[16];
  char configApSsid[32];
  char configApPassword[64];
  bool externalEnabled;
  char externalHost[64];
  uint16_t externalPort;
  char externalApiPath[64];
  char externalApiKey[64];
};

struct LedRingContext {
  Adafruit_NeoPixel* strip;
  uint16_t pixelCount;
};

struct RunDataSnapshot {
  char eventId[48];
  char deviceId[32];
  uint32_t bootId;
  uint32_t runNumber;
  uint32_t finishedAtMs;
  uint32_t durationMs;
  float referenceWeightG;
  float minWeightG;
  float startDropThresholdG;
  float stopRiseThresholdG;
};
