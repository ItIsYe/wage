#pragma once
#include <Arduino.h>
#include <map>
#include <vector>
// In-Memory-NVS: uebersteht "Neustarts" (statische Map), nur die vom Lauf-Modul genutzten Funktionen.
inline std::map<std::string, std::vector<uint8_t>>& nvsStore() { static std::map<std::string, std::vector<uint8_t>> m; return m; }
class Preferences {
  std::string ns;
 public:
  bool begin(const char* n, bool = false) { ns = n; return true; }
  void end() {}
  void remove(const char* k) { nvsStore().erase(ns + "/" + k); }
  size_t getBytesLength(const char* k) { auto it = nvsStore().find(ns + "/" + k); return it == nvsStore().end() ? 0 : it->second.size(); }
  size_t putBytes(const char* k, const void* v, size_t l) { nvsStore()[ns + "/" + k].assign((const uint8_t*)v, (const uint8_t*)v + l); return l; }
  size_t getBytes(const char* k, void* b, size_t l) {
    auto it = nvsStore().find(ns + "/" + k); if (it == nvsStore().end()) return 0;
    size_t n = l < it->second.size() ? l : it->second.size(); memcpy(b, it->second.data(), n); return n;
  }
};
