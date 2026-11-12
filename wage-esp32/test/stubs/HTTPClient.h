#pragma once
#include <Arduino.h>
#include <vector>
// Steuerbarer HTTP-Client-Stub: Antwortcodes werden pro POST aus einer Liste geliefert (danach 201).
struct HttpStubState { std::vector<int> codes; size_t next = 0; int posts = 0; std::string lastBody; };
inline HttpStubState& httpStub() { static HttpStubState s; return s; }
class HTTPClient {
 public:
  bool begin(const String&) { return true; }
  void setConnectTimeout(uint16_t) {}
  void setTimeout(uint16_t) {}
  void addHeader(const char*, const char*) {}
  int POST(uint8_t* data, size_t len) {
    auto& st = httpStub(); st.posts++; st.lastBody.assign((const char*)data, len);
    return st.next < st.codes.size() ? st.codes[st.next++] : 201;
  }
  String getString() { return String(""); }
  void end() {}
};
