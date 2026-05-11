#pragma once

#include <Arduino.h>
#include <Adafruit_SH110X.h>

extern Adafruit_SH1106G display;
extern bool oledReady;

void oledInit();
void initOledScale();
uint8_t oledTextSizeFromScale();
void oledMsg2(const char* line1, const char* line2);
void showNetworkStatus(const char* line1, const String& ip);
void oledTimingLive(uint32_t dtMs);
void oledDebugWeights();
void oledDebugPattern(uint32_t now);
