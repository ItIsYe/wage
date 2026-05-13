#pragma once

#include <Arduino.h>
#include "types.h"

void webConfigLoadDefaults(RuntimeConfig& cfg);
void webConfigLoadFromPrefs(RuntimeConfig& cfg, float& oledScale);
void webConfigSaveToPrefs(const RuntimeConfig& cfg);
void webConfigSetup();
void webService(uint32_t now);

bool webHasPendingConfig();
RuntimeConfig webGetPendingConfig();
void webClearPendingConfig();

bool webIsResetRequested();
void webClearResetRequested();

void webSetResetStatusMsg(const String& msg);
String webGetResetStatusMsg();

bool webIsWifiApMode();
String webGetNetworkInfo();
