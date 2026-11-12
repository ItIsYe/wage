#pragma once
#include <stdint.h>
struct IPAddress {
  uint8_t o[4];
  IPAddress(uint8_t a = 0, uint8_t b = 0, uint8_t c = 0, uint8_t d = 0) : o{a, b, c, d} {}
  // config.h legt IPAddress-Konstanten an und liest sie ueber toString() nicht aus; fuer die Tests genuegt der Typ.
};
