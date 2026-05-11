#pragma once

#include <Arduino.h>

#include "types.h"

extern float w_raw1;
extern float w_raw2;
extern float w_avg;
extern float w_filt;
extern bool isStable;
extern bool haveRead;
extern bool haveStableRead;
extern float absFilt;
extern bool objectMissingStable;
extern long raw1;
extern long raw2;

void scaleInit();
bool readScalesOnce(long& outRaw1, long& outRaw2);
void tareBoth();
bool isObjectPresentStable(float weight, bool stable);
void scaleService(uint32_t now);
