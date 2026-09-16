#pragma once

#include <Arduino.h>
#include <FastLED.h>

#include "types.h"

// Ring1 = Hauptring (PIXEL_COUNT physische Pixel, PIXEL_GROUPS logische
// Dreiergruppen). Eigenstaendiges Standby-Twinkle, kein Shared-Mechanismus
// mit Ring2 mehr - jeder Ring rendert sein eigenes Muster unabhaengig.

void ring1Init(CRGB* leds);
void ring1SetMode(LedMode mode, uint32_t now);
bool ring1Service(uint32_t now);
void ring1ApplyBrightnessForCurrentMode();
void ring1MarkDirty();
void ring1Clear();
void ring1FillDebugAllOn();

// Diagnose: geht alle physischen Pixel einzeln durch (weiss, ~400ms je
// Pixel) und meldet ueber Serial den aktuellen Index. Damit laesst sich am
// Strip ablesen, ab welchem physischen Pixel das Signal nicht mehr ankommt.
void ring1DiagnosticStart();
bool ring1DiagnosticService(uint32_t now);
bool ring1DiagnosticActive();
