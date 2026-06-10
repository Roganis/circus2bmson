# circus2bmson

Convert vintage tracker music into [bmson](https://bmson-spec.readthedocs.io/),
the JSON chart format for BMS. First target: **MOD** (ProTracker) modules.

The output is a standard bmson folder with every note placed on the **BGM lane
(`x: 0`)** so the result plays back like the original module and can then be
charted by hand (e.g. dragged into BmsONE).

## Design

A headless **C++ core library** plus a thin **CLI** wrapper, so the core can
later link into other projects natively.

Keysounds are produced with **approach #2**: render each MOD channel in
isolation with libopenmpt (mute all-but-one via the interactive interface),
then slice each stem at note-on times. This reuses libopenmpt's accurate
effect/resampling/Paula emulation instead of reimplementing a tracker replayer.

Timing stays on a **musical pulse grid** (resolution 240, one tracker row =
60 pulses) rather than being converted through seconds, so notes land on clean
positions for hand-charting. Speed and tempo are folded into bmson `bpm_events`
(`bpm = 6 * tempo / speed`).

## Status — roadmap

- [x] **M0** — CMake project (library + CLI + tools), vendored deps, Linux CI.
- [x] **M0.5** — de-risk the stem approach: prove per-channel renders sum back
      to the full mix (`tools/stem_sum_check`, gated in CI).
- [ ] **M1** — timeline: flatten orders, build the note grid, `bpm_events`,
      `lines`; `--max-loops` for backward jumps.
- [ ] **M2** — render per-channel stems, slice + content-hash dedup into stereo
      WAV keysounds (panning baked in).
- [ ] **M3** — emit the bmson document + WAV folder; validate in beatoraja.
- [ ] **M4** — refinements: `9xx`/`EDx` sub-row precision, finetune, OGG,
      macOS/Windows CI.

## Building

Requires a C++17 compiler, CMake ≥ 3.16, and libopenmpt.

```sh
sudo apt-get install -y libopenmpt-dev cmake ninja-build   # Debian/Ubuntu
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

`nlohmann/json` and `dr_wav` are vendored under `third_party/`, so libopenmpt
is the only external dependency.

### Try the spike

```sh
./build/tools/stem_sum_check tests/fixtures/10k_reggae_dub.mod
```

## Layout

```
include/circus2bmson/  public library headers
src/                   core library
cli/                   command-line wrapper
tools/                 diagnostic / de-risking spikes
tests/                 ctest entries + fixtures
third_party/           vendored single-header deps (nlohmann/json, dr_wav)
```

## Licensing

Project code: see repository license. Test fixtures under `tests/fixtures/`
are Public Domain modules from The Mod Archive — see
[`tests/fixtures/README.md`](tests/fixtures/README.md). Vendored deps:
nlohmann/json (MIT), dr_wav (public domain / MIT-0).
