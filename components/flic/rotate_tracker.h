// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov
// Portions derived from pyflic-ble (https://github.com/50ButtonsEach/pyflic-ble),
// Copyright Shortcut Labs, Apache-2.0: ported to C++ and modified. See NOTICE.

#pragma once
// Multi-mode rotation tracker for the Flic Twist, ported from pyflic-ble's
// rotate_tracker.py (MultiModeRotateTracker). The Twist has 13 "modes" (12 selector
// slots + a free-rotation mode 12). Each rotation notification carries per-mode deltas;
// this reproduces the SDK's absolute-position / min-boundary bookkeeping so that:
//   * get_mode_percentage(mode) gives the 0..100% knob fill (what a light/volume tracks)
//   * get_absolute_position(mode) + set_mode_min(mode, ...) drive the LED write-back
//     (UpdateTwistPosition sends new_min = absolute_position - desired_units).
// The velocity/rpm/acceleration outputs from the Python tracker are intentionally omitted
// (not needed for the light/media use-cases); only integer-percent detent crossings are
// reproduced, for firing increment/decrement events.

#include <cstdint>

namespace esphome {
namespace flic {

static const int32_t UNITS_PER_SLICE = 4096;
static const int32_t D360 = 12 * UNITS_PER_SLICE;  // 49152 raw units == 100%

struct RotateOutcome {
  float mode_percentage;   // 0..100 (clamped for slot modes; wrapped for continuous)
  int detent_crossings;    // signed integer-percent boundaries crossed this packet (+CW / -CCW)
};

class MultiModeRotateTracker {
 public:
  void configure(bool bound_mode_12, bool wrap_position) {
    this->bound_mode_12_ = bound_mode_12;
    this->wrap_position_ = wrap_position;
  }

  RotateOutcome apply(int mode_index, int32_t total_delta, int32_t min_delta, int32_t max_delta,
                      bool last_min_update_was_top) {
    if (mode_index < 0 || mode_index >= 13)
      mode_index = 0;

    int prev_int_pct = this->int_pct_(mode_index);

    int64_t new_position = this->positions_[mode_index] + (int64_t) total_delta;

    // Slot modes (0-11), and mode 12 when bound (DEFAULT/CONTINUOUS use bound_mode_12),
    // track a min boundary so the bounded position stays within one revolution.
    // Skip entirely when wrapping (continuous free rotation).
    if (!this->wrap_position_ && (mode_index < 12 || (mode_index == 12 && this->bound_mode_12_))) {
      int64_t current_min = this->mins_[mode_index];
      int64_t bottom = new_position - (int64_t) min_delta;
      int64_t top = new_position - (int64_t) max_delta;

      if (last_min_update_was_top) {
        if (bottom < current_min)
          current_min = bottom;
        if (top > current_min + D360)
          current_min = top - D360;
      } else {
        if (top > current_min + D360)
          current_min = top - D360;
        if (bottom < current_min)
          current_min = bottom;
      }

      if (new_position < current_min)
        current_min = new_position;
      else if (new_position > current_min + D360)
        current_min = new_position - D360;

      this->mins_[mode_index] = current_min;
    }

    this->positions_[mode_index] = new_position;

    RotateOutcome out;
    out.mode_percentage = this->get_mode_percentage(mode_index);
    out.detent_crossings = this->int_pct_(mode_index) - prev_int_pct;
    return out;
  }

  float get_mode_percentage(int mode_index) const {
    if (mode_index < 0 || mode_index >= 13)
      return 0.0f;
    if (this->wrap_position_) {
      int64_t wrapped = ((this->positions_[mode_index] % D360) + D360) % D360;
      return (float) wrapped / (float) D360 * 100.0f;
    }
    int64_t bounded = this->positions_[mode_index] - this->mins_[mode_index];
    float pct = (float) bounded / (float) D360 * 100.0f;
    if (pct < 0.0f)
      pct = 0.0f;
    if (pct > 100.0f)
      pct = 100.0f;
    return pct;
  }

  int64_t get_absolute_position(int mode_index) const {
    if (mode_index < 0 || mode_index >= 13)
      return 0;
    return this->positions_[mode_index];
  }

  // Start a new session frame for `mode_index`. A Twist starts every session with each mode at the
  // absolute position the init packet gives it and its min at 0; mirroring that keeps the absolute
  // new_min of UpdateTwistPosition meaning the same thing on both sides.
  void rebase(int mode_index, int64_t position) {
    if (mode_index < 0 || mode_index >= 13)
      return;
    this->positions_[mode_index] = position;
    this->mins_[mode_index] = 0;
  }

  void set_mode_min(int mode_index, int64_t new_min) {
    if (mode_index < 0 || mode_index >= 13)
      return;
    this->mins_[mode_index] = new_min;
  }

 protected:
  // Integer percent used for detent detection. For continuous/wrap mode this MUST use the
  // UNBOUNDED position (monotonic with physical rotation) so the detent SIGN reflects the
  // true direction across the 0/100% wrap — matching pyflic, whose RotateTracker runs on the
  // unbounded accumulator in wrap mode (get_mode_percentage still returns the wrapped value
  // for display). For bounded modes it uses the clamped bounded position, like pyflic.
  int int_pct_(int mode_index) const {
    if (this->wrap_position_) {
      return (int) (this->positions_[mode_index] * 100 / D360);
    }
    int64_t bounded = this->positions_[mode_index] - this->mins_[mode_index];
    if (bounded < 0)
      bounded = 0;
    if (bounded > D360)
      bounded = D360;
    return (int) (bounded * 100 / D360);
  }

  int64_t positions_[13] = {0};
  int64_t mins_[13] = {0};
  bool bound_mode_12_ = false;
  bool wrap_position_ = false;
};

}  // namespace flic
}  // namespace esphome
