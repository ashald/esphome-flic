// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov

// Host test: duo_input.h (Flic Duo click / hold / swipe / push-twist behaviour) on scripted
// sequences of decoded updates and push-twist notifications. Build + run via run.sh.
#include "duo_input.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace esphome::flic_twist;

struct Rec : DuoInputListener {
  std::vector<std::string> ev;
  int dial_calls = 0;
  void on_duo_event(uint8_t b, const char *type) override { ev.push_back(std::string(b ? "small:" : "big:") + type); }
  void on_duo_dial(uint8_t, int32_t) override { dial_calls++; }
};

static DuoUpdate up(uint8_t b, uint8_t type, int gesture = -1, bool flag = false, bool queued = false) {
  DuoUpdate u{};
  u.button = b;
  u.type = type;
  u.gesture = (int8_t) gesture;
  u.flag = flag;
  u.queued = queued;
  return u;
}
// push-twist flags: pressed mask (bits 0-1), first (2-3), held >= 0.5 s (4-5)
static uint8_t pt(uint8_t pressed, uint8_t first = 0, uint8_t half = 0) { return pressed | (first << 2) | (half << 4); }

static int fails = 0, runs = 0;
static void expect(const char *name, const Rec &r, std::vector<std::string> want) {
  runs++;
  if (r.ev != want) {
    fails++;
    printf("FAIL %s\n  got: ", name);
    for (auto &e : r.ev) printf("%s ", e.c_str());
    printf("\n  want:");
    for (auto &e : want) printf(" %s", e.c_str());
    printf("\n");
  }
}
static void expect_eq(const char *name, long long got, long long want) {
  runs++;
  if (got != want) {
    fails++;
    printf("FAIL %s: got %lld want %lld\n", name, got, want);
  }
}

int main() {
  const uint8_t B = 0, S = 1;
  {  // quick click: up within 0.5 s, then the single-click timeout
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(B, 0), 100); d.on_update(up(B, 6), 600);
    expect("quick click", r, {"big:click"});
  }
  {  // slow click: released after 0.5-1 s
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(S, 5), 0); d.on_update(up(S, 1), 700);
    expect("slow click", r, {"small:click"});
  }
  {
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(B, 0), 100); d.on_update(up(B, 5), 300); d.on_update(up(B, 3), 400);
    expect("double click", r, {"big:double_click"});
  }
  {
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(B, 7), 1000); d.on_update(up(B, 2), 1500);
    expect("hold", r, {"big:hold"});
  }
  {  // a short click then a long press: a double click, not a hold
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(B, 0), 100); d.on_update(up(B, 5), 300);
    d.on_update(up(B, 7, -1, true), 1300); d.on_update(up(B, 4, -1, true), 1500);
    expect("double click with held second press", r, {"big:double_click"});
  }
  {
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(B, 0, 1), 150); d.on_update(up(B, 6, 1), 650);
    expect("swipe replaces click", r, {"big:swipe_right"});
  }
  {
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(B, 1, 3), 700);
    expect("slow swipe", r, {"big:swipe_down"});
  }
  {
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(B, 0), 150); d.on_update(up(B, 6, 2), 650);
    expect("gesture reported with the timeout", r, {"big:swipe_up"});
  }
  {
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(B, 0, -2), 150); d.on_update(up(B, 6, -2), 650);
    expect("unrecognised gesture is a click", r, {"big:click"});
  }
  {  // twist: buffered for 250 ms, then follows; hold dropped
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0);
    d.on_push_twist(pt(1), 2000, 100);
    expect("twist: gate holds the first 250 ms", r, {});
    d.on_push_twist(pt(1), 2000, 300);
    d.on_push_twist(pt(1), 1000, 400);
    d.on_update(up(B, 7), 1000); d.on_update(up(B, 2), 1500);
    expect("twist", r, {"big:rotate_clockwise", "big:rotate_clockwise"});
    expect_eq("twist dial", d.dial(B), 5000);
  }
  {
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), -4000, 300); d.on_update(up(B, 1), 700);
    d.on_update(up(B, 6), 1200);  // stray timeout: nothing pending
    expect("short twist drops its click", r, {"big:rotate_counter_clockwise"});
    expect_eq("dial clamps at 0", d.dial(B), 0);
  }
  {  // wobble below one step during a hold: still a hold, dial untouched
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), 100, 300); d.on_push_twist(pt(1), -100, 400);
    d.on_push_twist(pt(1), 60, 500); d.on_update(up(B, 7), 1000); d.on_update(up(B, 2), 1200);
    expect("wobble during hold", r, {"big:hold"});
    expect_eq("wobble dial calls", r.dial_calls, 0);
  }
  {  // big wobble inside the gate window of a click: dropped with the release
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), 3000, 50); d.on_push_twist(pt(1), -2900, 100);
    d.on_update(up(B, 0), 150); d.on_update(up(B, 6), 650);
    expect("wobbly click", r, {"big:click"});
    expect_eq("wobbly click dial", d.dial(B), 0);
  }
  {  // push-twist right after a swipe is ignored
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(B, 0, 0), 150);
    d.on_push_twist(pt(1, 1, 1), 5000, 200);
    expect("swipe quiet", r, {"big:swipe_left"});
    expect_eq("swipe quiet dial", d.dial(B), 0);
  }
  {  // backlash: a small reverse is held back, a larger one goes through
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), 2000, 300); d.on_push_twist(pt(1), -1000, 400);
    expect("backlash holds a small reverse", r, {"big:rotate_clockwise"});
    d.on_push_twist(pt(1), -600, 500);
    expect("backlash passes a real reverse", r, {"big:rotate_clockwise", "big:rotate_counter_clockwise"});
    expect_eq("backlash dial", d.dial(B), 400);
  }
  {  // first step needs a full 1 % of range (163.84 units at 90 degrees)
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), 150, 300);
    expect("below one step", r, {});
    d.on_push_twist(pt(1), 20, 350);
    expect("one step", r, {"big:rotate_clockwise"});
    expect_eq("one step dial", d.dial(B), 170);
  }
  {  // both buttons held while turning: big dial moves, neither button holds
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(S, 5), 10); d.on_push_twist(pt(3), 5000, 300);
    d.on_update(up(B, 7), 1000); d.on_update(up(S, 7), 1010); d.on_update(up(B, 2), 1500); d.on_update(up(S, 2), 1510);
    expect("both held", r, {"big:rotate_clockwise"});
    expect_eq("both held small dial", d.dial(S), 0);
  }
  {  // small button pressed after the twist started is covered too
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), 5000, 300); d.on_update(up(S, 5), 400);
    d.on_push_twist(pt(3), 200, 500); d.on_update(up(S, 7), 1400); d.on_update(up(S, 2), 1600);
    expect("late second button", r, {"big:rotate_clockwise", "big:rotate_clockwise"});
  }
  {
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5, -1, false, true), 0); d.on_update(up(B, 0, -1, false, true), 100);
    d.on_update(up(B, 6, -1, false, true), 600); d.on_update(up(B, 5, -1, false, true), 700);
    d.on_update(up(B, 7, -1, false, true), 1700); d.on_update(up(B, 2, -1, false, true), 1800);
    expect("queued events are not replayed", r, {});
  }
  {  // at the end of the dial, steps keep coming (relative control)
    DuoInput d; Rec r; d.set_listener(&r);
    d.set_dial(B, 16384); d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), 2000, 300);
    expect("steps at the end", r, {"big:rotate_clockwise"});
    expect_eq("end dial", d.dial(B), 16384);
    expect_eq("end dial calls", r.dial_calls, 0);
  }
  {  // millis() wrap between a swipe and a later twist
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0xFFFFFF00u); d.on_update(up(B, 0, 0), 0xFFFFFF80u);
    d.on_push_twist(pt(1, 1), 5000, 0x00000200u); d.on_push_twist(pt(1), 5000, 0x00000300u);
    expect("wrap", r, {"big:swipe_left", "big:rotate_clockwise"});
  }
  {  // swipe, then a plain press within 0.5 s: the device reports a double click
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(B, 0, 1), 100); d.on_update(up(B, 5), 300); d.on_update(up(B, 3), 400);
    expect("swipe then click", r, {"big:swipe_right", "big:click"});
  }
  {  // plain press, then a swipe within 0.5 s
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(B, 0), 100); d.on_update(up(B, 5), 300); d.on_update(up(B, 3, 0), 400);
    expect("click then swipe", r, {"big:click", "big:swipe_left"});
  }
  {  // plain press, then a twist starting within 0.5 s
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(B, 0), 100); d.on_update(up(B, 5), 300);
    d.on_push_twist(pt(1), 3000, 600); d.on_update(up(B, 4), 900);
    expect("click then twist", r, {"big:rotate_clockwise", "big:click"});
  }
  {  // the two buttons are independent
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(S, 5), 50); d.on_update(up(B, 0), 100); d.on_update(up(S, 1), 700);
    d.on_update(up(B, 6), 600);
    expect("independent buttons", r, {"small:click", "big:click"});
  }
  {  // a new session keeps the dials
    DuoInput d; d.set_dial(S, 8192); d.reset_session();
    expect_eq("dial survives a session reset", d.dial(S), 8192);
  }
  printf("%d checks, %d failures\n", runs, fails);
  return fails ? 1 : 0;
}
