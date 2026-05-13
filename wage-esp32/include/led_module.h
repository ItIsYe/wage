#pragma once

#include <Arduino.h>

#include "types.h"

void ledsInit();
void ledsSetMode(LedMode m);
void ledService(uint32_t now);
void ledApplyBrightnessForCurrentMode();
void ledClear();
void ledShow();
