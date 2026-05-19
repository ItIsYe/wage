#include "web_config_module.h"

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#include "config.h"
#include "display_module.h"
#include "external_interface_module.h"

extern State state;
extern ErrCode err;
extern RuntimeConfig activeConfig;

static const char* FIRMWARE_VERSION = "v1-webcfg";
static const char* errToStrLocal(ErrCode e);
static const char* stateToStrLocal(State s);

static RuntimeConfig pendingConfig;
static bool pendingConfigValid = false;
static bool wifiApMode = false;
static String networkInfo;
static WebServer server(WEB_SERVER_PORT);
static Preferences prefs;
static bool resetRequested = false;
static String resetStatusMsg;
static uint32_t lastWebServiceMs = 0;
static const uint32_t STANDBY_MIN_S = 5U;
static const uint32_t STANDBY_MAX_S = 300U;
static const uint32_t STANDBY_MIN_MS = STANDBY_MIN_S * 1000U;
static const uint32_t STANDBY_MAX_MS = STANDBY_MAX_S * 1000U;
static const uint32_t STANDBY_FRAME_MIN_MS = 30U;
static const uint32_t STANDBY_FRAME_MAX_MS = 1000U;
static const uint32_t STANDBY_CHANGE_MIN_LIMIT_MS = 100U;
static const uint32_t STANDBY_CHANGE_MAX_LIMIT_MS = 5000U;

static void disableDebugModesNow() {
  activeConfig.oledDebugMode = false;
  activeConfig.debugMode = false;
  activeConfig.pixelDebugAllOn = false;
  activeConfig.ring2DebugAllOn = false;
  pendingConfig = activeConfig;
  pendingConfigValid = false;
  webConfigSaveToPrefs(activeConfig);
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

static bool isConfigApplyAllowedState() {
  return (state == State::IDLE_WAIT_GLASS);
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
  h += F("<p><b>State:</b> "); h += stateToStrLocal(state); h += F("</p>");
  h += F("<p><b>Fehlerstatus:</b> "); h += errToStrLocal(err); h += F("</p>");
  h += F("<p><b>Reset angefordert:</b> "); h += (resetRequested ? F("ja") : F("nein")); h += F("</p>");
  h += F("<p><b>Pending:</b> "); h += (pendingConfigValid ? F("ja") : F("nein")); h += F("</p>");
  h += F("<p><b>Hinweis:</b> "); h += (state==State::IDLE_WAIT_GLASS ? F("Aenderungen werden sofort aktiv") : F("Aenderungen warten bis Idle")); h += F("</p></div>");
  if (errorMsg.length()) { h += F("<div class='err'><b>Fehler:</b> "); h += htmlEscape(errorMsg); h += F("</div>"); }
  if (resetRequested) { h += F("<div class='err'><b>Reset:</b> Reset wartet auf sicheren Zustand</div>"); }
  if (state == State::TIMING) { h += F("<div class='err'><b>Info:</b> Reset wird erst nach der Messung ausgefuehrt</div>"); }
  if (resetStatusMsg.length()) { h += F("<div class='err'><b>Status:</b> "); h += htmlEscape(resetStatusMsg); h += F("</div>"); }
  h += F("<form method='POST' action='/reset-error'><button type='submit'>Fehlerreset / Neu nullen</button></form>");
  h += F("<form method='POST' action='/debug/off'><button type='submit'>Alle Debug-Modi deaktivieren</button></form>");
  h += F("<p><a href='/debug/off'>Alle Debug-Modi per Link deaktivieren</a></p>");
  h += F("<form method='POST' action='/save'>");
  h += F("<fieldset><legend>System</legend><label>Device-ID</label><input name='deviceId' maxlength='31' value='"); h += htmlEscape(String(c.deviceId)); h += F("'><small>1 bis 31 Zeichen</small>");
  h += F("<label>Firmware-Version</label><input value='"); h += FIRMWARE_VERSION; h += F("' readonly>");
  h += F("<label>State</label><input value='"); h += stateToStrLocal(state); h += F("' readonly></fieldset>");
  h += F("<fieldset><legend>Messung</legend>");
  h += F("<label>startDropPercent (%)</label><input type='number' step='0.1' min='0' max='100' name='startDropPercent' value='"); h += String(c.startDropPercent,2); h += F("'>");
  h += F("<label>stopRisePercent (%)</label><input type='number' step='0.1' min='0' max='100' name='stopRisePercent' value='"); h += String(c.stopRisePercent,2); h += F("'>");
  h += F("<label>objectPresentG (g)</label><input type='number' step='0.1' min='0' name='objectPresentG' value='"); h += String(c.objectPresentG,2); h += F("'>");
  h += F("<label>emptyThresholdG (g)</label><input type='number' step='0.1' min='0' name='emptyThresholdG' value='"); h += String(c.emptyThresholdG,2); h += F("'>");
  h += F("<label>retareTolG (g)</label><input type='number' step='0.1' min='0' name='retareTolG' value='"); h += String(c.retareTolG,2); h += F("'>");
  h += F("<label>standbyWakeThresholdG (g)</label><input type='number' step='0.1' min='0' name='standbyWakeThresholdG' value='"); h += String(c.standbyWakeThresholdG,2); h += F("'>");
  h += F("<label>Standby nach (s)</label><input type='number' min='5' max='300' name='standbyAfterS' value='"); h += String(c.standbyAfterMs / 1000U); h += F("'></fieldset>");
  h += F("<fieldset><legend>Ring 1 Standby-Twinkle</legend>");
  h += F("<label>Standby Twinkle Speed / Frame-Zeit (ms)</label><input type='number' min='30' max='1000' name='standbyFrameMs' value='"); h += String(c.standbyFrameMs); h += F("'>");
  h += F("<label>Standby Change Min (ms)</label><input type='number' min='100' max='5000' name='standbyChangeMinMs' value='"); h += String(c.standbyChangeMinMs); h += F("'>");
  h += F("<label>Standby Change Max (ms)</label><input type='number' min='100' max='5000' name='standbyChangeMaxMs' value='"); h += String(c.standbyChangeMaxMs); h += F("'>");
  h += F("<label>Standby Twinkle Anzahl Min</label><input type='number' min='0' max='25' name='standbyOnMin' value='"); h += String(c.standbyOnMin); h += F("'>");
  h += F("<label>Standby Twinkle Anzahl Max</label><input type='number' min='0' max='25' name='standbyOnMax' value='"); h += String(c.standbyOnMax); h += F("'>");
  h += F("<label>Standby Value Min</label><input type='number' min='0' max='255' name='standbyValueMin' value='"); h += String(c.standbyValueMin); h += F("'>");
  h += F("<label>Standby Value Max</label><input type='number' min='0' max='255' name='standbyValueMax' value='"); h += String(c.standbyValueMax); h += F("'>");
  h += F("<label>Standby Saturation</label><input type='number' min='0' max='255' name='standbySaturation' value='"); h += String(c.standbySaturation); h += F("'></fieldset>");
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
  h += F("<label><input type='checkbox' name='debugMode' "); if (c.debugMode) h += F("checked"); h += F("> Gewichts-Debug auf OLED anzeigen</label>");
  h += F("<label><input type='checkbox' name='oledDebugMode' "); if (c.oledDebugMode) h += F("checked"); h += F("> OLED-Pixel-Debug anzeigen</label></fieldset>");
  h += F("<fieldset><legend>Ring 1 / Haupt-LED-Ring</legend>");
  h += F("<label>pixelBrightnessPercent (%)</label><input type='number' min='0' max='100' name='pixelBrightnessPercent' value='"); h += String(c.pixelBrightnessPercent); h += F("'>");
  h += F("<label>standbyBrightnessPercent (%)</label><input type='number' min='0' max='100' name='standbyBrightnessPercent' value='"); h += String(c.standbyBrightnessPercent); h += F("'>");
  h += F("<label><input type='checkbox' name='pixelDebugAllOn' "); if (c.pixelDebugAllOn) h += F("checked"); h += F("> Ring 1 Debug: alle Pixel an</label></fieldset>");
  h += F("<fieldset><legend>Ring 2 / Zusatz-LED-Ring</legend>");
  h += F("<label><input type='checkbox' name='ring2Enabled' "); if (c.ring2Enabled) h += F("checked"); h += F("> Ring 2 aktiv</label>");
  h += F("<label>Ring 2 Helligkeit (%)</label><input type='number' min='0' max='100' name='ring2BrightnessPercent' value='"); h += String(c.ring2BrightnessPercent); h += F("'>");
  h += F("<label>Ring 2 Standby-Helligkeit (%)</label><input type='number' min='0' max='100' name='ring2StandbyBrightnessPercent' value='"); h += String(c.ring2StandbyBrightnessPercent); h += F("'>");
  h += F("<label><input type='checkbox' name='ring2DebugAllOn' "); if (c.ring2DebugAllOn) h += F("checked"); h += F("> Ring 2 Debug alle Pixel an</label>");
  h += F("<label>Ring 2 Pattern</label><select name='ring2PatternMode'><option value='0'"); if (c.ring2PatternMode == 0) h += F(" selected"); h += F(">Aus</option><option value='1'"); if (c.ring2PatternMode == 1) h += F(" selected"); h += F(">Solid Blau</option><option value='2'"); if (c.ring2PatternMode == 2) h += F(" selected"); h += F(">Pulse Blau</option><option value='3'"); if (c.ring2PatternMode == 3) h += F(" selected"); h += F(">Breathing Weiss</option><option value='4'"); if (c.ring2PatternMode == 4) h += F(" selected"); h += F(">Slow Blue Spinner</option></select></fieldset>");

  h += F("<fieldset><legend>Externe Schnittstelle</legend>");
  h += F("<label><input type='checkbox' name='externalEnabled' "); if (c.externalEnabled) h += F("checked"); h += F("> Externe Schnittstelle aktiv</label>");
  h += F("<label>Ziel-Host / IP</label><input name='externalHost' maxlength='63' value='"); h += htmlEscape(String(c.externalHost)); h += F("'>");
  h += F("<label>Port</label><input type='number' min='1' max='65535' name='externalPort' value='"); h += String(c.externalPort); h += F("'>");
  h += F("<label>API-Pfad</label><input name='externalApiPath' maxlength='63' value='"); h += htmlEscape(String(c.externalApiPath)); h += F("'><small>z.B. /api/v1/runs</small>");
  h += F("<label>API-Key</label><input name='externalApiKey' maxlength='63' value='"); h += htmlEscape(String(c.externalApiKey)); h += F("'>");
  h += F("<label>Queue-Tiefe</label><input value='"); h += String(externalInterfaceQueueDepth()); h += F("' readonly>");
  h += F("<label>Letzter Sendestatus</label><input value='"); h += htmlEscape(String(externalInterfaceLastStatus())); h += F("' readonly></fieldset>");

  h += F("<button type='submit'>Speichern</button></form></body></html>");
  return h;
}

static String renderBusyPage(const String& hint = "") {
  String h;
  h.reserve(1200);
  h += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>Waage aktiv</title><style>body{font-family:Arial,sans-serif;max-width:560px;margin:12px auto;padding:0 10px;}button{padding:10px 14px;margin-top:10px;}p{margin:8px 0;}.hint{background:#fff3cd;border:1px solid #d6b656;padding:8px;color:#6b5200;}</style></head><body>");
  h += F("<h2>Waage aktiv</h2>");
  h += F("<p><b>State:</b> "); h += stateToStrLocal(state); h += F("</p>");
  h += F("<p>Config im Idle bearbeiten</p>");
  if (hint.length()) { h += F("<p class='hint'>"); h += htmlEscape(hint); h += F("</p>"); }
  if (resetStatusMsg.length()) { h += F("<p class='hint'><b>Status:</b> "); h += htmlEscape(resetStatusMsg); h += F("</p>"); }
  h += F("<form method='GET' action='/'><button type='submit'>Neu laden</button></form>");
  h += F("<form method='POST' action='/reset'><button type='submit'>Reset anfordern</button></form>");
  h += F("<form method='POST' action='/debug/off'><button type='submit'>Alle Debug-Modi deaktivieren</button></form>");
  h += F("<p><a href='/debug/off'>Alle Debug-Modi per Link deaktivieren</a></p>");
  h += F("</body></html>");
  return h;
}

void webConfigLoadDefaults(RuntimeConfig& cfg) { memset(&cfg,0,sizeof(cfg));
  cfg.startDropPercent=DEFAULT_START_DROP_PERCENT; cfg.stopRisePercent=DEFAULT_STOP_RISE_PERCENT;
  cfg.objectPresentG=DEFAULT_OBJECT_PRESENT_G; cfg.emptyThresholdG=DEFAULT_EMPTY_THRESHOLD_G;
  cfg.retareTolG=DEFAULT_RETARE_TOL_G; cfg.standbyWakeThresholdG=DEFAULT_STANDBY_WAKE_THRESHOLD_G; cfg.standbyAfterMs=STANDBY_AFTER_MS;
  cfg.standbyFrameMs=STANDBY_FRAME_MS; cfg.standbyChangeMinMs=STANDBY_CHANGE_MIN_MS; cfg.standbyChangeMaxMs=STANDBY_CHANGE_MAX_MS;
  cfg.standbySaturation=STANDBY_SATURATION; cfg.standbyValueMin=STANDBY_VALUE_MIN; cfg.standbyValueMax=STANDBY_VALUE_MAX;
  cfg.standbyOnMin=STANDBY_ON_MIN; cfg.standbyOnMax=STANDBY_ON_MAX;
  cfg.oledTimingRefreshMs=DEFAULT_OLED_TIMING_REFRESH_MS; cfg.scaleReadIntervalMs=DEFAULT_SCALE_READ_INTERVAL_MS;
  cfg.scaleReadSamples=DEFAULT_SCALE_READ_SAMPLES; cfg.oledI2cClockHz=DEFAULT_OLED_I2C_CLOCK_HZ;
  cfg.oledRotation=DEFAULT_OLED_ROTATION; cfg.oledScaleValue=1.5f; cfg.debugMode=false; cfg.oledDebugMode=false;
  cfg.pixelBrightnessPercent=DEFAULT_PIXEL_BRIGHTNESS_PERCENT; cfg.standbyBrightnessPercent=DEFAULT_STANDBY_BRIGHTNESS_PERCENT;
  cfg.pixelDebugAllOn=DEFAULT_PIXEL_DEBUG_ALL_ON;
  cfg.ring2Enabled=DEFAULT_RING2_ENABLED; cfg.ring2BrightnessPercent=DEFAULT_RING2_BRIGHTNESS_PERCENT;
  cfg.ring2StandbyBrightnessPercent=DEFAULT_RING2_STANDBY_BRIGHTNESS_PERCENT; cfg.ring2DebugAllOn=DEFAULT_RING2_DEBUG_ALL_ON;
  cfg.ring2PatternMode=DEFAULT_RING2_PATTERN_MODE; strncpy(cfg.deviceId,"waage-01",sizeof(cfg.deviceId)-1);
  cfg.externalEnabled=EXTERNAL_INTERFACE_ENABLED_DEFAULT;
  strncpy(cfg.externalHost, EXTERNAL_TARGET_HOST_DEFAULT, sizeof(cfg.externalHost)-1);
  cfg.externalPort=EXTERNAL_TARGET_PORT_DEFAULT;
  strncpy(cfg.externalApiPath, EXTERNAL_API_PATH_DEFAULT, sizeof(cfg.externalApiPath)-1);
  strncpy(cfg.externalApiKey, EXTERNAL_API_KEY_DEFAULT, sizeof(cfg.externalApiKey)-1);}

void webConfigSaveToPrefs(const RuntimeConfig& c){
  prefs.begin("cfg", false);
  prefs.putFloat("startDrop", c.startDropPercent); prefs.putFloat("stopRise", c.stopRisePercent);
  prefs.putFloat("obj", c.objectPresentG); prefs.putFloat("empty", c.emptyThresholdG); prefs.putFloat("retare", c.retareTolG);
  prefs.putFloat("wake", c.standbyWakeThresholdG); prefs.putUInt("stbyAfter", c.standbyAfterMs); prefs.putUInt("oledRef", c.oledTimingRefreshMs); prefs.putUInt("scaleInt", c.scaleReadIntervalMs);
  prefs.putUInt("stbyFrm", c.standbyFrameMs); prefs.putUInt("stbyCMin", c.standbyChangeMinMs); prefs.putUInt("stbyCMax", c.standbyChangeMaxMs);
  prefs.putUChar("stbySat", c.standbySaturation); prefs.putUChar("stbyVMin", c.standbyValueMin); prefs.putUChar("stbyVMax", c.standbyValueMax);
  prefs.putUChar("stbyOMin", c.standbyOnMin); prefs.putUChar("stbyOMax", c.standbyOnMax);
  prefs.putUChar("samples", c.scaleReadSamples); prefs.putUInt("i2c", c.oledI2cClockHz); prefs.putUChar("rot", c.oledRotation);
  prefs.putFloat("oledScale", c.oledScaleValue); prefs.putBool("dbg", c.debugMode); prefs.putBool("odbg", c.oledDebugMode);
  prefs.putUChar("pixB", c.pixelBrightnessPercent); prefs.putUChar("stbyB", c.standbyBrightnessPercent); prefs.putBool("pixDbg", c.pixelDebugAllOn);
  prefs.putBool("r2en", c.ring2Enabled); prefs.putUChar("r2b", c.ring2BrightnessPercent); prefs.putUChar("r2sb", c.ring2StandbyBrightnessPercent);
  prefs.putBool("r2dbg", c.ring2DebugAllOn); prefs.putUChar("r2pat", c.ring2PatternMode);
  prefs.putString("dev", c.deviceId);
  prefs.putBool("exEn", c.externalEnabled);
  prefs.putString("exHost", c.externalHost);
  prefs.putUInt("exPort", c.externalPort);
  prefs.putString("exPath", c.externalApiPath);
  prefs.putString("exKey", c.externalApiKey);
  prefs.end();
}

void webConfigLoadFromPrefs(RuntimeConfig& cfg, float& oledScale){
  prefs.begin("cfg", true);
  cfg.startDropPercent=prefs.getFloat("startDrop", cfg.startDropPercent);
  cfg.stopRisePercent=prefs.getFloat("stopRise", cfg.stopRisePercent);
  cfg.objectPresentG=prefs.getFloat("obj", cfg.objectPresentG);
  cfg.emptyThresholdG=prefs.getFloat("empty", cfg.emptyThresholdG);
  cfg.retareTolG=prefs.getFloat("retare", cfg.retareTolG);
  cfg.standbyWakeThresholdG=prefs.getFloat("wake", cfg.standbyWakeThresholdG);
  cfg.standbyAfterMs=prefs.getUInt("stbyAfter", cfg.standbyAfterMs);
  cfg.standbyFrameMs=prefs.getUInt("stbyFrm", cfg.standbyFrameMs);
  cfg.standbyChangeMinMs=prefs.getUInt("stbyCMin", cfg.standbyChangeMinMs);
  cfg.standbyChangeMaxMs=prefs.getUInt("stbyCMax", cfg.standbyChangeMaxMs);
  cfg.standbySaturation=prefs.getUChar("stbySat", cfg.standbySaturation);
  cfg.standbyValueMin=prefs.getUChar("stbyVMin", cfg.standbyValueMin);
  cfg.standbyValueMax=prefs.getUChar("stbyVMax", cfg.standbyValueMax);
  cfg.standbyOnMin=prefs.getUChar("stbyOMin", cfg.standbyOnMin);
  cfg.standbyOnMax=prefs.getUChar("stbyOMax", cfg.standbyOnMax);
  if (cfg.standbyAfterMs < STANDBY_MIN_MS) cfg.standbyAfterMs = STANDBY_MIN_MS;
  if (cfg.standbyAfterMs > STANDBY_MAX_MS) cfg.standbyAfterMs = STANDBY_MAX_MS;
  if (cfg.standbyFrameMs < STANDBY_FRAME_MIN_MS) cfg.standbyFrameMs = STANDBY_FRAME_MIN_MS;
  if (cfg.standbyFrameMs > STANDBY_FRAME_MAX_MS) cfg.standbyFrameMs = STANDBY_FRAME_MAX_MS;
  if (cfg.standbyChangeMinMs < STANDBY_CHANGE_MIN_LIMIT_MS) cfg.standbyChangeMinMs = STANDBY_CHANGE_MIN_LIMIT_MS;
  if (cfg.standbyChangeMinMs > STANDBY_CHANGE_MAX_LIMIT_MS) cfg.standbyChangeMinMs = STANDBY_CHANGE_MAX_LIMIT_MS;
  if (cfg.standbyChangeMaxMs < STANDBY_CHANGE_MIN_LIMIT_MS) cfg.standbyChangeMaxMs = STANDBY_CHANGE_MIN_LIMIT_MS;
  if (cfg.standbyChangeMaxMs > STANDBY_CHANGE_MAX_LIMIT_MS) cfg.standbyChangeMaxMs = STANDBY_CHANGE_MAX_LIMIT_MS;
  if (cfg.standbyOnMin > PIXEL_COUNT) cfg.standbyOnMin = PIXEL_COUNT;
  if (cfg.standbyOnMax > PIXEL_COUNT) cfg.standbyOnMax = PIXEL_COUNT;
  cfg.oledTimingRefreshMs=prefs.getUInt("oledRef", cfg.oledTimingRefreshMs);
  cfg.scaleReadIntervalMs=prefs.getUInt("scaleInt", cfg.scaleReadIntervalMs);
  cfg.scaleReadSamples=prefs.getUChar("samples", cfg.scaleReadSamples);
  cfg.oledI2cClockHz=prefs.getUInt("i2c", cfg.oledI2cClockHz); cfg.oledRotation=prefs.getUChar("rot", cfg.oledRotation);
  cfg.oledScaleValue=prefs.getFloat("oledScale", cfg.oledScaleValue);
  cfg.debugMode=prefs.getBool("dbg", cfg.debugMode); cfg.oledDebugMode=prefs.getBool("odbg", cfg.oledDebugMode);
  cfg.pixelBrightnessPercent=prefs.getUChar("pixB", cfg.pixelBrightnessPercent); cfg.standbyBrightnessPercent=prefs.getUChar("stbyB", cfg.standbyBrightnessPercent); cfg.pixelDebugAllOn=prefs.getBool("pixDbg", cfg.pixelDebugAllOn);
  cfg.ring2Enabled=prefs.getBool("r2en", cfg.ring2Enabled); cfg.ring2BrightnessPercent=prefs.getUChar("r2b", cfg.ring2BrightnessPercent); cfg.ring2StandbyBrightnessPercent=prefs.getUChar("r2sb", cfg.ring2StandbyBrightnessPercent);
  cfg.ring2DebugAllOn=prefs.getBool("r2dbg", cfg.ring2DebugAllOn); cfg.ring2PatternMode=prefs.getUChar("r2pat", cfg.ring2PatternMode);
  if (cfg.ring2BrightnessPercent > 100) cfg.ring2BrightnessPercent = 100;
  if (cfg.ring2StandbyBrightnessPercent > 100) cfg.ring2StandbyBrightnessPercent = 100;
  if (cfg.ring2PatternMode > 4) cfg.ring2PatternMode = DEFAULT_RING2_PATTERN_MODE;
  String dev=prefs.getString("dev", cfg.deviceId); strncpy(cfg.deviceId, dev.c_str(), sizeof(cfg.deviceId)-1);
  cfg.externalEnabled=prefs.getBool("exEn", cfg.externalEnabled);
  String exHost=prefs.getString("exHost", cfg.externalHost); strncpy(cfg.externalHost, exHost.c_str(), sizeof(cfg.externalHost)-1);
  cfg.externalPort=(uint16_t)prefs.getUInt("exPort", cfg.externalPort);
  String exPath=prefs.getString("exPath", cfg.externalApiPath); strncpy(cfg.externalApiPath, exPath.c_str(), sizeof(cfg.externalApiPath)-1);
  String exKey=prefs.getString("exKey", cfg.externalApiKey); strncpy(cfg.externalApiKey, exKey.c_str(), sizeof(cfg.externalApiKey)-1);
  prefs.end();
  oledScale=cfg.oledScaleValue;
}

void webConfigSetup() {
  if (!WEB_CONFIG_ENABLED) return;
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
  if (!apStarted) Serial.println("[NET] Config AP Start fehlgeschlagen");
  showNetworkStatus("Config AP", networkInfo);
  WiFi.setSleep(false);

  server.on("/", HTTP_GET, [](){
    if (isConfigApplyAllowedState()) server.send(200, "text/html", renderConfigPage(activeConfig));
    else server.send(200, "text/html", renderBusyPage());
  });
  server.on("/save", HTTP_POST, [](){
    if (!isConfigApplyAllowedState()) {
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
    uint32_t standbyAfterS = n.standbyAfterMs / 1000U;
    parseUIntArg("standbyAfterS", standbyAfterS);
    if (standbyAfterS < STANDBY_MIN_S) standbyAfterS = STANDBY_MIN_S;
    if (standbyAfterS > STANDBY_MAX_S) standbyAfterS = STANDBY_MAX_S;
    n.standbyAfterMs = standbyAfterS * 1000U;
    parseUIntArg("standbyFrameMs", n.standbyFrameMs);
    parseUIntArg("standbyChangeMinMs", n.standbyChangeMinMs);
    parseUIntArg("standbyChangeMaxMs", n.standbyChangeMaxMs);
    parseU8Arg("standbyOnMin", n.standbyOnMin);
    parseU8Arg("standbyOnMax", n.standbyOnMax);
    parseU8Arg("standbyValueMin", n.standbyValueMin);
    parseU8Arg("standbyValueMax", n.standbyValueMax);
    parseU8Arg("standbySaturation", n.standbySaturation);
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
    parseBoolArg("ring2Enabled", n.ring2Enabled);
    parseU8Arg("ring2BrightnessPercent", n.ring2BrightnessPercent);
    parseU8Arg("ring2StandbyBrightnessPercent", n.ring2StandbyBrightnessPercent);
    parseBoolArg("ring2DebugAllOn", n.ring2DebugAllOn);
    parseU8Arg("ring2PatternMode", n.ring2PatternMode);
    parseBoolArg("externalEnabled", n.externalEnabled);
    String extHost = server.arg("externalHost"); extHost.trim(); strncpy(n.externalHost, extHost.c_str(), sizeof(n.externalHost)-1); n.externalHost[sizeof(n.externalHost)-1] = '\0';
    uint32_t extPort = n.externalPort; parseUIntArg("externalPort", extPort); n.externalPort = (uint16_t) extPort;
    String extPath = server.arg("externalApiPath"); extPath.trim(); strncpy(n.externalApiPath, extPath.c_str(), sizeof(n.externalApiPath)-1); n.externalApiPath[sizeof(n.externalApiPath)-1] = '\0';
    String extKey = server.arg("externalApiKey"); extKey.trim(); strncpy(n.externalApiKey, extKey.c_str(), sizeof(n.externalApiKey)-1); n.externalApiKey[sizeof(n.externalApiKey)-1] = '\0';

    if (!errMsg.length() && (n.startDropPercent < 0 || n.startDropPercent > 100 || n.stopRisePercent < 0 || n.stopRisePercent > 100)) errMsg = "Prozentwerte muessen zwischen 0 und 100 liegen.";
    if (!errMsg.length() && (n.pixelBrightnessPercent > 100 || n.standbyBrightnessPercent > 100)) errMsg = "Helligkeit muss 0..100 sein.";
    if (!errMsg.length() && (n.ring2BrightnessPercent > 100 || n.ring2StandbyBrightnessPercent > 100)) errMsg = "Ring-2-Helligkeit muss 0..100 sein.";
    if (!errMsg.length() && n.ring2PatternMode > 4) errMsg = "Ring-2-Pattern muss 0..4 sein.";
    if (!errMsg.length() && (n.scaleReadSamples < 1 || n.scaleReadSamples > 5)) errMsg = "scaleReadSamples muss 1..5 sein.";
    if (!errMsg.length() && n.oledRotation > 3) errMsg = "oledRotation muss 0..3 sein.";
    if (!errMsg.length() && (n.oledTimingRefreshMs < 50 || n.oledTimingRefreshMs > 1000)) errMsg = "oledTimingRefreshMs muss 50..1000 ms sein.";
    if (!errMsg.length() && (n.scaleReadIntervalMs < 10 || n.scaleReadIntervalMs > 500)) errMsg = "scaleReadIntervalMs muss 10..500 ms sein.";
    if (!errMsg.length() && (n.standbyAfterMs < STANDBY_MIN_MS || n.standbyAfterMs > STANDBY_MAX_MS)) errMsg = "Standby nach muss 5..300 s sein.";
    if (!errMsg.length() && (n.standbyFrameMs < STANDBY_FRAME_MIN_MS || n.standbyFrameMs > STANDBY_FRAME_MAX_MS)) errMsg = "Standby Frame-Zeit muss 30..1000 ms sein.";
    if (!errMsg.length() && (n.standbyChangeMinMs < STANDBY_CHANGE_MIN_LIMIT_MS || n.standbyChangeMinMs > STANDBY_CHANGE_MAX_LIMIT_MS)) errMsg = "Standby Change Min muss 100..5000 ms sein.";
    if (!errMsg.length() && (n.standbyChangeMaxMs < STANDBY_CHANGE_MIN_LIMIT_MS || n.standbyChangeMaxMs > STANDBY_CHANGE_MAX_LIMIT_MS)) errMsg = "Standby Change Max muss 100..5000 ms sein.";
    if (!errMsg.length() && n.standbyChangeMinMs > n.standbyChangeMaxMs) errMsg = "Standby Change Min darf nicht groesser als Max sein.";
    if (!errMsg.length() && n.standbyOnMin > PIXEL_COUNT) errMsg = "Standby Anzahl Min darf Ringgroesse nicht ueberschreiten.";
    if (!errMsg.length() && n.standbyOnMax > PIXEL_COUNT) errMsg = "Standby Anzahl Max darf Ringgroesse nicht ueberschreiten.";
    if (!errMsg.length() && n.standbyOnMin > n.standbyOnMax) errMsg = "Standby Anzahl Min darf nicht groesser als Max sein.";
    if (!errMsg.length() && n.standbyValueMin > n.standbyValueMax) errMsg = "Standby Value Min darf nicht groesser als Max sein.";
    if (!errMsg.length() && !(n.oledI2cClockHz == 100000 || n.oledI2cClockHz == 200000 || n.oledI2cClockHz == 400000)) errMsg = "oledI2cClockHz muss 100000, 200000 oder 400000 sein.";
    if (!errMsg.length() && (n.objectPresentG <= 0 || n.emptyThresholdG < 0 || n.retareTolG <= 0 || n.standbyWakeThresholdG <= 0 || n.oledScaleValue <= 0)) errMsg = "Gramm- und OLED-Skalierungswerte muessen positiv und sinnvoll sein.";
    if (!errMsg.length() && n.externalPort == 0) errMsg = "externalPort muss 1..65535 sein.";
    if (!errMsg.length() && n.externalEnabled && strlen(n.externalHost) == 0) errMsg = "externalHost darf bei aktivierter Schnittstelle nicht leer sein.";
    if (!errMsg.length() && n.externalEnabled && strlen(n.externalApiPath) == 0) errMsg = "externalApiPath darf bei aktivierter Schnittstelle nicht leer sein.";

    if (errMsg.length()) { server.send(400, "text/html", renderConfigPage(n, errMsg)); return; }
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
  auto disableDebugModesHandler = [](){
    disableDebugModesNow();
    server.send(200, "text/plain; charset=utf-8", "Debug deaktiviert");
  };
  server.on("/debug/off", HTTP_GET, disableDebugModesHandler);
  server.on("/debug/off", HTTP_POST, disableDebugModesHandler);
  server.begin();
}

void webService(uint32_t now){
  if (!WEB_CONFIG_ENABLED) return;
  const bool isIdleState = (state == State::IDLE_WAIT_GLASS || state == State::STANDBY);
  const uint32_t interval = isIdleState ? WEB_SERVICE_INTERVAL_IDLE_MS : WEB_SERVICE_INTERVAL_BUSY_MS;
  if (now - lastWebServiceMs < interval) return;
  lastWebServiceMs = now;
  server.handleClient();
}

bool webHasPendingConfig() { return pendingConfigValid; }
RuntimeConfig webGetPendingConfig() { return pendingConfig; }
void webClearPendingConfig() { pendingConfigValid = false; }
bool webIsResetRequested() { return resetRequested; }
void webClearResetRequested() { resetRequested = false; }
void webSetResetStatusMsg(const String& msg) { resetStatusMsg = msg; }
String webGetResetStatusMsg() { return resetStatusMsg; }
bool webIsWifiApMode() { return wifiApMode; }
String webGetNetworkInfo() { return networkInfo; }
static const char* errToStrLocal(ErrCode e) {
  switch (e) {
    case ErrCode::OK: return "OK";
    case ErrCode::NEGATIVE: return "NEGATIVE";
    case ErrCode::UNSTABLE: return "UNSTABLE";
  }
  return "UNKNOWN";
}

static const char* stateToStrLocal(State s) {
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
