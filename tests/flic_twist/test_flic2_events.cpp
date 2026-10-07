// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov

// Host test: flic2_events.h against the Flic 2 spec's event table for all 16 codes.
#include "flic2_events.h"
#include <cstdio>
#include <cstring>
using namespace esphome::flic_twist;

int main() {
  // Expected per the spec's "single click / double click / hold" use case and ACK rule.
  const char *want[16] = {nullptr, nullptr, "click", "hold", nullptr, nullptr, "click", nullptr,
                          nullptr, nullptr, "click", "double_click", nullptr, nullptr, nullptr, "double_click"};
  const bool ack[16] = {false, false, true, false, false, false, true, false,
                        false, false, true, true, false, false, true, true};
  int fails = 0;
  for (int e = 0; e < 16; e++) {
    const char *got = flic2_event_type((uint8_t) e);
    const bool same = (got == nullptr && want[e] == nullptr) || (got && want[e] && strcmp(got, want[e]) == 0);
    if (!same || flic2_event_needs_ack((uint8_t) e) != ack[e]) {
      printf("FAIL code %d: got %s/%d want %s/%d\n", e, got ? got : "-", flic2_event_needs_ack((uint8_t) e),
             want[e] ? want[e] : "-", ack[e]);
      fails++;
    }
  }
  printf("16 Flic 2 event codes, %d failures\n", fails);
  return fails ? 1 : 0;
}
