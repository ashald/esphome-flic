// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov

// Host test: twist_stream_guard.h (spotting a Twist stuck streaming rotation reports) on a real
// stuck stream recorded from a hub (twist_stuck_stream.txt) and on scripted everyday use. The
// parameters are the ones FlicTwist uses. Build + run via run.sh.
#include "twist_stream_guard.h"
#include <cstdio>
#include <utility>
#include <vector>
using namespace esphome::flic;

static const TwistStreamGuard::Params PARAMS{2000, 30000, 60, 49152 / 100, 300000};

using Stream = std::vector<std::pair<uint32_t, int32_t>>;  // (ms, raw movement)

// Times (ms) at which the guard asked for a reconnect.
static std::vector<uint32_t> run(TwistStreamGuard &g, const Stream &s) {
  std::vector<uint32_t> fired;
  for (auto &r : s)
    if (g.on_rotation_report(r.first, r.second))
      fired.push_back(r.first);
  return fired;
}

static int fails = 0, runs = 0;
static void check(const char *name, bool ok, const std::vector<uint32_t> &fired) {
  runs++;
  if (ok)
    return;
  fails++;
  printf("FAIL %s; reconnects at:", name);
  for (auto t : fired)
    printf(" %u", (unsigned) t);
  printf("\n");
}

// The recording repeated back to back for `ms`, one median gap (110 ms) between copies.
static Stream looped(const Stream &rec, uint32_t ms) {
  Stream out;
  for (uint32_t base = 0; base < ms; base += rec.back().first + 110)
    for (auto &r : rec)
      if (base + r.first < ms)
        out.push_back({base + r.first, r.second});
  return out;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: %s twist_stuck_stream.txt\n", argv[0]);
    return 2;
  }
  Stream rec;
  {
    FILE *f = fopen(argv[1], "r");
    if (f == nullptr) {
      printf("can't open %s\n", argv[1]);
      return 2;
    }
    char line[128];
    while (fgets(line, sizeof line, f) != nullptr) {
      unsigned ms;
      int d;
      if (line[0] != '#' && sscanf(line, "%u %d", &ms, &d) == 2)
        rec.push_back({ms, d});
    }
    fclose(f);
  }

  {  // the recorded stuck stream: one reconnect, 30 s in
    TwistStreamGuard g(PARAMS);
    auto f = run(g, rec);
    check("recorded stream", f.size() == 1 && f[0] >= 30000 && f[0] < 31000, f);
  }
  {  // the same stream cut short of 30 s: nothing
    TwistStreamGuard g(PARAMS);
    Stream s;
    for (auto &r : rec)
      if (r.first < 29000)
        s.push_back(r);
    auto f = run(g, s);
    check("29 s of stream", f.empty(), f);
  }
  {  // a stream that never stops: again only after the 5 min cooldown
    TwistStreamGuard g(PARAMS);
    auto f = run(g, looped(rec, 400000));
    check("endless stream", f.size() == 2 && f[0] >= 30000 && f[0] < 31000 && f[1] >= f[0] + 300000 &&
                                f[1] < f[0] + 301000, f);
  }
  {  // a new session 20 s in restarts the 30 s
    TwistStreamGuard g(PARAMS);
    std::vector<uint32_t> f;
    bool reconnected = false;
    for (auto &r : looped(rec, 60000)) {
      if (!reconnected && r.first >= 20000) {
        g.reset();
        reconnected = true;
      }
      if (g.on_rotation_report(r.first, r.second))
        f.push_back(r.first);
    }
    check("reset mid-stream", f.size() == 1 && f[0] >= 50000 && f[0] < 51000, f);
  }
  {  // turning it slowly but for real (1 % every 5 s): never a stream
    TwistStreamGuard g(PARAMS);
    Stream s;
    for (uint32_t t = 0; t < 120000; t += 125)
      s.push_back({t, 12});
    auto f = run(g, s);
    check("slow turn", f.empty(), f);
  }
  {  // fiddling in place, with a press every 20 s: someone is using it
    TwistStreamGuard g(PARAMS);
    std::vector<uint32_t> f;
    for (uint32_t t = 0; t < 120000; t += 100) {
      if (t % 20000 == 0)
        g.on_button_event();
      if (g.on_rotation_report(t, (t / 100) % 2 ? 20 : -20))
        f.push_back(t);
    }
    check("fiddling with presses", f.empty(), f);
  }
  {  // a stray report every 3 s (bumps, vibration): gaps end every run
    TwistStreamGuard g(PARAMS);
    Stream s;
    for (uint32_t t = 0; t < 600000; t += 3000)
      s.push_back({t, (t / 3000) % 2 ? 5 : -5});
    auto f = run(g, s);
    check("sparse reports", f.empty(), f);
  }
  {  // one report a second, not moving: a stream once 60 reports are in
    TwistStreamGuard g(PARAMS);
    Stream s;
    for (uint32_t t = 0; t < 90000; t += 1000)
      s.push_back({t, (t / 1000) % 2 ? 3 : -3});
    auto f = run(g, s);
    check("slow stream", f.size() == 1 && f[0] == 59000, f);
  }
  {  // millis() wrapping around mid-stream changes nothing
    TwistStreamGuard g(PARAMS);
    Stream s;
    for (auto &r : rec)
      s.push_back({r.first + 0xFFFFFFFFu - 10000u, r.second});
    auto f = run(g, s);
    check("millis wrap", f.size() == 1 && (uint32_t) (f[0] - (0xFFFFFFFFu - 10000u)) >= 30000, f);
  }

  printf("%d/%d twist stream guard checks passed\n", runs - fails, runs);
  return fails ? 1 : 0;
}
