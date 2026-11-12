#pragma once

#include <Arduino.h>

#include "types.h"

void externalInterfaceInit(const RuntimeConfig& cfg, const char* firmwareVersion, uint32_t bootId);
void externalInterfaceUpdateConfig(const RuntimeConfig& cfg);
bool externalInterfaceEnqueueRun(const RunDataSnapshot& snapshot);
// safeToRetry: Heartbeat und Senden der Queue sind erlaubt (Waage im Leerlauf).
// quietHeartbeat: Waage wartet mit bereitstehendem Glas und es deutet nichts auf einen
// Messstart hin. Dann darf (nur) der Heartbeat laufen, damit der Pi die Waage nicht
// nach 90 s als offline anzeigt. Gesendet wird dabei nur, wenn der Pi als erreichbar
// gilt, und mit kurzem Verbindungs-Timeout, um die Messschleife nicht zu blockieren.
void externalInterfaceService(uint32_t now, bool safeToRetry, bool quietHeartbeat = false);
size_t externalInterfaceQueueDepth();
const char* externalInterfaceLastStatus();
bool externalInterfaceHasSendError();
uint32_t externalInterfaceErrorEventCounter();
