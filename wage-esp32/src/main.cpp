#include <Arduino.h>
#include <HX711.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <string.h>

#include "config.h"
#include "types.h"
#include "display_module.h"
#include "led_module.h"

/* =========================================================
   SCALE / CALIBRATION
   ========================================================= */

HX711 scale1;
HX711 scale2;

/* =========================================================
   DISPLAY
   ========================================================= */

Adafruit_SH1106G display(SCREEN_W, SCREEN_H, &Wire, -1);

/* =========================================================
   EXTERNAL INTERFACE PREP (STUBS ONLY)
   ========================================================= */

// TODO(interface): spaeter Queue/Retry + Pi/HTTP Transport anbinden.
static bool enqueueRunDataForExternalSend(const RunDataSnapshot&) {
  // Stub: bewusst ohne Seiteneffekte. Nur Strukturvorbereitung fuer spaetere Erweiterungen.
  return false;
}


/* =========================================================
   ERROR CODES
   ========================================================= */

const char* errToStr(ErrCode e) {
  switch (e) {
    case ErrCode::OK: return "OK";
    case ErrCode::NEGATIVE: return "NEGATIVE";
    case ErrCode::UNSTABLE: return "UNSTABLE";
  }
  return "UNKNOWN";
}

/* =========================================================
   STATE MACHINE
   ========================================================= */

State state = State::BOOT_MSG;
ErrCode err = ErrCode::OK;

static const char* FIRMWARE_VERSION = "v1-webcfg";

RuntimeConfig activeConfig;
static RuntimeConfig pendingConfig;
static bool pendingConfigValid = false;
static bool wifiApMode = false;
static String networkInfo;
static WebServer server(WEB_SERVER_PORT);
static Preferences prefs;
static bool resetRequested = false;
static String resetStatusMsg;

/* =========================================================
   RUNTIME VARIABLES
   ========================================================= */

// moving average buffer
static float maBuf[MA_N];
static uint8_t maIdx = 0;
static bool maFilled = false;
static float maSum = 0.0f;

float w_raw1 = 0.0f, w_raw2 = 0.0f;   // in g (after calibration)
float w_avg  = 0.0f;                  // raw mean (2 cells)
float w_filt = 0.0f;                  // filtered mean
float oledScale = 1.5f;

// stability window
static float stabMin = 1e9f, stabMax = -1e9f;
static uint32_t stabWindowStart = 0;
static uint32_t stableSince = 0;
bool isStable = false;

// object tracking
bool objectPresent = false;
static float readyReferenceWeight = 0.0f;
static float startDropThresholdG = 0.0f;
static float stopRiseThresholdG = 0.0f;

// timing
static uint32_t detectAtMs = 0;
static uint32_t tStartMs   = 0;
static uint32_t showUntilMs= 0;

static float minDuringTiming = 1e9f;
static uint32_t dropSinceMs = 0;
static uint32_t stopCandidateSinceMs = 0;

// standby
static uint32_t lastActionMs = 0;

// recovery
static uint32_t recoverUntilMs = 0;


static bool oledDebugLedsCleared = false;

// OLED-Cache gegen redundante Full-Refreshes
// Serial-Rate-Limiter
static uint32_t lastSerialBaseMs = 0;
static uint32_t lastSerialReadyMs = 0;
static uint32_t lastSerialTimingMs = 0;
static uint32_t lastSerialRecoverMs = 0;
static uint32_t lastScaleReadMs = 0;
static bool haveRead = false;
static bool haveStableRead = false;
static float absFilt = 0.0f;
static bool objectMissingStable = false;
static long raw1 = 0, raw2 = 0;


static uint32_t perfLastLogMs = 0;
static uint32_t perfMaxScaleUs = 0;
static uint32_t perfMaxLedUs = 0;
static uint32_t perfMaxOledUs = 0;
static uint32_t perfMaxStateUs = 0;
static uint32_t perfMaxConfigUs = 0;
static uint32_t perfMaxResetUs = 0;
static uint32_t perfMaxWebUs = 0;
static uint32_t perfMaxLoopUs = 0;
static uint32_t lastWebServiceMs = 0;

/* =========================================================
   FORWARD DECLARATIONS
   ========================================================= */
static void setState(State s);
static void finalizeTiming(uint32_t now);
static float percentOfReference(float referenceWeight, float percent);
static void refreshTimingThresholds();
static const char* stateToStr(State s);
static bool shouldCheckNegativeError(State s);
static void scaleService(uint32_t now);
static void oledService(uint32_t now);
static void stateMachineService(uint32_t now);
static void webService(uint32_t now);
static void applyPendingConfigIfAllowed();
static void handleResetRequestIfAllowed(uint32_t now);

/* =========================================================
   HELPERS
   ========================================================= */

static void serialPrintAll(long raw1, long raw2) {
  Serial.print("RAW1:");
  Serial.print(raw1);
  Serial.print(" g1:");
  Serial.print(w_raw1, 2);
  Serial.print("  ||  RAW2:");
  Serial.print(raw2);
  Serial.print(" g2:");
  Serial.print(w_raw2, 2);
  Serial.print("  AVG:");
  Serial.print(w_avg, 2);
  Serial.print("  FILT:");
  Serial.print(w_filt, 2);
  Serial.print("  STB:");
  Serial.print(isStable ? 1 : 0);
  Serial.print("  STATE:");
  Serial.print((int)state);
  Serial.print("  OBJ:");
  Serial.print(objectPresent ? 1 : 0);
  Serial.print("  ERR:");
  Serial.println(errToStr(err));
}

/* =========================================================
   SCALE READ + FILTER + STABILITY
   ========================================================= */

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

static bool readScalesOnce(long& raw1, long& raw2) {
  if (!scale1.is_ready() || !scale2.is_ready()) return false;

  raw1 = scale1.read_average(activeConfig.scaleReadSamples);
  raw2 = scale2.read_average(activeConfig.scaleReadSamples);

  w_raw1 = unitsFromRaw(raw1, scale1, INVERT1);
  w_raw2 = unitsFromRaw(raw2, scale2, INVERT2);
  w_avg  = (w_raw1 + w_raw2) * 0.5f;

  updateFilter(w_avg);
  updateStability(w_filt);
  w_filt = sanitizeNegativeWeight(w_filt);

  return true;
}

static void tareBoth() {
  scale1.tare(TARE_SAMPLES);
  scale2.tare(TARE_SAMPLES);

  for (uint8_t i=0;i<MA_N;i++) maBuf[i] = 0.0f;
  maIdx = 0;
  maFilled = false;
  maSum = 0.0f;
  w_raw1 = w_raw2 = w_avg = w_filt = 0.0f;

  stabWindowStart = 0;
  stableSince = 0;
  isStable = false;
}

static bool isObjectPresentStable(float weight, bool stable) {
  return stable && (weight >= activeConfig.objectPresentG);
}

static float percentOfReference(float referenceWeight, float percent) {
  return referenceWeight * (percent * 0.01f);
}

static void refreshTimingThresholds() {
  readyReferenceWeight = max(readyReferenceWeight, activeConfig.objectPresentG);
  startDropThresholdG = max(
    MIN_DYNAMIC_THRESHOLD_G,
    percentOfReference(readyReferenceWeight, activeConfig.startDropPercent)
  );
  stopRiseThresholdG = max(
    MIN_DYNAMIC_THRESHOLD_G,
    percentOfReference(readyReferenceWeight, activeConfig.stopRisePercent)
  );

  if (MASTER_DEBUG_LOG) {
    Serial.print("[THRESH] ref=");
    Serial.print(readyReferenceWeight, 2);
    Serial.print("g startDrop=");
    Serial.print(startDropThresholdG, 2);
    Serial.print("g stopRise=");
    Serial.print(stopRiseThresholdG, 2);
    Serial.println("g (% vom ref)");
  }
}

static RunDataSnapshot buildRunDataSnapshot(uint32_t now, uint32_t durationMs) {
  RunDataSnapshot snapshot{};
  snapshot.finishedAtMs = now;
  snapshot.durationMs = durationMs;
  snapshot.referenceWeightG = readyReferenceWeight;
  snapshot.minWeightG = minDuringTiming;
  snapshot.startDropThresholdG = startDropThresholdG;
  snapshot.stopRiseThresholdG = stopRiseThresholdG;
  strncpy(snapshot.deviceId, activeConfig.deviceId, sizeof(snapshot.deviceId) - 1);
  snapshot.deviceId[sizeof(snapshot.deviceId) - 1] = '\0';
  return snapshot;
}

static void finalizeTiming(uint32_t now) {
  const uint32_t dt = now - tStartMs;
  const RunDataSnapshot runData = buildRunDataSnapshot(now, dt);
  (void)enqueueRunDataForExternalSend(runData); // TODO(interface): Rueckgabe spaeter fuer Queue/Retry nutzen.
  char buf[24];
  snprintf(buf, sizeof(buf), "Zeit: %.1fs", dt / 1000.0f);
  oledMsg2("Fertig", buf);

  ledsSetMode(LedMode::RESULT_FLASH_GB_ONCE);
  showUntilMs = now + SHOW_RESULT_MS;
  setState(State::SHOW_RESULT);
}

/* =========================================================
   STATE MACHINE
   ========================================================= */

static void setError(ErrCode e) {
  err = e;
  ledsSetMode(LedMode::ERROR_BLINK_RED);
  char line2[32];
  snprintf(line2, sizeof(line2), "ERR: %s", errToStr(e));
  oledMsg2("Fehler!", line2);
  state = State::ERROR_RECOVER;
  recoverUntilMs = millis() + RECOVER_WAIT_MS;
}

static const char* stateToStr(State s) {
  switch (s) {
    case State::BOOT_MSG: return "BOOT_MSG";
    case State::BOOT_TARE: return "BOOT_TARE";
    case State::IDLE_WAIT_GLASS: return "IDLE_WAIT_GLASS";
    case State::GLASS_DETECTED: return "GLASS_DETECTED";
    case State::READY_FOR_TIMING: return "READY_FOR_TIMING";
    case State::TIMING: return "TIMING";
    case State::SHOW_RESULT: return "SHOW_RESULT";
    case State::WAIT_EMPTY_AFTER_RESULT: return "WAIT_EMPTY_AFTER_RESULT";
    case State::CHECK_RETARE: return "CHECK_RETARE";
    case State::STANDBY: return "STANDBY";
    case State::ERROR_RECOVER: return "ERROR_RECOVER";
  }
  return "UNKNOWN";
}


static bool shouldCheckNegativeError(State s) {
  switch (s) {
    case State::IDLE_WAIT_GLASS:
    case State::WAIT_EMPTY_AFTER_RESULT:
    case State::CHECK_RETARE:
    case State::ERROR_RECOVER:
      return true;
    default:
      return false;
  }
}

static void setState(State s) {
  if (MASTER_DEBUG_LOG && state != s) {
    Serial.print("[STATE] ");
    Serial.print(stateToStr(state));
    Serial.print(" -> ");
    Serial.println(stateToStr(s));
  }
  state = s;
  lastActionMs = millis();
}

static void scaleService(uint32_t now) {
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

  if (MASTER_DEBUG_LOG && haveRead && (now - lastSerialBaseMs >= SERIAL_BASE_REFRESH_MS)) {
    lastSerialBaseMs = now;
    serialPrintAll(raw1, raw2);
  }
}

static void oledService(uint32_t now) {
  if (state == State::TIMING) {
    oledTimingLive(now - tStartMs);
  }
}

/* =========================================================
   SETUP / LOOP
   ========================================================= */

static void loadDefaults(){ memset(&activeConfig,0,sizeof(activeConfig));
  activeConfig.startDropPercent=DEFAULT_START_DROP_PERCENT; activeConfig.stopRisePercent=DEFAULT_STOP_RISE_PERCENT;
  activeConfig.objectPresentG=DEFAULT_OBJECT_PRESENT_G; activeConfig.emptyThresholdG=DEFAULT_EMPTY_THRESHOLD_G;
  activeConfig.retareTolG=DEFAULT_RETARE_TOL_G; activeConfig.standbyWakeThresholdG=DEFAULT_STANDBY_WAKE_THRESHOLD_G;
  activeConfig.oledTimingRefreshMs=DEFAULT_OLED_TIMING_REFRESH_MS; activeConfig.scaleReadIntervalMs=DEFAULT_SCALE_READ_INTERVAL_MS;
  activeConfig.scaleReadSamples=DEFAULT_SCALE_READ_SAMPLES; activeConfig.oledI2cClockHz=DEFAULT_OLED_I2C_CLOCK_HZ;
  activeConfig.oledRotation=DEFAULT_OLED_ROTATION; activeConfig.oledScaleValue=1.5f; activeConfig.debugMode=false; activeConfig.oledDebugMode=false;
  activeConfig.pixelBrightnessPercent=DEFAULT_PIXEL_BRIGHTNESS_PERCENT; activeConfig.standbyBrightnessPercent=DEFAULT_STANDBY_BRIGHTNESS_PERCENT;
  activeConfig.pixelDebugAllOn=DEFAULT_PIXEL_DEBUG_ALL_ON; strncpy(activeConfig.deviceId,"waage-01",sizeof(activeConfig.deviceId)-1);}


static void saveConfigToPrefs(const RuntimeConfig& c){
  prefs.begin("cfg", false);
  prefs.putFloat("startDrop", c.startDropPercent); prefs.putFloat("stopRise", c.stopRisePercent);
  prefs.putFloat("obj", c.objectPresentG); prefs.putFloat("empty", c.emptyThresholdG); prefs.putFloat("retare", c.retareTolG);
  prefs.putFloat("wake", c.standbyWakeThresholdG); prefs.putUInt("oledRef", c.oledTimingRefreshMs); prefs.putUInt("scaleInt", c.scaleReadIntervalMs);
  prefs.putUChar("samples", c.scaleReadSamples); prefs.putUInt("i2c", c.oledI2cClockHz); prefs.putUChar("rot", c.oledRotation);
  prefs.putFloat("oledScale", c.oledScaleValue); prefs.putBool("dbg", c.debugMode); prefs.putBool("odbg", c.oledDebugMode);
  prefs.putUChar("pixB", c.pixelBrightnessPercent); prefs.putUChar("stbyB", c.standbyBrightnessPercent); prefs.putBool("pixDbg", c.pixelDebugAllOn);
  prefs.putString("dev", c.deviceId); prefs.end();
}

static void loadConfigFromPrefs(){
  prefs.begin("cfg", true);
  activeConfig.startDropPercent=prefs.getFloat("startDrop", activeConfig.startDropPercent);
  activeConfig.stopRisePercent=prefs.getFloat("stopRise", activeConfig.stopRisePercent);
  activeConfig.objectPresentG=prefs.getFloat("obj", activeConfig.objectPresentG);
  activeConfig.emptyThresholdG=prefs.getFloat("empty", activeConfig.emptyThresholdG);
  activeConfig.retareTolG=prefs.getFloat("retare", activeConfig.retareTolG);
  activeConfig.standbyWakeThresholdG=prefs.getFloat("wake", activeConfig.standbyWakeThresholdG);
  activeConfig.oledTimingRefreshMs=prefs.getUInt("oledRef", activeConfig.oledTimingRefreshMs);
  activeConfig.scaleReadIntervalMs=prefs.getUInt("scaleInt", activeConfig.scaleReadIntervalMs);
  activeConfig.scaleReadSamples=prefs.getUChar("samples", activeConfig.scaleReadSamples);
  activeConfig.oledI2cClockHz=prefs.getUInt("i2c", activeConfig.oledI2cClockHz); activeConfig.oledRotation=prefs.getUChar("rot", activeConfig.oledRotation);
  activeConfig.oledScaleValue=prefs.getFloat("oledScale", activeConfig.oledScaleValue);
  activeConfig.debugMode=prefs.getBool("dbg", activeConfig.debugMode); activeConfig.oledDebugMode=prefs.getBool("odbg", activeConfig.oledDebugMode);
  activeConfig.pixelBrightnessPercent=prefs.getUChar("pixB", activeConfig.pixelBrightnessPercent); activeConfig.standbyBrightnessPercent=prefs.getUChar("stbyB", activeConfig.standbyBrightnessPercent); activeConfig.pixelDebugAllOn=prefs.getBool("pixDbg", activeConfig.pixelDebugAllOn);
  String dev=prefs.getString("dev", activeConfig.deviceId); strncpy(activeConfig.deviceId, dev.c_str(), sizeof(activeConfig.deviceId)-1);
  prefs.end();
  oledScale=activeConfig.oledScaleValue;
}

static String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in.charAt(i);
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else if (c == '\'') out += F("&#39;");
    else out += c;
  }
  return out;
}

static bool parseFloatArg(const char* key, float& out) {
  if (!server.hasArg(key)) return false;
  const String v = server.arg(key);
  if (v.length() == 0) return false;
  out = v.toFloat();
  return true;
}

static bool parseUIntArg(const char* key, uint32_t& out) {
  if (!server.hasArg(key)) return false;
  const String v = server.arg(key);
  if (v.length() == 0) return false;
  out = (uint32_t) v.toInt();
  return true;
}

static bool parseU8Arg(const char* key, uint8_t& out) {
  uint32_t tmp = 0;
  if (!parseUIntArg(key, tmp)) return false;
  out = (uint8_t) tmp;
  return true;
}

static bool parseBoolArg(const char* key, bool& out) {
  out = server.hasArg(key);
  return true;
}

static bool isIdleOrStandbyState() {
  return (state == State::IDLE_WAIT_GLASS || state == State::STANDBY);
}

static String renderConfigPage(const RuntimeConfig& c, const String& errorMsg = "") {
  String h;
  h.reserve(9000);
  h += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>Waage Config</title><style>body{font-family:Arial,sans-serif;max-width:760px;margin:12px auto;padding:0 10px;}fieldset{margin:12px 0;padding:10px;}label{display:block;margin:7px 0 3px;}input[type=text],input[type=number],select{width:100%;padding:8px;box-sizing:border-box;}small{color:#666;}button{padding:10px 14px;margin-top:10px;}.meta p{margin:4px 0;}.err{background:#ffdede;border:1px solid #cc3a3a;padding:8px;margin:10px 0;color:#7a0000;}</style></head><body>");
  h += F("<h2>Waage Config</h2><div class='meta'>");
  h += F("<p><b>IP:</b> "); h += htmlEscape(networkInfo); h += F("</p>");
  h += F("<p><b>Modus:</b> "); h += (wifiApMode ? F("Fallback-AP") : F("WLAN")); h += F("</p>");
  h += F("<p><b>Feste IP aktiv:</b> "); h += (WIFI_USE_STATIC_IP ? F("ja") : F("nein")); h += F("</p>");
  h += F("<p><b>Firmware:</b> "); h += FIRMWARE_VERSION; h += F("</p>");
  h += F("<p><b>State:</b> "); h += stateToStr(state); h += F("</p>");
  h += F("<p><b>Fehlerstatus:</b> "); h += errToStr(err); h += F("</p>");
  h += F("<p><b>Reset angefordert:</b> "); h += (resetRequested ? F("ja") : F("nein")); h += F("</p>");
  h += F("<p><b>Pending:</b> "); h += (pendingConfigValid ? F("ja") : F("nein")); h += F("</p>");
  h += F("<p><b>Hinweis:</b> "); h += (state==State::IDLE_WAIT_GLASS ? F("Aenderungen werden sofort aktiv") : F("Aenderungen warten bis Idle")); h += F("</p></div>");
  if (errorMsg.length()) { h += F("<div class='err'><b>Fehler:</b> "); h += htmlEscape(errorMsg); h += F("</div>"); }
  if (resetRequested) { h += F("<div class='err'><b>Reset:</b> Reset wartet auf sicheren Zustand</div>"); }
  if (state == State::TIMING) { h += F("<div class='err'><b>Info:</b> Reset wird erst nach der Messung ausgefuehrt</div>"); }
  if (resetStatusMsg.length()) { h += F("<div class='err'><b>Status:</b> "); h += htmlEscape(resetStatusMsg); h += F("</div>"); }
  h += F("<form method='POST' action='/reset-error'><button type='submit'>Fehlerreset / Neu nullen</button></form>");
  h += F("<form method='POST' action='/save'>");
  h += F("<fieldset><legend>System</legend><label>Device-ID</label><input name='deviceId' maxlength='31' value='"); h += htmlEscape(String(c.deviceId)); h += F("'><small>1 bis 31 Zeichen</small>");
  h += F("<label>Firmware-Version</label><input value='"); h += FIRMWARE_VERSION; h += F("' readonly>");
  h += F("<label>State</label><input value='"); h += stateToStr(state); h += F("' readonly></fieldset>");
  h += F("<fieldset><legend>Messung</legend>");
  h += F("<label>startDropPercent (%)</label><input type='number' step='0.1' min='0' max='100' name='startDropPercent' value='"); h += String(c.startDropPercent,2); h += F("'>");
  h += F("<label>stopRisePercent (%)</label><input type='number' step='0.1' min='0' max='100' name='stopRisePercent' value='"); h += String(c.stopRisePercent,2); h += F("'>");
  h += F("<label>objectPresentG (g)</label><input type='number' step='0.1' min='0' name='objectPresentG' value='"); h += String(c.objectPresentG,2); h += F("'>");
  h += F("<label>emptyThresholdG (g)</label><input type='number' step='0.1' min='0' name='emptyThresholdG' value='"); h += String(c.emptyThresholdG,2); h += F("'>");
  h += F("<label>retareTolG (g)</label><input type='number' step='0.1' min='0' name='retareTolG' value='"); h += String(c.retareTolG,2); h += F("'>");
  h += F("<label>standbyWakeThresholdG (g)</label><input type='number' step='0.1' min='0' name='standbyWakeThresholdG' value='"); h += String(c.standbyWakeThresholdG,2); h += F("'></fieldset>");
  h += F("<fieldset><legend>Performance</legend>");
  h += F("<label>oledTimingRefreshMs (ms)</label><input type='number' min='50' max='1000' name='oledTimingRefreshMs' value='"); h += String(c.oledTimingRefreshMs); h += F("'>");
  h += F("<label>scaleReadIntervalMs (ms)</label><input type='number' min='10' max='500' name='scaleReadIntervalMs' value='"); h += String(c.scaleReadIntervalMs); h += F("'>");
  h += F("<label>scaleReadSamples</label><input type='number' min='1' max='5' name='scaleReadSamples' value='"); h += String(c.scaleReadSamples); h += F("'>");
  h += F("<label>oledI2cClockHz (Hz)</label><select name='oledI2cClockHz'><option value='100000'");
  if (c.oledI2cClockHz == 100000) h += F(" selected"); h += F(">100000</option><option value='200000'");
  if (c.oledI2cClockHz == 200000) h += F(" selected"); h += F(">200000</option><option value='400000'");
  if (c.oledI2cClockHz == 400000) h += F(" selected"); h += F(">400000</option></select></fieldset>");
  h += F("<fieldset><legend>OLED</legend>");
  h += F("<label>oledRotation</label><input type='number' min='0' max='3' name='oledRotation' value='"); h += String(c.oledRotation); h += F("'>");
  h += F("<label>oledScaleValue</label><input type='number' step='0.1' min='0.1' name='oledScaleValue' value='"); h += String(c.oledScaleValue,2); h += F("'>");
  h += F("<label><input type='checkbox' name='debugMode' "); if (c.debugMode) h += F("checked"); h += F("> debugMode</label>");
  h += F("<label><input type='checkbox' name='oledDebugMode' "); if (c.oledDebugMode) h += F("checked"); h += F("> oledDebugMode</label></fieldset>");
  h += F("<fieldset><legend>Pixel</legend>");
  h += F("<label>pixelBrightnessPercent (%)</label><input type='number' min='0' max='100' name='pixelBrightnessPercent' value='"); h += String(c.pixelBrightnessPercent); h += F("'>");
  h += F("<label>standbyBrightnessPercent (%)</label><input type='number' min='0' max='100' name='standbyBrightnessPercent' value='"); h += String(c.standbyBrightnessPercent); h += F("'>");
  h += F("<label><input type='checkbox' name='pixelDebugAllOn' "); if (c.pixelDebugAllOn) h += F("checked"); h += F("> pixelDebugAllOn</label></fieldset>");
  h += F("<button type='submit'>Speichern</button></form></body></html>");
  return h;
}

static String renderBusyPage(const String& hint = "") {
  String h;
  h.reserve(1200);
  h += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>Waage aktiv</title><style>body{font-family:Arial,sans-serif;max-width:560px;margin:12px auto;padding:0 10px;}button{padding:10px 14px;margin-top:10px;}p{margin:8px 0;}.hint{background:#fff3cd;border:1px solid #d6b656;padding:8px;color:#6b5200;}</style></head><body>");
  h += F("<h2>Waage aktiv</h2>");
  h += F("<p><b>State:</b> "); h += stateToStr(state); h += F("</p>");
  h += F("<p>Config im Idle bearbeiten</p>");
  if (hint.length()) { h += F("<p class='hint'>"); h += htmlEscape(hint); h += F("</p>"); }
  if (resetStatusMsg.length()) { h += F("<p class='hint'><b>Status:</b> "); h += htmlEscape(resetStatusMsg); h += F("</p>"); }
  h += F("<form method='GET' action='/'><button type='submit'>Neu laden</button></form>");
  h += F("<form method='POST' action='/reset'><button type='submit'>Reset anfordern</button></form>");
  h += F("</body></html>");
  return h;
}

void setup() {
  Serial.begin(115200);
  loadDefaults();
  initOledScale();
  loadConfigFromPrefs();
  randomSeed(esp_random());

  ledsInit();
  oledInit();

  scale1.begin(HX1_DOUT, HX1_SCK);
  scale2.begin(HX2_DOUT, HX2_SCK);
  scale1.set_scale(DEFAULT_CAL1);
  scale2.set_scale(DEFAULT_CAL2);

  ledsSetMode(LedMode::RED_SOLID);
  oledMsg2("Start...", "Initialisierung");
  setState(State::BOOT_MSG);
  if (WEB_CONFIG_ENABLED) {
    (void)WIFI_STA_ENABLED;
    (void)WIFI_USE_STATIC_IP;
    (void)WIFI_SSID;
    (void)WIFI_PASSWORD;
    (void)WIFI_CONNECT_TIMEOUT_MS;
    (void)WIFI_LOCAL_IP;
    (void)WIFI_GATEWAY;
    (void)WIFI_SUBNET;
    (void)WIFI_DNS1;
    (void)WIFI_DNS2;
    WiFi.mode(WIFI_AP);
    const bool apStarted = WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASSWORD);
    wifiApMode = true;
    networkInfo = WiFi.softAPIP().toString();
    if (!apStarted) {
      Serial.println("[NET] Config AP Start fehlgeschlagen");
    }
    showNetworkStatus("Config AP", networkInfo);
    WiFi.setSleep(false);
    server.on("/", HTTP_GET, [](){
      if (isIdleOrStandbyState()) {
        server.send(200, "text/html", renderConfigPage(activeConfig));
      } else {
        server.send(200, "text/html", renderBusyPage());
      }
    });
    server.on("/save", HTTP_POST, [](){
      if (!isIdleOrStandbyState()) {
        server.send(409, "text/html", renderBusyPage("Speichern nur im Idle moeglich."));
        return;
      }
      RuntimeConfig n = activeConfig;
      String errMsg;
      String dev = server.arg("deviceId");
      dev.trim();
      if (dev.length() < 1 || dev.length() > 31) errMsg = "deviceId muss 1..31 Zeichen haben.";
      else { strncpy(n.deviceId, dev.c_str(), sizeof(n.deviceId)-1); n.deviceId[sizeof(n.deviceId)-1] = '\0'; }
      parseFloatArg("startDropPercent", n.startDropPercent);
      parseFloatArg("stopRisePercent", n.stopRisePercent);
      parseFloatArg("objectPresentG", n.objectPresentG);
      parseFloatArg("emptyThresholdG", n.emptyThresholdG);
      parseFloatArg("retareTolG", n.retareTolG);
      parseFloatArg("standbyWakeThresholdG", n.standbyWakeThresholdG);
      parseUIntArg("oledTimingRefreshMs", n.oledTimingRefreshMs);
      parseUIntArg("scaleReadIntervalMs", n.scaleReadIntervalMs);
      parseU8Arg("scaleReadSamples", n.scaleReadSamples);
      parseUIntArg("oledI2cClockHz", n.oledI2cClockHz);
      parseU8Arg("oledRotation", n.oledRotation);
      parseFloatArg("oledScaleValue", n.oledScaleValue);
      parseBoolArg("debugMode", n.debugMode);
      parseBoolArg("oledDebugMode", n.oledDebugMode);
      parseU8Arg("pixelBrightnessPercent", n.pixelBrightnessPercent);
      parseU8Arg("standbyBrightnessPercent", n.standbyBrightnessPercent);
      parseBoolArg("pixelDebugAllOn", n.pixelDebugAllOn);

      if (!errMsg.length() && (n.startDropPercent < 0 || n.startDropPercent > 100 || n.stopRisePercent < 0 || n.stopRisePercent > 100)) errMsg = "Prozentwerte muessen zwischen 0 und 100 liegen.";
      if (!errMsg.length() && (n.pixelBrightnessPercent > 100 || n.standbyBrightnessPercent > 100)) errMsg = "Helligkeit muss 0..100 sein.";
      if (!errMsg.length() && (n.scaleReadSamples < 1 || n.scaleReadSamples > 5)) errMsg = "scaleReadSamples muss 1..5 sein.";
      if (!errMsg.length() && n.oledRotation > 3) errMsg = "oledRotation muss 0..3 sein.";
      if (!errMsg.length() && (n.oledTimingRefreshMs < 50 || n.oledTimingRefreshMs > 1000)) errMsg = "oledTimingRefreshMs muss 50..1000 ms sein.";
      if (!errMsg.length() && (n.scaleReadIntervalMs < 10 || n.scaleReadIntervalMs > 500)) errMsg = "scaleReadIntervalMs muss 10..500 ms sein.";
      if (!errMsg.length() && !(n.oledI2cClockHz == 100000 || n.oledI2cClockHz == 200000 || n.oledI2cClockHz == 400000)) errMsg = "oledI2cClockHz muss 100000, 200000 oder 400000 sein.";
      if (!errMsg.length() && (n.objectPresentG <= 0 || n.emptyThresholdG < 0 || n.retareTolG <= 0 || n.standbyWakeThresholdG <= 0 || n.oledScaleValue <= 0)) errMsg = "Gramm- und OLED-Skalierungswerte muessen positiv und sinnvoll sein.";

      if (errMsg.length()) {
        server.send(400, "text/html", renderConfigPage(n, errMsg));
        return;
      }
      pendingConfig = n;
      pendingConfigValid = true;
      server.sendHeader("Location","/");
      server.send(303);
    });
    server.on("/reset-error", HTTP_POST, [](){
      resetRequested = true;
      resetStatusMsg = "Reset angefordert. Wird sicher ausgefuehrt.";
      server.sendHeader("Location","/");
      server.send(303);
    });
    server.on("/reset", HTTP_POST, [](){
      resetRequested = true;
      resetStatusMsg = "Reset angefordert. Wird sicher ausgefuehrt.";
      server.sendHeader("Location","/");
      server.send(303);
    });
    server.begin();
  }
}

static void stateMachineService(uint32_t now) {
  if (haveRead && shouldCheckNegativeError(state) && w_filt < NEGATIVE_ERROR_G) {
    setError(ErrCode::NEGATIVE);
  }

  const bool standbyDue = (now - lastActionMs) > STANDBY_AFTER_MS;

  switch (state) {
    case State::BOOT_MSG: {
      if (now - lastActionMs >= BOOT_MSG_MS) {
        oledMsg2("Nullung...", "Bitte nichts auflegen");
        // Nullung = alle rot dauerhaft.
        ledsSetMode(LedMode::RED_SOLID);
        setState(State::BOOT_TARE);
      }
      break;
    }

    case State::BOOT_TARE: {
      tareBoth();
      err = ErrCode::OK;
      objectPresent = false;

      oledMsg2("Nullung OK", "Warte auf Glas");
      ledsSetMode(LedMode::OK_ALT_GB);
      setState(State::IDLE_WAIT_GLASS);
      break;
    }

    case State::IDLE_WAIT_GLASS: {
      if (standbyDue) {
        oledMsg2("Standby", "Bewegung = Aktiv");
        ledsSetMode(LedMode::STANDBY_TWINKLE);
        setState(State::STANDBY);
        break;
      }

      ledsSetMode(LedMode::OK_ALT_GB);
      oledMsg2("Warte auf Glas", "...");

      if (haveRead && isObjectPresentStable(w_filt, isStable)) {
        objectPresent = true;
        readyReferenceWeight = w_filt;
        detectAtMs = now;
        oledMsg2("Glas erkannt", "");
        ledsSetMode(LedMode::GLASS_GREEN_SOLID);
        setState(State::GLASS_DETECTED);
      }
      break;
    }

    case State::GLASS_DETECTED: {
      if (objectMissingStable) {
        objectPresent = false;
        oledMsg2("Objekt weg", "Warte auf Glas");
        ledsSetMode(LedMode::OK_ALT_GB);
        setState(State::IDLE_WAIT_GLASS);
        break;
      }

      if (haveRead) {
        readyReferenceWeight = max(readyReferenceWeight, w_filt);
      }

      if (haveRead && (now - detectAtMs >= READY_AFTER_DETECT_MS)) {
        readyReferenceWeight = max(readyReferenceWeight, w_filt);
        refreshTimingThresholds();
        oledMsg2("Bereit fuer", "Zeitmessung");
        dropSinceMs = 0;
        stopCandidateSinceMs = 0;
        ledsSetMode(LedMode::READY_GREEN_BLINK);
        setState(State::READY_FOR_TIMING);
      }
      break;
    }

    case State::READY_FOR_TIMING: {
      const float w = w_filt;
      const float drop = readyReferenceWeight - w;

      if (objectMissingStable) {
        objectPresent = false;
        oledMsg2("Objekt weg", "Warte auf Glas");
        ledsSetMode(LedMode::OK_ALT_GB);
        setState(State::IDLE_WAIT_GLASS);
        break;
      }

      if (haveRead && drop >= startDropThresholdG) {
        if (dropSinceMs == 0) dropSinceMs = now;
        if (now - dropSinceMs >= DROP_HOLD_MS) {
          tStartMs = now;
          minDuringTiming = w;
          dropSinceMs = 0;
          stopCandidateSinceMs = 0;
          ledsSetMode(LedMode::TIMING_BLUE_SPINNER);
          setState(State::TIMING);
        }
      } else if (drop <= (startDropThresholdG - START_RESET_HYST_G)) {
        dropSinceMs = 0;
      }

      if (MASTER_DEBUG_LOG && haveRead && (now - lastSerialReadyMs >= SERIAL_STATE_REFRESH_MS)) {
        lastSerialReadyMs = now;
        Serial.print("[READY] w=");
        Serial.print(w, 2);
        Serial.print(" ref=");
        Serial.print(readyReferenceWeight, 2);
        Serial.print(" startThr=");
        Serial.print(startDropThresholdG, 2);
        Serial.print(" stopThr=");
        Serial.print(stopRiseThresholdG, 2);
        Serial.print(" drop=");
        Serial.println(drop, 2);
      }
      break;
    }

    case State::TIMING: {
      if (haveRead) {
        const float w = w_filt;

        // Minimum nur während "freiem Lauf" weiter nachführen.
        // Sobald ein Stop-Kandidat aktiv ist, wird das Minimum eingefroren,
        // damit die Schwelle nicht von Messrauschen "wegläuft".
        if (stopCandidateSinceMs == 0 && w < minDuringTiming) {
          minDuringTiming = w;
        }

        const float stopThreshold = minDuringTiming + stopRiseThresholdG;
        const float rebound = w - minDuringTiming;
        const bool aboveStopThreshold = (w >= stopThreshold);
        const uint32_t stopHoldMs = (stopCandidateSinceMs == 0) ? 0 : (now - stopCandidateSinceMs);

        if (aboveStopThreshold) {
          if (stopCandidateSinceMs == 0) stopCandidateSinceMs = now;
          if ((now - stopCandidateSinceMs) >= STOP_HOLD_MS) {
            finalizeTiming(now);
            break;
          }
        } else if (stopCandidateSinceMs != 0) {
          const float resetLimit = stopThreshold - STOP_RESET_HYST_G;
          if (w < resetLimit) {
            stopCandidateSinceMs = 0;
          }
        }

        if (MASTER_DEBUG_LOG && (now - lastSerialTimingMs >= SERIAL_STATE_REFRESH_MS)) {
          lastSerialTimingMs = now;
          Serial.print("[TIMING] w=");
          Serial.print(w, 2);
          Serial.print(" min=");
          Serial.print(minDuringTiming, 2);
          Serial.print(" stopThr=");
          Serial.print(stopThreshold, 2);
          Serial.print(" rebound=");
          Serial.print(rebound, 2);
          Serial.print(" candHold=");
          Serial.print(stopHoldMs);
          Serial.print("ms active=");
          Serial.println(stopCandidateSinceMs != 0 ? 1 : 0);
        }
      }
      break;
    }

    case State::SHOW_RESULT: {
      if (now >= showUntilMs) {
        oledMsg2("Bitte leeren", "Glas entfernen");
        setState(State::WAIT_EMPTY_AFTER_RESULT);
      }
      break;
    }

    case State::WAIT_EMPTY_AFTER_RESULT: {
      // Bitte freihalten / kein Glas aufstellen = alle rot dauerhaft.
      ledsSetMode(LedMode::RED_SOLID);
      const bool emptyAndStable = haveStableRead && absFilt < activeConfig.emptyThresholdG;
      if (!emptyAndStable) {
        oledMsg2("Bitte leeren", "Glas entfernen");
      } else {
        oledMsg2("Pruefe Nullpunkt", "...");
        setState(State::CHECK_RETARE);
      }
      break;
    }

    case State::CHECK_RETARE: {
      // Nullung = alle rot dauerhaft.
      ledsSetMode(LedMode::RED_SOLID);
      if (!haveStableRead) break;

      if (absFilt > activeConfig.retareTolG) {
        Serial.print("[RETARE] empty+stable offset=");
        Serial.print(w_filt, 2);
        Serial.println("g -> tare");
        oledMsg2("Nullung...", "Offset korr.");
        tareBoth();
      }

      oledMsg2("Warte auf Glas", "...");
      ledsSetMode(LedMode::OK_ALT_GB);
      objectPresent = false;
      setState(State::IDLE_WAIT_GLASS);
      break;
    }

    case State::STANDBY: {
      if (haveRead && absFilt > activeConfig.standbyWakeThresholdG) {
        oledMsg2("Aktiv", "Warte auf Glas");
        ledsSetMode(LedMode::OK_ALT_GB);
        setState(State::IDLE_WAIT_GLASS);
      }
      break;
    }

    case State::ERROR_RECOVER: {
      if (now >= recoverUntilMs) {
        if (haveStableRead && absFilt < 2.0f) {
          if ((now - lastSerialRecoverMs) >= SERIAL_STATE_REFRESH_MS) {
            lastSerialRecoverMs = now;
            Serial.println("[RECOVER] Tare (empty & stable).");
          }
          oledMsg2("Recovery", "Nullung...");
          tareBoth();
          err = ErrCode::OK;
          objectPresent = false;
          oledMsg2("OK", "Warte auf Glas");
          ledsSetMode(LedMode::OK_ALT_GB);
          setState(State::IDLE_WAIT_GLASS);
        } else {
          if ((now - lastSerialRecoverMs) >= SERIAL_STATE_REFRESH_MS) {
            lastSerialRecoverMs = now;
            Serial.println("[RECOVER] Waiting for empty/stable before tare...");
          }
          oledMsg2("Fehler", "Bitte leeren!");
          recoverUntilMs = now + 700;
        }
      }
      break;
    }
  }

}


static void webService(uint32_t now){
  if (!WEB_CONFIG_ENABLED) return;
  const bool isIdleState = isIdleOrStandbyState();
  const uint32_t interval = isIdleState ? WEB_SERVICE_INTERVAL_IDLE_MS : WEB_SERVICE_INTERVAL_BUSY_MS;
  if (now - lastWebServiceMs < interval) return;
  lastWebServiceMs = now;
  server.handleClient();
}

static void applyPendingConfigIfAllowed(){
  if (!(pendingConfigValid && state == State::IDLE_WAIT_GLASS)) return;
  activeConfig = pendingConfig;
  oledScale = activeConfig.oledScaleValue;
  Wire.setClock(activeConfig.oledI2cClockHz);
  display.setRotation(activeConfig.oledRotation);
  applyBrightnessForLedMode();
  DEBUG_MODE = activeConfig.debugMode;
  OLED_DEBUG_MODE = activeConfig.oledDebugMode;
  saveConfigToPrefs(activeConfig);
  pendingConfigValid = false;
}

static void handleResetRequestIfAllowed(uint32_t now){
  (void)now;
  if (!resetRequested) return;
  const bool canResetNow = (
    state == State::IDLE_WAIT_GLASS ||
    state == State::STANDBY ||
    state == State::WAIT_EMPTY_AFTER_RESULT ||
    state == State::ERROR_RECOVER
  );
  if (!canResetNow) {
    resetStatusMsg = "Reset wartet auf sicheren Zustand.";
    return;
  }

  err = ErrCode::OK;
  objectPresent = false;
  dropSinceMs = 0;
  stopCandidateSinceMs = 0;
  minDuringTiming = 1e9f;
  readyReferenceWeight = 0.0f;
  startDropThresholdG = 0.0f;
  stopRiseThresholdG = 0.0f;
  stableSince = 0;
  isStable = false;
  resetRequested = false;
  resetStatusMsg = "Reset ausgefuehrt. Nullung laeuft.";

  oledMsg2("Nullung...", "Bitte nichts auflegen");
  ledsSetMode(LedMode::RED_SOLID);
  setState(State::BOOT_TARE);
}

void loop() {
  const uint32_t now = millis();
  const uint32_t loopStartUs = PERFORMANCE_DEBUG ? micros() : 0;

  if (activeConfig.oledDebugMode) {
    if (!oledDebugLedsCleared) {
      pixelsClear();
      pixelsShow();
      oledDebugLedsCleared = true;
    }
    oledDebugPattern(now);
    delay(20);
    return;
  }
  oledDebugLedsCleared = false;

  const uint32_t ledStartUs = PERFORMANCE_DEBUG ? micros() : 0;
  ledService(now);
  const uint32_t ledDurUs = PERFORMANCE_DEBUG ? (micros() - ledStartUs) : 0;

  const uint32_t scaleStartUs = PERFORMANCE_DEBUG ? micros() : 0;
  scaleService(now);
  const uint32_t scaleDurUs = PERFORMANCE_DEBUG ? (micros() - scaleStartUs) : 0;

  const uint32_t stateStartUs = PERFORMANCE_DEBUG ? micros() : 0;
  stateMachineService(now);
  const uint32_t stateDurUs = PERFORMANCE_DEBUG ? (micros() - stateStartUs) : 0;

  const uint32_t oledStartUs = PERFORMANCE_DEBUG ? micros() : 0;
  oledService(now);
  const uint32_t oledDurUs = PERFORMANCE_DEBUG ? (micros() - oledStartUs) : 0;

  const uint32_t configStartUs = PERFORMANCE_DEBUG ? micros() : 0;
  applyPendingConfigIfAllowed();
  const uint32_t configDurUs = PERFORMANCE_DEBUG ? (micros() - configStartUs) : 0;

  const uint32_t resetStartUs = PERFORMANCE_DEBUG ? micros() : 0;
  handleResetRequestIfAllowed(now);
  const uint32_t resetDurUs = PERFORMANCE_DEBUG ? (micros() - resetStartUs) : 0;

  const uint32_t webStartUs = PERFORMANCE_DEBUG ? micros() : 0;
  webService(now);
  const uint32_t webDurUs = PERFORMANCE_DEBUG ? (micros() - webStartUs) : 0;

  if (PERFORMANCE_DEBUG) {
    const uint32_t loopDurUs = micros() - loopStartUs;
    perfMaxLedUs = max(perfMaxLedUs, ledDurUs);
    perfMaxScaleUs = max(perfMaxScaleUs, scaleDurUs);
    perfMaxStateUs = max(perfMaxStateUs, stateDurUs);
    perfMaxOledUs = max(perfMaxOledUs, oledDurUs);
    perfMaxConfigUs = max(perfMaxConfigUs, configDurUs);
    perfMaxResetUs = max(perfMaxResetUs, resetDurUs);
    perfMaxWebUs = max(perfMaxWebUs, webDurUs);
    perfMaxLoopUs = max(perfMaxLoopUs, loopDurUs);

    if (now - perfLastLogMs >= 1000) {
      perfLastLogMs = now;
      Serial.print("[PERF] scale=");
      Serial.print(scaleDurUs);
      Serial.print("us led=");
      Serial.print(ledDurUs);
      Serial.print("us state=");
      Serial.print(stateDurUs);
      Serial.print("us oled=");
      Serial.print(oledDurUs);
      Serial.print("us cfg=");
      Serial.print(configDurUs);
      Serial.print("us reset=");
      Serial.print(resetDurUs);
      Serial.print("us web=");
      Serial.print(webDurUs);
      Serial.print("us loop=");
      Serial.print(loopDurUs);
      Serial.print("us max(scale/led/state/oled/cfg/reset/web/loop)=");
      Serial.print(perfMaxScaleUs);
      Serial.print('/');
      Serial.print(perfMaxLedUs);
      Serial.print('/');
      Serial.print(perfMaxStateUs);
      Serial.print('/');
      Serial.print(perfMaxOledUs);
      Serial.print('/');
      Serial.print(perfMaxConfigUs);
      Serial.print('/');
      Serial.print(perfMaxResetUs);
      Serial.print('/');
      Serial.print(perfMaxWebUs);
      Serial.print('/');
      Serial.println(perfMaxLoopUs);
      perfMaxScaleUs = perfMaxLedUs = perfMaxStateUs = perfMaxOledUs = perfMaxConfigUs = perfMaxResetUs = perfMaxWebUs = perfMaxLoopUs = 0;
    }
  }

  if (activeConfig.debugMode) oledDebugWeights();
}
