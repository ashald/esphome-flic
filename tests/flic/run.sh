#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Borys Pierov
# Host tests for the flic component's protocol logic:
#   test_flic2_events  Flic 2 event codes -> click / double_click / hold + ACK rule (flic2_events.h)
#   test_duo_codec     Flic Duo bit-packed event decoder (duo_codec.h) vs spec-encoded packets
#   test_duo_input     Flic Duo click / hold / swipe / push-twist behaviour (duo_input.h)
# Needs python3 and a C++17 compiler; pyflic-ble is optional (cross-check only).
set -euo pipefail
cd "$(dirname "$0")"
CXX=${CXX:-g++}
FLAGS=(-std=c++17 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -I ../../components/flic)
out=$(mktemp -d)
trap 'rm -rf "$out" vectors.txt' EXIT
for t in test_flic2_events test_duo_codec test_duo_input; do
  $CXX "${FLAGS[@]}" "$t.cpp" -o "$out/$t"
done
"$out/test_flic2_events"
"$out/test_duo_input"
for seed in 1 2 3; do
  python3 gen_vectors.py "$seed" 4000
  "$out/test_duo_codec" vectors.txt
done
