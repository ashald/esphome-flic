// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov

// Host test: duo_input.h (Flic Duo click / hold / swipe / push-twist behaviour) on scripted
// sequences of decoded updates and push-twist notifications. Build + run via run.sh.
#include "duo_input.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace esphome::flic;

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
  // Units: 65536 per turn; default dial range 90 degrees = 16384; one step = 163.84; dead zone 1820.
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
  {  // the Duo saw a gesture but could not classify it: a failed swipe, not a click
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(B, 0, -2), 150); d.on_update(up(B, 6, -2), 650);
    d.on_update(up(S, 5), 1000); d.on_update(up(S, 1, -2), 1700);
    expect("unrecognised gesture fires nothing", r, {});
  }
  {  // a swipe that turns the Duo within the first half second: buffered, then dropped with the release
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), 1500, 150); d.on_push_twist(pt(1), 1700, 300);
    d.on_update(up(B, 0, 3), 390); d.on_update(up(B, 6, 3), 700);
    expect("fast swipe that turned", r, {"big:swipe_down"});
    expect_eq("fast swipe dial", d.dial(B), 0);
  }
  {  // twist: buffered until the button is held 0.5 s, then the rotation past the dead zone counts
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0);
    d.on_push_twist(pt(1), 2000, 100);
    d.on_push_twist(pt(1), 2000, 300);
    expect("twist: gate holds the first half second", r, {});
    d.on_push_twist(pt(1), 1000, 600);   // 5000 total, 3180 past the dead zone
    d.on_push_twist(pt(1), 1000, 700);   // 4180
    d.on_update(up(B, 7), 1000); d.on_update(up(B, 2), 1500);
    expect("twist", r, {"big:rotate_clockwise", "big:rotate_clockwise"});
    expect_eq("twist dial", d.dial(B), 4180);
  }
  {  // the Duo's own held-half-second flag opens the gate too
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_push_twist(pt(1, 0, 1), 5000, 200);
    expect("half-second flag", r, {"big:rotate_clockwise"});
  }
  {
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), -4000, 600); d.on_update(up(B, 1), 700);
    d.on_update(up(B, 6), 1200);  // stray timeout: nothing pending
    expect("short twist drops its click", r, {"big:rotate_counter_clockwise"});
    expect_eq("dial clamps at 0", d.dial(B), 0);
  }
  {  // wobble inside the dead zone during a hold: still a hold, dial untouched
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), 100, 600); d.on_push_twist(pt(1), -100, 650);
    d.on_push_twist(pt(1), 60, 700); d.on_push_twist(pt(1), 1500, 800);  // 1560 net, under 1820
    d.on_update(up(B, 7), 1000); d.on_update(up(B, 2), 1200);
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
    d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), 4000, 600); d.on_push_twist(pt(1), -1000, 700);
    expect("backlash holds a small reverse", r, {"big:rotate_clockwise"});
    d.on_push_twist(pt(1), -600, 800);
    expect("backlash passes a real reverse", r, {"big:rotate_clockwise", "big:rotate_counter_clockwise"});
    expect_eq("backlash dial", d.dial(B), 580);
  }
  {  // the dial starts moving just past the dead zone; the first step follows a step later
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), 1900, 600);
    expect("past the dead zone, below a step", r, {});
    expect_eq("dial follows past the dead zone", d.dial(B), 80);
    d.on_push_twist(pt(1), 100, 650);
    expect("one step", r, {"big:rotate_clockwise"});
    expect_eq("one step dial", d.dial(B), 180);
  }
  {  // both buttons held while turning: big dial moves, neither button holds
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_update(up(S, 5), 10); d.on_push_twist(pt(3), 5000, 600);
    d.on_update(up(B, 7), 1000); d.on_update(up(S, 7), 1010); d.on_update(up(B, 2), 1500); d.on_update(up(S, 2), 1510);
    expect("both held", r, {"big:rotate_clockwise"});
    expect_eq("both held small dial", d.dial(S), 0);
  }
  {  // small button pressed after the twist started is covered too
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), 5000, 600); d.on_update(up(S, 5), 700);
    d.on_push_twist(pt(3), 200, 800); d.on_update(up(S, 7), 1700); d.on_update(up(S, 2), 1900);
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
    d.set_dial(B, 16384); d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), 2000, 600);
    expect("steps at the end", r, {"big:rotate_clockwise"});
    expect_eq("end dial", d.dial(B), 16384);
    expect_eq("end dial calls", r.dial_calls, 0);
  }
  {  // millis() wrap between a swipe and a later twist
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0xFFFFFF00u); d.on_update(up(B, 0, 0), 0xFFFFFF80u);
    d.on_push_twist(pt(1, 1), 5000, 0x00000200u); d.on_push_twist(pt(1), 5000, 0x00000400u);
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
    d.on_push_twist(pt(1), 3000, 900); d.on_update(up(B, 4), 1000);
    expect("click then twist", r, {"big:rotate_clockwise", "big:click"});
  }
  {  // a slow swipe whose jerk turned the Duo past the dead zone: released within 1 s, the swipe wins
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), 3000, 600); d.on_update(up(B, 1, 2), 800);
    expect("slow swipe that turned", r, {"big:rotate_clockwise", "big:swipe_up"});
  }
  {  // a long twist that happens to end with a gesture stays a twist
    DuoInput d; Rec r; d.set_listener(&r);
    d.on_update(up(B, 5), 0); d.on_push_twist(pt(1), 5000, 600); d.on_update(up(B, 7), 1000);
    d.on_update(up(B, 2, 1), 1500);
    expect("long twist ending in a gesture", r, {"big:rotate_clockwise"});
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
