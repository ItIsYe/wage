#pragma once

#include <Arduino.h>
#include "types.h"

// Initialisiert das Run-Modul mit Boot-ID und Device-ID.
void runModuleInit(uint32_t bootId, const char* deviceId);

// Setzt die Referenzgewicht-Werte für den nächsten Lauf.
void runModuleSetReference(float referenceWeightG, const RuntimeConfig& cfg);

// Aktualisiert das Minimum während des Timings.
void runModuleUpdateMin(float weightG);

// Gibt das aktuelle Minimum zurück.
float runModuleGetMin();

// Gibt die Start-Drop-Schwelle zurück.
float runModuleGetStartDropThreshold();

// Gibt die Stop-Rise-Schwelle zurück.
float runModuleGetStopRiseThreshold();

// Gibt das Referenzgewicht zurück.
float runModuleGetReference();

// Baut einen RunDataSnapshot und gibt ihn zurück.
RunDataSnapshot runModuleBuildSnapshot(uint32_t finishedAtMs, uint32_t durationMs);

// Setzt den internen Zustand zurück (nach Tare oder neuem Lauf).
void runModuleReset();
