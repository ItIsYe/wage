#pragma once

#include <Arduino.h>

#include "types.h"

void externalInterfaceInit(const RuntimeConfig& cfg, const char* firmwareVersion);
void externalInterfaceUpdateConfig(const RuntimeConfig& cfg);
bool externalInterfaceEnqueueRun(const RunDataSnapshot& snapshot);
void externalInterfaceService(uint32_t now, bool safeToRetry);
size_t externalInterfaceQueueDepth();
const char* externalInterfaceLastStatus();
bool externalInterfaceHasSendError();
uint32_t externalInterfaceErrorEventCounter();
