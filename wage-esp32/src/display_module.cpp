#include "display_module.h"

#include <Wire.h>
#include <string.h>

#include "config.h"
#include "types.h"

extern RuntimeConfig activeConfig;
extern float oledScale;
extern float w_raw1;
extern float w_raw2;
extern float w_avg;
extern float w_filt;
extern bool isStable;
extern State state;
extern bool objectPresent;
extern ErrCode err;
extern const char* errToStr(ErrCode e);

bool oledReady = false;

static bool oledMsgValid = false;
static char oledLastLine1[32] = {0};
static char oledLastLine2[32] = {0};
static uint32_t lastOledTimingMs = 0;
static uint32_t lastOledDebugMs = 0;
static uint32_t lastOledPatternMs = 0;
static int32_t lastTimingTenths = -1;
static uint32_t lastOledFlushMs = 0;
static bool oledFlushPending = false;
static constexpr uint32_t OLED_MIN_FLUSH_INTERVAL_MS = 50;

// Page-Dirty-Tracking fuer partielles I2C-Update
// SH1106G: 128x64 = 8 Pages a 128 Bytes
static constexpr uint8_t OLED_PAGES = 8;
static constexpr uint8_t OLED_PAGE_WIDTH = 128;
static uint8_t oledPageSnapshot[OLED_PAGES][OLED_PAGE_WIDTH] = {};
static bool oledSnapshotValid = false;

// Sendet eine einzelne Page direkt per I2C (SH1106G, Spalten-Offset 2)
static void sh1106SendPage(uint8_t page, const uint8_t* data) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00);
  Wire.write(0xB0 | (page & 0x07));
  Wire.write(0x02);
  Wire.write(0x10);
  Wire.endTransmission();
  for (uint8_t col = 0; col < OLED_PAGE_WIDTH; col += 16) {
    Wire.beginTransmission(OLED_ADDR);
    Wire.write(0x40);
    const uint8_t chunk = (col + 16 <= OLED_PAGE_WIDTH) ? 16 : (OLED_PAGE_WIDTH - col);
    Wire.write(data + col, chunk);
    Wire.endTransmission();
  }
}

// Partielles Update: nur geaenderte Pages uebertragen
static void oledFlushPartial() {
  uint8_t* buf = display.getBuffer();
  if (!buf) { display.display(); return; }
  for (uint8_t page = 0; page < OLED_PAGES; ++page) {
    const uint8_t* pageData = buf + (page * OLED_PAGE_WIDTH);
    if (oledSnapshotValid && memcmp(pageData, oledPageSnapshot[page], OLED_PAGE_WIDTH) == 0) continue;
    sh1106SendPage(page, pageData);
    memcpy(oledPageSnapshot[page], pageData, OLED_PAGE_WIDTH);
  }
  oledSnapshotValid = true;
}

static inline void oledFlush() {
  const uint32_t now = millis();
  if ((now - lastOledFlushMs) < OLED_MIN_FLUSH_INTERVAL_MS) {
    oledFlushPending = true;
    return;
  }
  lastOledFlushMs = now;
  oledFlushPending = false;
  oledFlushPartial();
}

void oledFlushIfPending() {
  if (!oledFlushPending || !oledReady) return;
  const uint32_t now = millis();
  if ((now - lastOledFlushMs) < OLED_MIN_FLUSH_INTERVAL_MS) return;
  lastOledFlushMs = now;
  oledFlushPending = false;
  oledFlushPartial();
}

void oledInit() {
  oledReady = false;
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(activeConfig.oledI2cClockHz);
  if (!display.begin(OLED_ADDR, true)) return;
  oledReady = true;
  oledSnapshotValid = false;
  display.setRotation(activeConfig.oledRotation);
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.display();
  lastOledFlushMs = millis();
}

void initOledScale() {
  String normalized = OLED_SCALE_CONFIG;
  normalized.replace(',', '.');

  float parsed = normalized.toFloat();
  if (parsed <= 0.0f) parsed = 1.0f;

  oledScale = parsed;
  activeConfig.oledScaleValue = parsed;

  if (MASTER_DEBUG_LOG) {
    Serial.print("[OLED] scale config=");
    Serial.print(OLED_SCALE_CONFIG);
    Serial.print(" normalized=");
    Serial.print(normalized);
    Serial.print(" parsed=");
    Serial.println(oledScale, 2);
  }
}

uint8_t oledTextSizeFromScale() {
  int textSize = (int)lroundf(oledScale);
  if (textSize < 1) textSize = 1;
  if (textSize > 8) textSize = 8;
  return (uint8_t)textSize;
}

void oledMsg2(const char* line1, const char* line2) {
  if (!oledReady) return;

  if (oledMsgValid && strcmp(line1, oledLastLine1) == 0 && strcmp(line2, oledLastLine2) == 0) {
    return;
  }

  strncpy(oledLastLine1, line1, sizeof(oledLastLine1) - 1);
  oledLastLine1[sizeof(oledLastLine1) - 1] = '\0';
  strncpy(oledLastLine2, line2, sizeof(oledLastLine2) - 1);
  oledLastLine2[sizeof(oledLastLine2) - 1] = '\0';
  oledMsgValid = true;

  const uint8_t textScale = oledTextSizeFromScale();
  const int lineHeight = (int)textScale * 8;
  const int gap = 4;
  int line2Y = lineHeight + gap;
  const int maxY = display.height() - lineHeight;
  if (line2Y > maxY) line2Y = maxY;
  display.clearDisplay();
  display.setTextSize(textScale);
  display.setCursor(0, 0);
  display.println(line1);
  display.setCursor(0, line2Y);
  display.println(line2);
  oledFlush();
}

void showNetworkStatus(const char* line1, const String& ip) {
  Serial.print("[NET] ");
  Serial.print(line1);
  Serial.print(" IP: ");
  Serial.println(ip);
  if (!oledReady) return;
  oledMsg2(line1, ip.c_str());
  delay(1200);
}

void oledTimingLive(uint32_t dtMs) {
  if (!oledReady) return;
  const uint32_t now = millis();
  if ((now - lastOledTimingMs) < activeConfig.oledTimingRefreshMs) return;
  lastOledTimingMs = now;
  const int32_t dtTenths = (int32_t)(dtMs / 100);
  if (dtTenths == lastTimingTenths) return;
  lastTimingTenths = dtTenths;
  oledMsgValid = false;

  const uint8_t baseScale = oledTextSizeFromScale();
  const uint8_t titleScale = (baseScale > 1) ? 1 : baseScale;
  uint8_t timeScale = (display.height() >= 64) ? 2 : baseScale;
  if (timeScale < 1) timeScale = 1;
  if (timeScale > 4) timeScale = 4;

  const int titleLineHeight = (int)titleScale * 8;
  const int timeLineHeight = (int)timeScale * 8;
  const int timeY = display.height() - timeLineHeight - 2;
  display.clearDisplay();
  display.setTextSize(titleScale);
  display.setCursor(0, 0);
  display.println("Zeitmessung");
  display.println("laeuft...");
  if (titleLineHeight * 2 < timeY - 2) {
    display.setCursor(0, titleLineHeight * 2 + 2);
    display.println("Live:");
  }
  display.setTextSize(timeScale);
  display.setCursor(0, timeY);
  const uint32_t seconds = dtMs / 1000u;
  const uint32_t tenths = (dtMs % 1000u) / 100u;
  display.print(seconds);
  display.print('.');
  display.print(tenths);
  display.print('s');
  oledFlush();
}

void oledDebugWeights() {
  if (!oledReady) return;
  const uint32_t now = millis();
  if ((now - lastOledDebugMs) < OLED_DEBUG_REFRESH_MS) return;
  lastOledDebugMs = now;
  oledMsgValid = false;

  const uint8_t textScale = 1;
  display.clearDisplay();
  display.setTextSize(textScale);
  display.setCursor(0, 0);

  display.print("DBG R1:");
  display.print(w_raw1, 1);
  display.print(" R2:");
  display.println(w_raw2, 1);

  display.print("AVG:");
  display.print(w_avg, 1);
  display.print(" FILT:");
  display.println(w_filt, 1);

  display.print("STB:");
  display.print(isStable ? "Y" : "N");
  display.print(" S:");
  display.print((int)state);
  display.setCursor(0, 24);
  display.print("OBJ:");
  display.print(objectPresent ? "Y" : "N");
  display.print(" E:");
  display.println(errToStr(err));

  oledFlush();
}

void oledDebugPattern(uint32_t now) {
  if (!oledReady) return;
  if ((now - lastOledPatternMs) < OLED_PATTERN_REFRESH_MS) return;
  lastOledPatternMs = now;
  oledMsgValid = false;
  display.clearDisplay();

  display.drawRect(0, 0, display.width(), display.height(), SH110X_WHITE);
  display.drawLine(0, display.height() / 2, display.width() - 1, display.height() / 2, SH110X_WHITE);
  display.drawLine(display.width() / 2, 0, display.width() / 2, display.height() - 1, SH110X_WHITE);

  const int16_t sweepX = (int16_t)(now / 20u) % display.width();
  display.drawLine(sweepX, 0, sweepX, display.height() - 1, SH110X_WHITE);
  oledFlush();
}
