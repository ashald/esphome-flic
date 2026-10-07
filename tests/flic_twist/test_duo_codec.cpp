// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov

// Host test: duo_codec.h against spec-encoded vectors from gen_vectors.py (see run.sh).
#include "duo_codec.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace esphome::flic_twist;

int main(int argc, char **argv) {
  FILE *f = fopen(argc > 1 ? argv[1] : "vectors.txt", "r");
  if (!f) { perror("vectors"); return 2; }
  static char line[1 << 16];
  int n = 0, bad = 0;
  unsigned long long nev = 0;
  while (fgets(line, sizeof(line), f)) {
    char hex[8192], evs[1 << 15];
    unsigned long long ts0, ts1; unsigned long c00, c01, c10, c11; int eoq0, eoq1, nexp;
    if (sscanf(line, "%8191s %llu %lu %lu %d %llu %lu %lu %d %d %32767s", hex, &ts0, &c00, &c01, &eoq0, &ts1, &c10, &c11,
               &eoq1, &nexp, evs) != 11) { fprintf(stderr, "parse error line %d\n", n + 1); return 2; }
    std::vector<uint8_t> data;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) { char b[3] = {hex[i], hex[i + 1], 0}; data.push_back((uint8_t) strtoul(b, nullptr, 16)); }
    DuoDecoderState st; st.ts_ms = ts0; st.count[0] = c00; st.count[1] = c01; st.end_of_queue = eoq0;
    std::vector<DuoUpdate> got;
    duo_decode_events(st, data.data(), data.size(), [&](const DuoUpdate &u) { got.push_back(u); });
    bool ok = got.size() == (size_t) nexp && st.ts_ms == ts1 && st.count[0] == c10 && st.count[1] == c11 && st.end_of_queue == (bool) eoq1;
    if (ok && nexp > 0) {
      char *save = nullptr; char *tok = strtok_r(evs, ";", &save);
      for (int k = 0; k < nexp && tok; k++, tok = strtok_r(nullptr, ";", &save)) {
        int b, t, fl, g, q, ax, ay, az; unsigned long cnt; unsigned long long ts;
        sscanf(tok, "%d,%d,%d,%d,%d,%d,%d,%d,%lu,%llu", &b, &t, &fl, &g, &q, &ax, &ay, &az, &cnt, &ts);
        const DuoUpdate &u = got[k];
        if (u.button != b || u.type != t || u.flag != (bool) fl || u.gesture != g || u.queued != (bool) q || u.accel[0] != ax ||
            u.accel[1] != ay || u.accel[2] != az || u.count != cnt || u.ts_ms != ts) {
          ok = false;
          fprintf(stderr, "line %d event %d: got b%u t%u f%d g%d q%d a%d,%d,%d c%u ts%llu, want %s\n", n + 1, k, u.button, u.type, u.flag,
                  u.gesture, u.queued, u.accel[0], u.accel[1], u.accel[2], u.count, (unsigned long long) u.ts_ms, tok);
          break;
        }
      }
    }
    if (!ok) {
      if (bad < 5) fprintf(stderr, "MISMATCH line %d: got %zu events (want %d), ts %llu/%llu counts %u,%u/%lu,%lu eoq %d/%d\n", n + 1,
                           got.size(), nexp, (unsigned long long) st.ts_ms, ts1, st.count[0], st.count[1], c10, c11, st.end_of_queue, eoq1);
      bad++;
    }
    nev += got.size();
    n++;
  }
  printf("%d packets, %llu events decoded, %d mismatches\n", n, nev, bad);
  return bad ? 1 : 0;
}
