#pragma once

#include <Arduino.h>
#include <FastLED.h>

#include "types.h"

void ring1Init(CRGB* leds);
void ring1SetMode(LedMode mode, uint32_t now);
bool ring1Service(uint32_t now);
void ring1ApplyBrightnessForCurrentMode();
void ring1MarkDirty();
void ring1Clear();
void ring1FillDebugAllOn();
