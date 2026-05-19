#pragma once

#include <Arduino.h>
#include <FastLED.h>

#include "types.h"

void ring2Init(CRGB* leds);
bool ring2Service(uint32_t now);
void ring2MarkDirty();
void ring2Clear();
bool ring2ForceTestService(uint32_t now);
void ring2LogWrite(uint32_t now);
void ring2SetState(State s);
