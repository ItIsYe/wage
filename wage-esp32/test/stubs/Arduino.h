#pragma once
// Minimaler Host-Stub fuer den Arduino-Core (nur fuer die nativen Unit-Tests: pio test -e native).
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <stdarg.h>

#define F(x) x
inline uint32_t& stubMillis() { static uint32_t m = 0; return m; }
inline uint32_t millis() { return stubMillis(); }

struct String {
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& c) : s(c) {}
  String(int v) : s(std::to_string(v)) {}
  String(unsigned v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  String(float v, int dec = 2) { char b[32]; snprintf(b, sizeof b, "%.*f", dec, v); s = b; }
  void reserve(size_t) {}
  size_t length() const { return s.size(); }
  char charAt(size_t i) const { return s[i]; }
  int indexOf(const char* p) const { auto i = s.find(p); return i == std::string::npos ? -1 : (int)i; }
  const char* c_str() const { return s.c_str(); }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o) { s += o; return *this; }
  String& operator+=(char o) { s += o; return *this; }
};
inline String operator+(const String& a, const String& b) { return String(a.s + b.s); }
inline String operator+(const String& a, const char* b) { return String(a.s + b); }
inline String operator+(const char* a, const String& b) { return String(std::string(a) + b.s); }

struct SerialStub {
  void print(const char*) {} void println(const char*) {} void println() {}
  void print(float, int = 2) {} void println(float, int = 2) {}
  void printf(const char*, ...) {}
};
static SerialStub Serial;
