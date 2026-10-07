// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov
// Portions derived from pyflic-ble (https://github.com/50ButtonsEach/pyflic-ble),
// Copyright Shortcut Labs, Apache-2.0: ported to C++ and modified. See NOTICE.

#pragma once
// Turns decoded Flic Duo updates (duo_codec.h) and push-twist notifications into the events and dial
// movements the component exposes. Header-only and free of ESPHome dependencies so the behaviour
// can be tested on a host (tests/flic). FlicDuo feeds it and forwards its output to the entities.
//
// Events follow the spec's "single click / double click / hold" use case, with refinements so one
// physical gesture yields one event:
//   - a press whose release carries a recognised gesture is a swipe: swipe_<dir> fires instead of
//     its click. A swipe means jerking the whole Duo, which also turns it, so on a release within
//     1 s the swipe wins even if the turning already counted as a twist;
//   - a gesture the Duo saw but could not classify is a failed swipe: nothing fires, not a click;
//   - a press that turned the Duo past the dead zone is a push-twist: its click / double_click /
//     hold are dropped, for every button held during it. A hold fires ~1 s into a press, so a twist
//     that starts later keeps its hold.
// A double click whose presses were not both plain clicks resolves to what they were: the plain
// press still fires its click (and a swipe its swipe).
// Queued updates (from before the session) only update the press state; nothing is emitted.
//
// Rotation counts only once the button has been held ROTATION_GATE_MS (the Duo's own "held half a
// second" flag); what turned before that is buffered, and dropped on an earlier release. Then the
// press must turn TWIST_DEAD_ZONE_UNITS before anything moves, and the dial follows the rotation
// beyond that, so a hold, slow click or swipe that turns the Duo a little never registers as a
// twist (measured: swipes turn it up to ~18 degrees, mostly within half a second). Also: zero
// deltas dropped, push-twist ignored for SWIPE_QUIET_MS after a swipe, backlash suppression per
// press (a change of direction needs BACKLASH_UNITS of reverse travel). Both buttons held: the big
// button's dial. The dial is clamped to 0..dial_range; a rotate event fires per notification in
// which the press's unclamped travel (beyond the dead zone) crossed a 1 %-of-range step, so
// relative control keeps working at either end.

#include <cstdint>
#include <cstdlib>  // std::abs

#include "duo_codec.h"

namespace esphome {
namespace flic {

class DuoInputListener {
 public:
  virtual void on_duo_event(uint8_t button, const char *type) = 0;
  virtual void on_duo_dial(uint8_t button, int32_t units) = 0;  // the (clamped) dial moved
};

class DuoInput {
 public:
  static constexpr uint32_t ROTATION_GATE_MS = 500;        // = the Duo's "held >= 0.5 s" flag
  static constexpr int32_t TWIST_DEAD_ZONE_UNITS = 1820;   // 10 degrees at 65536 units per turn
  static constexpr uint32_t SWIPE_QUIET_MS = 300;
  static constexpr int32_t BACKLASH_UNITS = 1500;  // ~8 degrees at 65536 units per turn

  static constexpr const char *EV_CLICK = "click";
  static constexpr const char *EV_DOUBLE_CLICK = "double_click";
  static constexpr const char *EV_HOLD = "hold";
  static constexpr const char *EV_ROTATE_CW = "rotate_clockwise";
  static constexpr const char *EV_ROTATE_CCW = "rotate_counter_clockwise";
  static const char *swipe_event(int gesture) {
    static const char *const SWIPES[4] = {"swipe_left", "swipe_right", "swipe_up", "swipe_down"};
    return SWIPES[gesture & 3];
  }

  void set_listener(DuoInputListener *listener) { this->listener_ = listener; }
  void set_dial_range(int32_t units) { this->dial_range_ = units > 0 ? units : 16384; }
  int32_t dial_range() const { return this->dial_range_; }
  int32_t dial(uint8_t button) const { return this->btn_[button & 1].units; }

  // Re-base a dial (clamped). Emits nothing; the caller publishes.
  void set_dial(uint8_t button, int32_t units) {
    ButtonState &st = this->btn_[button & 1];
    st.units = units < 0 ? 0 : (units > this->dial_range_ ? this->dial_range_ : units);
    st.dir = 0;
    st.reverse_buf = 0;
  }

  // New session: press tracking restarts; the dials persist.
  void reset_session() {
    for (auto &st : this->btn_) {
      const int32_t units = st.units;
      st = ButtonState{};
      st.units = units;
    }
  }

  void on_update(const DuoUpdate &u, uint32_t now) {
    const uint8_t b = u.button & 1;
    ButtonState &st = this->btn_[b];
    switch (u.type) {
      case 5:  // down
        this->start_press_(st, now);
        return;
      case 7:  // hold (~1 s into a press); flag = the next up completes a double click
        if (!u.queued && !u.flag && !st.twisted)
          this->emit_(b, EV_HOLD);
        return;
      case 6:  // single-click timeout: a short press (type 0) did not become a double click
        if (!u.queued && st.pending == PENDING_CLICK && u.gesture != -2)
          this->emit_(b, u.gesture >= 0 ? swipe_event(u.gesture) : EV_CLICK);
        st.pending = PENDING_NONE;
        return;
      default: {  // 0-4: up
        st.pressed = false;
        st.gate_open = false;
        st.gate_buf = 0;
        // Released within 1 s (not after a hold): a recognised gesture is a swipe even if the jerk
        // turned the Duo past the dead zone. After a longer press the twist wins.
        const bool short_release = u.type == 0 || u.type == 1 || u.type == 3;
        const bool swipe = u.gesture >= 0 && (short_release || !st.twisted);
        // A twist drops its click; so does a swipe, and a gesture the Duo could not classify.
        const bool swallowed = st.twisted || swipe || u.gesture == -2;
        if (swipe) {
          st.swipe_quiet = true;
          st.swipe_ms = now;
        }
        if (u.type == 0) {
          // Released within 0.5 s: a click only once the timeout (6) says no second press came, or
          // the first half of a double click (3 / 4).
          if (swipe && !u.queued)
            this->emit_(b, swipe_event(u.gesture));
          st.pending = swallowed ? PENDING_SWALLOWED : PENDING_CLICK;
          return;
        }
        const uint8_t first = st.pending;
        st.pending = PENDING_NONE;
        if (u.queued)
          return;
        if (u.type == 3 || u.type == 4) {
          // The second press of a double click: only a double click if both presses were clicks;
          // otherwise the plain one is a click (the first press's comes first).
          if (swallowed) {
            if (first == PENDING_CLICK)
              this->emit_(b, EV_CLICK);
          } else if (first == PENDING_SWALLOWED) {
            this->emit_(b, EV_CLICK);
          } else {
            this->emit_(b, EV_DOUBLE_CLICK);
          }
        } else if (u.type == 1 && !swallowed) {
          this->emit_(b, EV_CLICK);  // released after 0.5-1 s: a single click, no double possible
        }
        // type 2: released after a hold, which already fired (or was dropped by a twist).
        if (swipe)
          this->emit_(b, swipe_event(u.gesture));
        return;
      }
    }
  }

  // PushTwistDataNotification: flags = buttons_pressed:2 | is_first_event:2 | held_half_second:2.
  void on_push_twist(uint8_t flags, int32_t diff, uint32_t now) {
    const uint8_t pressed = flags & 0x03, first = (flags >> 2) & 0x03, half = (flags >> 4) & 0x03;
    if (diff == 0 || pressed == 0)
      return;
    const uint8_t b = (pressed & 0x01) ? 0 : 1;  // both held: the big button's dial
    const uint8_t bit = (uint8_t) (1u << b);
    ButtonState &st = this->btn_[b];
    if (st.swipe_quiet) {
      if ((uint32_t) (now - st.swipe_ms) < SWIPE_QUIET_MS)
        return;  // the wobble of a swipe that just ended
      st.swipe_quiet = false;
    }
    if (!st.pressed && (first & bit))
      this->start_press_(st, now);  // the down event never arrived: time the press from here
    if (!st.gate_open) {
      if ((half & bit) != 0 || (st.pressed && (uint32_t) (now - st.down_ms) >= ROTATION_GATE_MS)) {
        st.gate_open = true;
        const int32_t total = st.gate_buf + diff;
        st.gate_buf = 0;
        if (total != 0)
          this->apply_rotation_(b, total, pressed);
      } else {
        st.gate_buf += diff;  // first 250 ms of a press: could still be a click or a swipe
      }
      return;
    }
    this->apply_rotation_(b, diff, pressed);
  }

 protected:
  enum : uint8_t { PENDING_NONE = 0, PENDING_CLICK, PENDING_SWALLOWED };

  struct ButtonState {
    bool pressed{false};
    uint32_t down_ms{0};
    bool twisted{false};  // this press turned the Duo by at least one step
    // A short press (released < 0.5 s) awaiting its single-click timeout or a second press: a plain
    // click, or a swipe / twist whose click is dropped.
    uint8_t pending{PENDING_NONE};
    bool gate_open{false};
    int32_t gate_buf{0};
    bool swipe_quiet{false};
    uint32_t swipe_ms{0};
    int8_t dir{0};          // backlash: direction of travel in this press (+1 cw / -1 ccw / 0)
    int32_t reverse_buf{0};  // backlash: reverse rotation not yet accepted
    int32_t units{0};         // dial, 0..dial_range_
    int64_t press_travel{0};  // rotation in this press (after filtering), unclamped
    int64_t press_eff{0};     // press_travel beyond the dead zone, already applied to the dial
    int64_t press_steps{0};   // whole steps of press_eff already reported
  };

  void start_press_(ButtonState &st, uint32_t now) {
    st.pressed = true;
    st.down_ms = now;
    st.twisted = false;
    st.gate_open = false;
    st.gate_buf = 0;
    st.dir = 0;  // backlash filtering is per press
    st.reverse_buf = 0;
    st.press_travel = 0;
    st.press_eff = 0;
    st.press_steps = 0;
    // st.pending is kept: this may be the second press of a double click.
  }

  void apply_rotation_(uint8_t b, int32_t diff, uint8_t pressed_mask) {
    ButtonState &st = this->btn_[b];
    // Backlash suppression (pyflic-ble RotateTracker): reverse movement is buffered until it exceeds
    // BACKLASH_UNITS; forward movement cancels the buffer, so the net position stays exact.
    int32_t applied = 0;
    if (st.dir != 0) {
      const bool forward = (diff > 0) == (st.dir > 0);
      if (forward && st.reverse_buf == 0) {
        applied = diff;
      } else {
        st.reverse_buf += diff;
        if (st.reverse_buf == 0) {
          // jitter cancelled out
        } else if ((st.reverse_buf > 0) == (st.dir > 0)) {
          applied = st.reverse_buf;  // swung back forward
          st.reverse_buf = 0;
        } else if (std::abs(st.reverse_buf) >= BACKLASH_UNITS) {
          applied = st.reverse_buf;  // a real change of direction
          st.dir = applied > 0 ? 1 : -1;
          st.reverse_buf = 0;
        }
      }
    } else {
      applied = diff;
      st.dir = diff > 0 ? 1 : -1;
    }
    if (applied == 0)
      return;

    st.press_travel += applied;
    // The dead zone: the dial follows only the rotation beyond TWIST_DEAD_ZONE_UNITS either way.
    const int64_t t = st.press_travel;
    const int64_t eff = t > TWIST_DEAD_ZONE_UNITS    ? t - TWIST_DEAD_ZONE_UNITS
                        : t < -TWIST_DEAD_ZONE_UNITS ? t + TWIST_DEAD_ZONE_UNITS
                                                     : 0;
    if (!st.twisted) {
      if (eff == 0)
        return;  // not a twist (yet): a wobble does not move the dial
      st.twisted = true;
    }
    // Every button held while the Duo turns loses its click / hold (one pressed later included).
    for (uint8_t i = 0; i < 2; i++) {
      if (pressed_mask & (1u << i))
        this->btn_[i].twisted = true;
    }
    const int64_t move = eff - st.press_eff;
    st.press_eff = eff;
    int64_t u = (int64_t) st.units + move;
    if (u < 0)
      u = 0;
    if (u > this->dial_range_)
      u = this->dial_range_;
    if (u != st.units) {
      st.units = (int32_t) u;
      if (this->listener_ != nullptr)
        this->listener_->on_duo_dial(b, st.units);
    }
    // Whole steps of the rotation beyond the dead zone, truncated toward zero.
    const int64_t steps_now = eff * 100 / this->dial_range_;
    const int64_t steps = steps_now - st.press_steps;
    st.press_steps = steps_now;
    if (steps != 0)
      this->emit_(b, steps > 0 ? EV_ROTATE_CW : EV_ROTATE_CCW);
  }

  void emit_(uint8_t b, const char *type) {
    if (this->listener_ != nullptr)
      this->listener_->on_duo_event(b, type);
  }

  DuoInputListener *listener_{nullptr};
  int32_t dial_range_{16384};  // 90 degrees
  ButtonState btn_[2];
};

}  // namespace flic
}  // namespace esphome
