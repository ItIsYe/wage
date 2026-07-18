#pragma once

#include <Arduino.h>

// Performance-Tracking — nur aktiv wenn PERFORMANCE_DEBUG == true.
// Alle Funktionen sind No-Ops wenn PERFORMANCE_DEBUG == false.

void perfReset();
void perfTrackScale(uint32_t us);
void perfTrackLed(uint32_t us);
void perfTrackState(uint32_t us);
void perfTrackOled(uint32_t us);
void perfTrackConfig(uint32_t us);
void perfTrackReset(uint32_t us);
void perfTrackWeb(uint32_t us);
void perfTrackLoop(uint32_t us);

// Gibt Perf-Log aus wenn Intervall abgelaufen. Gibt aktuelle Loop-Dauer zurück.
void perfLog(uint32_t now);
