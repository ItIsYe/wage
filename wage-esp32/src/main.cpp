#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ArduinoOTA.h>
#include <string.h>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

#include "config.h"
#include "types.h"
#include "display_module.h"
#include "led_module.h"
#include "scale_module.h"
#include "web_config_module.h"
#include "external_interface_module.h"
#include "run_module.h"
#include "perf_module.h"
#include "serial_debug_module.h"

/* =========================================================
   DISPLAY
   ========================================================= */

Adafruit_SH1106G display(SCREEN_W, SCREEN_H, &Wire, -1);

/* =========================================================
   EXTERNAL INTERFACE
   ========================================================= */

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

RuntimeConfig activeConfig;

/* =========================================================
   RUNTIME VARIABLES
   ========================================================= */

// moving average buffer
float oledScale = 1.5f;

// object tracking
bool objectPresent = false;

// timing
static uint32_t detectAtMs = 0;
static uint32_t tStartMs   = 0;
static uint32_t showUntilMs= 0;

static uint32_t dropSinceMs = 0;
static uint32_t stopCandidateSinceMs = 0;

// standby
static uint32_t lastActionMs = 0;

// recovery
static uint32_t recoverUntilMs = 0;

static uint32_t bootId = 0;
static uint32_t lastShownExternalErrorEvent = 0;

static bool oledDebugLedsCleared = false;


/* =========================================================
   FORWARD DECLARATIONS
   ========================================================= */
static void setState(State s);
static const char* stateToStr(State s);
static bool shouldCheckNegativeError(State s);
static void oledService(uint32_t now);
static void stateMachineService(uint32_t now);
static void applyPendingConfigIfAllowed();
static void handleResetRequestIfAllowed(uint32_t now);
static void showStatus(const char* line1, const char* line2);
static void resetNegativeDetection();
static void resetRuntimeAfterTare();

/* =========================================================
   HELPERS
   ========================================================= */

// serialPrintAll -> serial_debug_module.cpp

static void finalizeTiming(uint32_t now) {
  const uint32_t dt = now - tStartMs;
  const RunDataSnapshot runData = runModuleBuildSnapshot(now, dt);
  (void)externalInterfaceEnqueueRun(runData);
  char buf[24];
  snprintf(buf, sizeof(buf), "Zeit: %.1fs", dt / 1000.0f);
  showStatus("Fertig", buf);
  ledsSetMode(LedMode::RESULT_FLASH_GB_ONCE);
  showUntilMs = now + SHOW_RESULT_MS;
  setState(State::SHOW_RESULT);
}

static void showStatusstatic void showStatus(const char* line1, const char* line2) {
  if (activeConfig.debugMode) return;
  oledMsg2(line1, line2);
}

static uint32_t negativeSinceMs = 0;

static void resetNegativeDetection() {
  negativeSinceMs = 0;
}

static void resetRuntimeAfterTare() {
  err = ErrCode::OK;
  objectPresent = false;
  dropSinceMs = 0;
  stopCandidateSinceMs = 0;
  runModuleReset();
  resetNegativeDetection();
}

/* =========================================================
   STATE MACHINE
   ========================================================= */

static void setError(ErrCode e) {
  err = e;
  ledsSetMode(LedMode::ERROR_BLINK_RED);
  char line2[32];
  snprintf(line2, sizeof(line2), "ERR: %s", errToStr(e));
  showStatus("Fehler!", line2);
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
  ledSetState(s);
  lastActionMs = millis();
}

static void oledService(uint32_t now) {
  if (state == State::TIMING) {
    if (activeConfig.debugMode) return;
    oledTimingLive(now - tStartMs);
  }
}

/* =========================================================
   SETUP / LOOP
   ========================================================= */

void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println("[BOOT] WAGE ESP32 beta led-module-state build");
  Serial.printf("[BOOT] ring1Pin=%u ring2Pin=%u ring2Enabled=%u ring2BootTest=%u ring2ForceTest=%u masterDebug=%u\n",
                (unsigned)LED_STRIP_PIN,
                (unsigned)RING2_PIN,
                (unsigned)RING2_ENABLED,
                (unsigned)RING2_BOOT_TEST,
                (unsigned)RING2_FORCE_INDEPENDENT_TEST,
                (unsigned)MASTER_DEBUG_LOG);
  webConfigLoadDefaults(activeConfig);
  initOledScale();
  webConfigLoadFromPrefs(activeConfig, oledScale);
  Serial.printf("[CFG] Standby after=%lu ms (%lus)\n",
                (unsigned long)activeConfig.standbyAfterMs,
                (unsigned long)(activeConfig.standbyAfterMs / 1000U));
  randomSeed(esp_random());
  bootId = esp_random();
  serialDebugInit();
  runModuleInit(bootId, activeConfig.deviceId);

  ledsInit();
  oledInit();
  scaleInit();

  // Sofort anzeigen bevor WLAN-Connect (kann bis zu 30s dauern)
  ledsSetMode(LedMode::RED_SOLID);
  oledMsg2("Start...", "Verbinde WLAN...");
  setState(State::BOOT_MSG);

  webConfigSetup();
  externalInterfaceInit(activeConfig, FIRMWARE_VERSION);

  oledMsg2("Start...", "Initialisierung");

  // ArduinoOTA: nur starten wenn STA-WLAN verbunden (nicht im Fallback-AP)
  if (!webIsWifiApMode() && WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.setHostname("wage-esp32");
    ArduinoOTA.setPassword("wagefirmware");  // Passwort für OTA-Upload, in platformio.ini hinterlegen
    ArduinoOTA.onStart([]() {
      Serial.println("[OTA] Start");
      ledClear(); ledShow();
      oledMsg2("Firmware-Update", "Bitte warten...");
    });
    ArduinoOTA.onEnd([]() {
      Serial.println("[OTA] Fertig, starte neu...");
      oledMsg2("Update fertig", "Neustart...");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      const uint8_t percent = (uint8_t)(progress * 100 / total);
      char buf[20];
      snprintf(buf, sizeof(buf), "Fortschritt: %u%%", (unsigned)percent);
      oledMsg2("Firmware-Update", buf);
      if (MASTER_DEBUG_LOG) {
        Serial.printf("[OTA] %u%%\n", progress * 100 / total);
      }
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("[OTA] Fehler[%u]\n", error);
      oledMsg2("Update Fehler!", "Neustart...");
    });
    ArduinoOTA.begin();
    Serial.print("[OTA] ArduinoOTA bereit, IP=");
    Serial.println(WiFi.localIP().toString());
  } else {
    Serial.println("[OTA] ArduinoOTA nicht gestartet (kein STA-WLAN)");
  }
}

static void stateMachineService(uint32_t now) {
  if (haveStableRead && shouldCheckNegativeError(state) && !isNegativeCheckSuppressed(now)) {
    if (w_filt < NEGATIVE_ERROR_G) {
      if (negativeSinceMs == 0) negativeSinceMs = now;
      if ((now - negativeSinceMs) >= 400) {
        if (MASTER_DEBUG_LOG) {
          Serial.print("[NEGATIVE] state=");
          Serial.print(stateToStr(state));
          Serial.print(" w_filt=");
          Serial.println(w_filt, 2);
        }
        setError(ErrCode::NEGATIVE);
      }
    } else if (w_filt > (NEGATIVE_ERROR_G + 0.5f)) {
      resetNegativeDetection();
    }
  } else {
    resetNegativeDetection();
  }

  const bool standbyDue = (now - lastActionMs) > activeConfig.standbyAfterMs;

  switch (state) {
    case State::BOOT_MSG: {
      if (now - lastActionMs >= BOOT_MSG_MS) {
        showStatus("Nullung...", "Bitte nichts auflegen");
        // Nullung = alle rot dauerhaft.
        ledsSetMode(LedMode::RED_SOLID);
        setState(State::BOOT_TARE);
      }
      break;
    }

    case State::BOOT_TARE: {
      tareBoth();
      resetRuntimeAfterTare();

      showStatus("Nullung OK", "Warte auf Glas");
      ledsSetMode(LedMode::OK_ALT_GB);
      setState(State::IDLE_WAIT_GLASS);
      break;
    }

    case State::IDLE_WAIT_GLASS: {
      if (standbyDue) {
        showStatus("Standby", "Bewegung = Aktiv");
        ledsSetMode(LedMode::STANDBY_TWINKLE);
        setState(State::STANDBY);
        break;
      }

      ledsSetMode(LedMode::OK_ALT_GB);
      showStatus("Warte auf Glas", "...");

      if (haveRead && isObjectPresentStable(w_filt, isStable)) {
        objectPresent = true;
        runModuleSetReference(w_filt, activeConfig);
        detectAtMs = now;
        showStatus("Glas erkannt", "");
        ledsSetMode(LedMode::GLASS_GREEN_SOLID);
        setState(State::GLASS_DETECTED);
      }
      break;
    }

    case State::GLASS_DETECTED: {
      if (objectMissingStable) {
        objectPresent = false;
        showStatus("Objekt weg", "Warte auf Glas");
        ledsSetMode(LedMode::OK_ALT_GB);
        setState(State::IDLE_WAIT_GLASS);
        break;
      }

      if (haveRead) {
        runModuleSetReference(max(runModuleGetReference(), w_filt), activeConfig);
      }

      if (haveRead && (now - detectAtMs >= READY_AFTER_DETECT_MS)) {
        runModuleSetReference(max(runModuleGetReference(), w_filt), activeConfig);
        showStatus("Bereit fuer", "Zeitmessung");
        dropSinceMs = 0;
        stopCandidateSinceMs = 0;
        ledsSetMode(LedMode::READY_GREEN_BLINK);
        setState(State::READY_FOR_TIMING);
      }
      break;
    }

    case State::READY_FOR_TIMING: {
      const float w = w_filt;
      const float drop = runModuleGetReference() - w;

      if (objectMissingStable) {
        objectPresent = false;
        showStatus("Objekt weg", "Warte auf Glas");
        ledsSetMode(LedMode::OK_ALT_GB);
        setState(State::IDLE_WAIT_GLASS);
        break;
      }

      if (haveRead && drop >= runModuleGetStartDropThreshold()) {
        if (dropSinceMs == 0) dropSinceMs = now;
        if (now - dropSinceMs >= DROP_HOLD_MS) {
          tStartMs = now;
          runModuleReset();
          runModuleUpdateMin(w);
          dropSinceMs = 0;
          stopCandidateSinceMs = 0;
          ledsSetMode(LedMode::TIMING_BLUE_SPINNER);
          setState(State::TIMING);
        }
      } else if (drop <= (runModuleGetStartDropThreshold() - START_RESET_HYST_G)) {
        dropSinceMs = 0;
      }

      serialDebugPrintReady(now, w, runModuleGetReference(), runModuleGetStartDropThreshold(), runModuleGetStopRiseThreshold(), drop);
      break;
    }

    case State::TIMING: {
      if (haveRead) {
        const float w = w_filt;

        // Minimum nur während "freiem Lauf" weiter nachführen.
        // Sobald ein Stop-Kandidat aktiv ist, wird das Minimum eingefroren,
        // damit die Schwelle nicht von Messrauschen "wegläuft".
        if (stopCandidateSinceMs == 0) runModuleUpdateMin(w);

        const float stopThreshold = runModuleGetMin() + runModuleGetStopRiseThreshold();
        const float rebound = w - runModuleGetMin();
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

        serialDebugPrintTiming(now, w, runModuleGetMin(), stopThreshold, rebound, stopHoldMs, stopCandidateSinceMs != 0);
      }
      break;
    }

    case State::SHOW_RESULT: {
      if (now >= showUntilMs) {
        showStatus("Bitte leeren", "Glas entfernen");
        setState(State::WAIT_EMPTY_AFTER_RESULT);
      }
      break;
    }

    case State::WAIT_EMPTY_AFTER_RESULT: {
      // Bitte freihalten / kein Glas aufstellen = alle rot dauerhaft.
      ledsSetMode(LedMode::RED_SOLID);
      const bool emptyAndStable = haveStableRead && absFilt < activeConfig.emptyThresholdG;
      if (!emptyAndStable) {
        showStatus("Bitte leeren", "Glas entfernen");
      } else {
        if (MASTER_DEBUG_LOG) {
          Serial.print("[RETARE] WAIT_EMPTY -> CHECK_RETARE absFilt=");
          Serial.println(absFilt, 2);
        }
        showStatus("Pruefe Nullpunkt", "...");
        setState(State::CHECK_RETARE);
      }
      break;
    }

    case State::CHECK_RETARE: {
      // Nullung = alle rot dauerhaft.
      ledsSetMode(LedMode::RED_SOLID);
      if (!haveStableRead) break;

      if (absFilt > activeConfig.retareTolG) {
        if (MASTER_DEBUG_LOG) {
          Serial.print("[RETARE] before tare w_filt=");
          Serial.print(w_filt, 2);
          Serial.print(" absFilt=");
          Serial.println(absFilt, 2);
        }
        showStatus("Nullung...", "Offset korr.");
        tareBoth();
        if (MASTER_DEBUG_LOG) Serial.println("[RETARE] after tare reset scale state");
      }

      showStatus("Warte auf Glas", "...");
      ledsSetMode(LedMode::OK_ALT_GB);
      resetRuntimeAfterTare();
      setState(State::IDLE_WAIT_GLASS);
      break;
    }

    case State::STANDBY: {
      if (haveRead && absFilt > activeConfig.standbyWakeThresholdG) {
        showStatus("Aktiv", "Warte auf Glas");
        ledsSetMode(LedMode::OK_ALT_GB);
        setState(State::IDLE_WAIT_GLASS);
      }
      break;
    }

    case State::ERROR_RECOVER: {
      if (now >= recoverUntilMs) {
        if (haveStableRead && absFilt < 2.0f) {
          serialDebugPrintRecover(now, "[RECOVER] Tare (empty & stable).");
          showStatus("Recovery", "Nullung...");
          tareBoth();
          resetRuntimeAfterTare();
          showStatus("OK", "Warte auf Glas");
          ledsSetMode(LedMode::OK_ALT_GB);
          setState(State::IDLE_WAIT_GLASS);
        } else {
          serialDebugPrintRecover(now, "[RECOVER] Waiting for empty/stable before tare...");
          showStatus("Fehler", "Bitte leeren!");
          recoverUntilMs = now + 700;
        }
      }
      break;
    }
  }

}


static void applyPendingConfigIfAllowed(){
  if (!(webHasPendingConfig() && (state == State::IDLE_WAIT_GLASS || state == State::STANDBY))) return;
  const RuntimeConfig pendingConfig = webGetPendingConfig();
  activeConfig = pendingConfig;
  oledScale = activeConfig.oledScaleValue;
  Wire.setClock(activeConfig.oledI2cClockHz);
  display.setRotation(activeConfig.oledRotation);
  ledApplyBrightnessForCurrentMode();
  ledMarkAllDirty();
  webConfigSaveToPrefs(activeConfig);
  if (MASTER_DEBUG_LOG) {
    Serial.printf("[CFG] Ring2 en=%u dbg=%u b=%u sb=%u\n",
                  (unsigned)activeConfig.ring2Enabled,
                  (unsigned)activeConfig.ring2DebugAllOn,
                  (unsigned)activeConfig.ring2BrightnessPercent,
                  (unsigned)activeConfig.ring2StandbyBrightnessPercent);
    Serial.printf("[CFG] Standby after=%lu ms (%lus)\n",
                  (unsigned long)activeConfig.standbyAfterMs,
                  (unsigned long)(activeConfig.standbyAfterMs / 1000U));
  }
  externalInterfaceUpdateConfig(activeConfig);
  webClearPendingConfig();
}

static void handleResetRequestIfAllowed(uint32_t now){
  (void)now;
  if (!webIsResetRequested()) return;
  const bool canResetNow = (
    state == State::IDLE_WAIT_GLASS ||
    state == State::STANDBY ||
    state == State::WAIT_EMPTY_AFTER_RESULT ||
    state == State::ERROR_RECOVER
  );
  if (!canResetNow) {
    webSetResetStatusMsg("Reset wartet auf sicheren Zustand.");
    return;
  }

  err = ErrCode::OK;
  resetRuntimeAfterTare();
  isStable = false;
  webClearResetRequested();
  webSetResetStatusMsg("Reset ausgefuehrt. Nullung laeuft.");

  showStatus("Nullung...", "Bitte nichts auflegen");
  ledsSetMode(LedMode::RED_SOLID);
  setState(State::BOOT_TARE);
}

void loop() {
  if (state != State::TIMING) ArduinoOTA.handle();
  const uint32_t now = millis();
  const uint32_t loopStartUs = PERFORMANCE_DEBUG ? micros() : 0;

  const bool oledDebugActive = activeConfig.oledDebugMode;
  if (oledDebugActive && !oledDebugLedsCleared) {
    ledClear();
    ledShow();
    oledDebugLedsCleared = true;
  } else if (!oledDebugActive) {
    oledDebugLedsCleared = false;
  }

  const uint32_t ledStartUs = PERFORMANCE_DEBUG ? micros() : 0;
  ledService(now);
  const uint32_t ledDurUs = PERFORMANCE_DEBUG ? (micros() - ledStartUs) : 0;

  const uint32_t scaleStartUs = PERFORMANCE_DEBUG ? micros() : 0;
  scaleService(now);
  serialDebugPrintBase(now, raw1, raw2, w_raw1, w_raw2, w_avg, w_filt, isStable, state, objectPresent, errToStr(err));
  const uint32_t scaleDurUs = PERFORMANCE_DEBUG ? (micros() - scaleStartUs) : 0;

  const uint32_t stateStartUs = PERFORMANCE_DEBUG ? micros() : 0;
  stateMachineService(now);
  const uint32_t stateDurUs = PERFORMANCE_DEBUG ? (micros() - stateStartUs) : 0;

  const uint32_t oledStartUs = PERFORMANCE_DEBUG ? micros() : 0;
  if (oledDebugActive) {
    oledDebugPattern(now);
  } else {
    oledService(now);
  }
  const uint32_t oledDurUs = PERFORMANCE_DEBUG ? (micros() - oledStartUs) : 0;

  const uint32_t configStartUs = PERFORMANCE_DEBUG ? micros() : 0;
  applyPendingConfigIfAllowed();
  const uint32_t configDurUs = PERFORMANCE_DEBUG ? (micros() - configStartUs) : 0;

  const uint32_t resetStartUs = PERFORMANCE_DEBUG ? micros() : 0;
  handleResetRequestIfAllowed(now);
  const uint32_t resetDurUs = PERFORMANCE_DEBUG ? (micros() - resetStartUs) : 0;

  const bool safeToRetry = (
    state == State::IDLE_WAIT_GLASS ||
    state == State::WAIT_EMPTY_AFTER_RESULT ||
    state == State::CHECK_RETARE ||
    state == State::STANDBY
  );
  externalInterfaceService(now, safeToRetry);
  const bool externalHintStateOk = (state == State::IDLE_WAIT_GLASS || state == State::STANDBY);
  const uint32_t currentErrorEvent = externalInterfaceErrorEventCounter();
  const bool canShowExternalHint = (
    activeConfig.externalEnabled &&
    !activeConfig.debugMode &&
    !activeConfig.oledDebugMode &&
    !activeConfig.pixelDebugAllOn &&
    externalHintStateOk
  );
  if (canShowExternalHint && externalInterfaceHasSendError() && currentErrorEvent != lastShownExternalErrorEvent) {
    char queueLine[24];
    snprintf(queueLine, sizeof(queueLine), "Queue: %u", (unsigned)externalInterfaceQueueDepth());
    showStatus("Sendefehler", queueLine);
    lastShownExternalErrorEvent = currentErrorEvent;
  }

  const uint32_t webStartUs = PERFORMANCE_DEBUG ? micros() : 0;
  webService(now);
  const uint32_t webDurUs = PERFORMANCE_DEBUG ? (micros() - webStartUs) : 0;

  if (oledDebugActive) delay(20);

  perfTrackScale(scaleDurUs);
  perfTrackLed(ledDurUs);
  perfTrackState(stateDurUs);
  perfTrackOled(oledDurUs);
  perfTrackConfig(configDurUs);
  perfTrackReset(resetDurUs);
  perfTrackWeb(webDurUs);
  if (PERFORMANCE_DEBUG) {
    const uint32_t loopDurUs = micros() - loopStartUs;
    perfTrackLoop(loopDurUs);
  }
  perfLog(now);

  if (activeConfig.debugMode) oledDebugWeights();

  oledFlushIfPending();
}
