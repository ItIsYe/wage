// Native Tests fuer die Lauf-Queue (4xx-Verwerfen, Offline-Erkennung, Flash-Persistenz).
#include <unity.h>
#include "external_interface_module.h"
#include "config.h"
#include "HTTPClient.h"
#include "Preferences.h"

static RuntimeConfig cfg() {
  RuntimeConfig c{};
  c.externalEnabled = true;
  strcpy(c.externalHost, "192.168.50.1"); c.externalPort = 8000; strcpy(c.externalApiPath, "/api/v1/runs");
  strcpy(c.deviceId, "waage-01");
  return c;
}
static RunDataSnapshot run(uint32_t n) {
  RunDataSnapshot r{}; r.runNumber = n; r.bootId = 7; r.durationMs = 1000; r.ageKnown = true; r.finishedAtMs = 0;
  snprintf(r.eventId, sizeof r.eventId, "waage-01-7-%u", (unsigned)n); strcpy(r.deviceId, "waage-01"); return r;
}
static void tick(int times) { for (int i = 0; i < times; ++i) { stubMillis() += EXTERNAL_RETRY_INTERVAL_MS + 1; externalInterfaceService(stubMillis(), true); } }
static void fresh() { nvsStore().clear(); httpStub() = HttpStubState(); stubMillis() = 100000; externalInterfaceInit(cfg(), "test", 7); }

void setUp() {}
void tearDown() {}

void test_success_empties_queue() {
  fresh(); externalInterfaceEnqueueRun(run(1)); externalInterfaceEnqueueRun(run(2));
  tick(2);
  TEST_ASSERT_EQUAL_UINT32(0, externalInterfaceQueueDepth());
}

// Regression: ein 422 hat frueher die Queue dauerhaft blockiert.
void test_422_is_dropped_and_does_not_block() {
  fresh(); httpStub().codes = {201 /*Heartbeat*/, 422, 201};
  externalInterfaceEnqueueRun(run(1)); externalInterfaceEnqueueRun(run(2));
  tick(2);
  TEST_ASSERT_EQUAL_UINT32(0, externalInterfaceQueueDepth());
}

void test_500_is_retried() {
  fresh(); httpStub().codes = {201 /*Heartbeat*/, 500, 201};
  externalInterfaceEnqueueRun(run(1));
  tick(1); TEST_ASSERT_EQUAL_UINT32(1, externalInterfaceQueueDepth());
  tick(1); TEST_ASSERT_EQUAL_UINT32(0, externalInterfaceQueueDepth());
}

void test_connection_failures_switch_to_offline_probe() {
  fresh(); httpStub().codes = {-1, -1, -1, -1, -1, -1, -1, -1};
  externalInterfaceEnqueueRun(run(1));
  tick(3);                               // Verbindungsfehler -> offline
  int postsBefore = httpStub().posts;
  tick(2);                               // offline: die Queue wird nicht bei jedem Tick versucht
  TEST_ASSERT_TRUE(httpStub().posts - postsBefore < 2);
  TEST_ASSERT_EQUAL_UINT32(1, externalInterfaceQueueDepth());
}

void test_queue_survives_reboot() {
  fresh(); externalInterfaceEnqueueRun(run(1)); externalInterfaceEnqueueRun(run(2));
  externalInterfaceInit(cfg(), "test", 8);   // "Neustart": RAM weg, NVS bleibt
  TEST_ASSERT_EQUAL_UINT32(2, externalInterfaceQueueDepth());
  httpStub().codes = {201, 201, 201}; tick(2);
  TEST_ASSERT_EQUAL_UINT32(0, externalInterfaceQueueDepth());
  externalInterfaceInit(cfg(), "test", 9);
  TEST_ASSERT_EQUAL_UINT32(0, externalInterfaceQueueDepth());  // gesendet -> nicht erneut wiederhergestellt
}

void test_payload_has_age_and_boot() {
  fresh(); stubMillis() = 5000; externalInterfaceEnqueueRun(run(1)); httpStub().codes = {201, 201}; tick(1);
  TEST_ASSERT_TRUE(httpStub().lastBody.find("\"age_ms\":") != std::string::npos);
  TEST_ASSERT_TRUE(httpStub().lastBody.find("\"boot_id\":7") != std::string::npos);
}

void test_restored_runs_have_no_age() {
  fresh(); externalInterfaceEnqueueRun(run(1));
  externalInterfaceInit(cfg(), "test", 8);
  httpStub().codes = {201, 201}; tick(1);
  TEST_ASSERT_TRUE(httpStub().lastBody.find("\"age_ms\"") == std::string::npos);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_success_empties_queue);
  RUN_TEST(test_422_is_dropped_and_does_not_block);
  RUN_TEST(test_500_is_retried);
  RUN_TEST(test_connection_failures_switch_to_offline_probe);
  RUN_TEST(test_queue_survives_reboot);
  RUN_TEST(test_payload_has_age_and_boot);
  RUN_TEST(test_restored_runs_have_no_age);
  return UNITY_END();
}
