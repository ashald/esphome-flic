// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov
#pragma once

#include <cstdint>
#include <cstdlib>

namespace esphome {
namespace flic {

// Spots a Twist stuck streaming rotation reports while nobody turns it. In that state it reports
// tiny back-and-forth movement several times a second (seen: ~8/s, staying within ~30 raw units,
// a few hundredths of a percent) and keeps its LED ring lit until its session is restarted. None of
// it crosses a detent, so nothing reaches Home Assistant.
//
// A stream is a run of rotation reports, each within max_gap_ms of the previous, whose summed
// movement stays within max_excursion raw units of where the run started. A run lasting stream_ms
// with at least min_reports reports and no button event in between is a stuck Twist. Real use ends
// a run: turning it moves it further than max_excursion, a press is a button event, and leaving it
// alone leaves a gap. At most one recovery per cooldown_ms, in case it streams again right away.
class TwistStreamGuard {
 public:
  struct Params {
    uint32_t max_gap_ms;
    uint32_t stream_ms;
    uint32_t min_reports;
    int32_t max_excursion;
    uint32_t cooldown_ms;
  };

  explicit TwistStreamGuard(const Params &p) : p_(p) {}

  // Every rotation report with its raw movement. True when the Twist should be reconnected now.
  bool on_rotation_report(uint32_t now_ms, int32_t total_delta) {
    if (this->count_ == 0 || now_ms - this->last_ms_ > this->p_.max_gap_ms) {
      this->start_run_(now_ms);
    } else {
      this->offset_ += total_delta;
      if (std::abs(this->offset_) > this->p_.max_excursion) {
        this->start_run_(now_ms);  // turned for real: a new run starts here
      } else {
        this->count_++;
      }
    }
    this->last_ms_ = now_ms;
    if (this->count_ < this->p_.min_reports || now_ms - this->start_ms_ < this->p_.stream_ms)
      return false;
    if (this->recovered_ && now_ms - this->last_recovery_ms_ < this->p_.cooldown_ms)
      return false;
    this->recovered_ = true;
    this->last_recovery_ms_ = now_ms;
    this->count_ = 0;
    return true;
  }

  // A press, click or hold: someone is using the Twist, so the run so far was not a stream.
  void on_button_event() { this->count_ = 0; }

  // New session: start over. The cooldown carries on.
  void reset() { this->count_ = 0; }

 protected:
  void start_run_(uint32_t now_ms) {
    this->start_ms_ = now_ms;
    this->offset_ = 0;
    this->count_ = 1;
  }

  Params p_;
  uint32_t start_ms_{0};
  uint32_t last_ms_{0};
  uint32_t count_{0};
  int32_t offset_{0};
  bool recovered_{false};
  uint32_t last_recovery_ms_{0};
};

}  // namespace flic
}  // namespace esphome
