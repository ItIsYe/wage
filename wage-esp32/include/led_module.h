#pragma once

#include <Arduino.h>

#include "types.h"

void ledsInit();
void ledsSetMode(LedMode m);
// Zentrale API fuer den Waagen-State (u.a. Ring 2).
void ledSetState(State state);
void ledService(uint32_t now);
void ledApplyBrightnessForCurrentMode();
void ledClear();
void ledShow();
void ledMarkAllDirty();
