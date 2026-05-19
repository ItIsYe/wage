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

void ring1ApplySharedStandby(const bool* on, const uint16_t* hue, const uint8_t* value, uint16_t count);
