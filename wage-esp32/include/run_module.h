#pragma once

#include <Arduino.h>
#include "types.h"

// Initialisiert das Run-Modul mit Boot-ID und Device-ID.
void runModuleInit(uint32_t bootId, const char* deviceId);

// Neuer Geraete-Name (Web-Konfiguration). Gilt ab dem naechsten Lauf.
void runModuleSetDeviceId(const char* deviceId);

// Setzt die Referenzgewicht-Werte für den nächsten Lauf.
void runModuleSetReference(float referenceWeightG, const RuntimeConfig& cfg);

// Startet eine Messung: setzt nur das Minimum auf das aktuelle Gewicht.
// Referenzgewicht und Schwellen bleiben erhalten (werden fuer Stop-Erkennung
// und den gesendeten Lauf gebraucht).
void runModuleStartTiming(float weightG);

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

// Setzt den gesamten internen Zustand zurück (Referenz, Schwellen, Minimum) –
// nach Tare bzw. nach abgeschlossenem Lauf. Nicht am Messstart verwenden,
// dafür gibt es runModuleStartTiming().
void runModuleReset();
