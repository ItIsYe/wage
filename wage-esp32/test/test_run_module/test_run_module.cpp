// Native Unit-Tests fuer run_module (Referenz, Schwellen, Snapshot). Ausfuehren: pio test -e native
#include <unity.h>
#include "run_module.h"
#include "config.h"

static RuntimeConfig cfg() {
  RuntimeConfig c{};
  c.objectPresentG = 100.0f;
  c.startDropPercent = 5.0f;
  c.stopRisePercent = 5.0f;
  return c;
}

void setUp() { runModuleInit(42, "waage-test"); runModuleReset(); }
void tearDown() {}

void test_thresholds_from_reference() {
  runModuleSetReference(400.0f, cfg());
  TEST_ASSERT_EQUAL_FLOAT(400.0f, runModuleGetReference());
  TEST_ASSERT_EQUAL_FLOAT(20.0f, runModuleGetStartDropThreshold());
  TEST_ASSERT_EQUAL_FLOAT(20.0f, runModuleGetStopRiseThreshold());
}

// Regression (Refactoring 3d53877): der Messstart darf Referenz und Schwellen nicht loeschen.
void test_start_timing_keeps_reference_and_thresholds() {
  runModuleSetReference(400.0f, cfg());
  runModuleUpdateMin(150.0f);
  runModuleStartTiming(380.0f);
  TEST_ASSERT_EQUAL_FLOAT(380.0f, runModuleGetMin());
  TEST_ASSERT_EQUAL_FLOAT(400.0f, runModuleGetReference());
  TEST_ASSERT_EQUAL_FLOAT(20.0f, runModuleGetStopRiseThreshold());
}

void test_reset_clears_everything() {
  runModuleSetReference(400.0f, cfg());
  runModuleStartTiming(380.0f);
  runModuleReset();
  TEST_ASSERT_EQUAL_FLOAT(0.0f, runModuleGetReference());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, runModuleGetStopRiseThreshold());
}

void test_snapshot_contains_measurement_data() {
  runModuleSetReference(400.0f, cfg());
  runModuleStartTiming(380.0f);
  runModuleUpdateMin(120.0f);
  RunDataSnapshot s = runModuleBuildSnapshot(5000, 3200);
  TEST_ASSERT_EQUAL_FLOAT(400.0f, s.referenceWeightG);
  TEST_ASSERT_EQUAL_FLOAT(120.0f, s.minWeightG);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, s.stopRiseThresholdG);
  TEST_ASSERT_EQUAL_UINT32(3200, s.durationMs);
  TEST_ASSERT_EQUAL_UINT32(1, s.runNumber);
  TEST_ASSERT_EQUAL_STRING("waage-test-42-1", s.eventId);
}

void test_run_number_increments() {
  runModuleBuildSnapshot(1, 1);
  RunDataSnapshot s = runModuleBuildSnapshot(2, 1);
  TEST_ASSERT_EQUAL_UINT32(2, s.runNumber);
}

// Die laengstmoegliche Event-ID (31 Zeichen Geraetename, 10-stellige Boot-ID und Laufnummer) muss
// in den Puffer passen, sonst wird sie abgeschnitten und verschiedene Laeufe haetten dieselbe ID.
void test_longest_event_id_is_not_truncated() {
  char name[32]; memset(name, 'a', 31); name[31] = 0;
  runModuleInit(4294967295u, name);
  RunDataSnapshot s = runModuleBuildSnapshot(1, 1);
  char expected[80]; snprintf(expected, sizeof expected, "%s-4294967295-1", name);
  TEST_ASSERT_EQUAL_STRING(expected, s.eventId);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_thresholds_from_reference);
  RUN_TEST(test_start_timing_keeps_reference_and_thresholds);
  RUN_TEST(test_reset_clears_everything);
  RUN_TEST(test_snapshot_contains_measurement_data);
  RUN_TEST(test_run_number_increments);
  RUN_TEST(test_longest_event_id_is_not_truncated);
  return UNITY_END();
}
