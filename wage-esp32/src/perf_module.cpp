#include "perf_module.h"
#include "config.h"

namespace {
  uint32_t maxScale  = 0;
  uint32_t maxLed    = 0;
  uint32_t maxState  = 0;
  uint32_t maxOled   = 0;
  uint32_t maxConfig = 0;
  uint32_t maxReset  = 0;
  uint32_t maxWeb    = 0;
  uint32_t maxLoop   = 0;

  uint32_t curScale  = 0;
  uint32_t curLed    = 0;
  uint32_t curState  = 0;
  uint32_t curOled   = 0;
  uint32_t curConfig = 0;
  uint32_t curReset  = 0;
  uint32_t curWeb    = 0;
  uint32_t curLoop   = 0;

  uint32_t lastLogMs = 0;
}

void perfReset() {
  maxScale = maxLed = maxState = maxOled = maxConfig = maxReset = maxWeb = maxLoop = 0;
}

void perfTrackScale(uint32_t us)  { if (!PERFORMANCE_DEBUG) return; curScale  = us; if (us > maxScale)  maxScale  = us; }
void perfTrackLed(uint32_t us)    { if (!PERFORMANCE_DEBUG) return; curLed    = us; if (us > maxLed)    maxLed    = us; }
void perfTrackState(uint32_t us)  { if (!PERFORMANCE_DEBUG) return; curState  = us; if (us > maxState)  maxState  = us; }
void perfTrackOled(uint32_t us)   { if (!PERFORMANCE_DEBUG) return; curOled   = us; if (us > maxOled)   maxOled   = us; }
void perfTrackConfig(uint32_t us) { if (!PERFORMANCE_DEBUG) return; curConfig = us; if (us > maxConfig) maxConfig = us; }
void perfTrackReset(uint32_t us)  { if (!PERFORMANCE_DEBUG) return; curReset  = us; if (us > maxReset)  maxReset  = us; }
void perfTrackWeb(uint32_t us)    { if (!PERFORMANCE_DEBUG) return; curWeb    = us; if (us > maxWeb)    maxWeb    = us; }
void perfTrackLoop(uint32_t us)   { if (!PERFORMANCE_DEBUG) return; curLoop   = us; if (us > maxLoop)   maxLoop   = us; }

void perfLog(uint32_t now) {
  if (!PERFORMANCE_DEBUG) return;
  if (now - lastLogMs < 1000) return;
  lastLogMs = now;

  Serial.print("[PERF] scale="); Serial.print(curScale);
  Serial.print("us led=");       Serial.print(curLed);
  Serial.print("us state=");     Serial.print(curState);
  Serial.print("us oled=");      Serial.print(curOled);
  Serial.print("us cfg=");       Serial.print(curConfig);
  Serial.print("us reset=");     Serial.print(curReset);
  Serial.print("us web=");       Serial.print(curWeb);
  Serial.print("us loop=");      Serial.print(curLoop);
  Serial.print("us max(scale/led/state/oled/cfg/reset/web/loop)=");
  Serial.print(maxScale);  Serial.print('/');
  Serial.print(maxLed);    Serial.print('/');
  Serial.print(maxState);  Serial.print('/');
  Serial.print(maxOled);   Serial.print('/');
  Serial.print(maxConfig); Serial.print('/');
  Serial.print(maxReset);  Serial.print('/');
  Serial.print(maxWeb);    Serial.print('/');
  Serial.println(maxLoop);

  perfReset();
}
