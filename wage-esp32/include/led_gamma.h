#pragma once
#include <stdint.h>
#include <math.h>

// Gemeinsame Gamma-2.8 Lookup-Table für alle LED-Module.
// Einmalig beim ersten Aufruf von ledGammaLutBuild() befüllt,
// danach O(1) statt powf() pro Pixel.

namespace LedGamma {

inline uint8_t lut[256] = {};
inline bool ready = false;

inline void build() {
  if (ready) return;
  for (uint16_t i = 0; i < 256; ++i) {
    const float normalized = i / 255.0f;
    const float corrected = powf(normalized, 2.8f);
    lut[i] = (uint8_t)(corrected * 255.0f + 0.5f);
  }
  ready = true;
}

inline uint8_t apply(uint8_t value) {
  return lut[value];
}

}  // namespace LedGamma
