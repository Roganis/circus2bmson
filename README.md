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
- [x] **M1** — timeline: direct MOD parser + sequencer that flattens orders
      (`Bxx`/`Dxx`/`E6x`, `--max-loops`), builds the note grid, `bpm_events` and
      `lines`, and emits a bmson skeleton (notes on lane 0, provisional
      keysounds). Timing is cross-checked against libopenmpt's duration in CI.
- [x] **M2** — render per-channel stems, slice at note-on times, content-hash
      dedup into stereo WAV keysounds (pitch/effects/pan baked in), and bind each
      note to its keysound. A reconstruction test (keysounds replaced at their
      onsets vs libopenmpt's full mix, ~-80 dB residual) gates fidelity in CI.
- [ ] **M3** — polish the bmson/folder output and validate in beatoraja
      (drag into BmsONE; confirm the convert -> chart workflow).
- [ ] **M4** — refinements: `9xx`/`EDx` sub-row precision, finetune, OGG,
      macOS CI. (Windows CI + prebuilt `.exe` artifact: done.)

## Building

Requires a C++17 compiler, CMake ≥ 3.16, and libopenmpt.

```sh
sudo apt-get install -y libopenmpt-dev cmake ninja-build   # Debian/Ubuntu
sudo pacman -S --needed libopenmpt cmake ninja gcc          # Arch
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

`nlohmann/json` and `dr_wav` are vendored under `third_party/`, so libopenmpt
is the only external dependency.

### Windows

A prebuilt `circus2bmson.exe` (plus its DLLs and the sample modules) is
published by CI as the **`circus2bmson-windows-x64`** artifact on each run —
download it from the Actions tab, no toolchain required.

To build it yourself, use [MSYS2](https://www.msys2.org/) in the **UCRT64**
shell:

```sh
pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-libopenmpt
cmake -S . -B build -G Ninja && cmake --build build
```

### Convert

```sh
./build/cli/circus2bmson tests/fixtures/10k_reggae_dub.mod -o out
# writes out/10k_reggae_dub.bmson + descriptive keysound WAVs (BGM lane)
# --name-by channel     channel1_001.wav       (default; simple, per channel)
# --name-by instrument  s05_bass_ch01_A-2.wav  (descriptive, group by sample)
# --name-by lane        ch01_s05_bass_A-2.wav  (descriptive, group by channel)
# --volume-ramping      keep libopenmpt's anti-click ramp (yields more keysounds)
# --no-audio    emit the bmson skeleton only (structure, no WAVs)
# --max-loops N unroll a looping section N times
```

By default keysounds are named `channel{N}_{seq}.wav`, so they group by MOD
channel in an editor's sound list with no clutter. `--name-by instrument`/`lane`
instead encode the sample, sample name and note (`s05_bass_ch01_A-2.wav`).

Identical notes are deduplicated by exact audio: onsets are pinned to the sample
so repeats are byte-identical, and libopenmpt's volume ramping is off by default
(more authentic to Amiga, and it stops a note's attack from depending on the
previous note). The remaining distinct keysounds reflect genuine differences —
pitch, volume, effects, hold length and stereo panning.

### Try the de-risking spike

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
