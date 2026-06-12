# circus2bmson

Convert vintage tracker music into [bmson](https://bmson-spec.readthedocs.io/),
the JSON chart format for BMS. Accepts **any format libopenmpt plays** — MOD
(ProTracker & variants), XM, S3M, IT, and dozens more.

The output is a standard bmson folder with every note placed on the **BGM lane
(`x: 0`)** so the result plays back like the original module and can then be
charted by hand (e.g. dragged into BmsONE).

## Design

A headless **C++ core library** plus a thin **CLI** wrapper, so the core can
later link into other projects natively.

The score is read **format-agnostically through libopenmpt itself**: the module
is played once and every row's onset is pinned to the exact sample, so control
flow (jumps, breaks, loops, pattern delay) and tempo semantics are whatever
libopenmpt actually played — no per-format sequencer to maintain. An
independent MOD parser + sequencer is kept in-tree as a validation oracle and
must agree with the generic engine in CI.

Keysounds are produced by rendering each pattern channel in isolation (mute
all-but-one via libopenmpt's interactive interface) and slicing each stem at
note-on times — pitch, effects and panning baked in, reusing libopenmpt's
accurate playback instead of reimplementing one.

Timing stays on a **musical pulse grid** (resolution 240, one tracker row =
60 pulses). BPM events use `6 * tempo / speed` when it matches the measured
row duration, and the measured duration otherwise — so pattern delay and tempo
slides keep the grid exactly aligned with the rendered audio.

## Status — roadmap

- [x] **M0** — CMake project (library + CLI + tools), vendored deps, Linux CI.
- [x] **M0.5** — de-risk the stem approach: prove per-channel renders sum back
      to the full mix (`tools/stem_sum_check`; gated in CI for MOD, XM and IT).
- [x] **M1** — timeline on a clean pulse grid: note grid, `bpm_events`, `lines`,
      `--max-loops` for looping songs; MOD parser + sequencer kept as the
      validation oracle for the generic engine.
- [x] **M2** — per-channel stems sliced into deduplicated stereo keysounds
      (exact onsets; ~-80 dB reconstruction residual gated in CI).
- [x] **Wider formats** — generic score via libopenmpt: XM/S3M/IT and every
      other libopenmpt format use the same pipeline (XM + IT covered by
      fixtures in CI).
- [x] **OGG** — `--format ogg` encodes keysounds with libvorbis (~70 % smaller
      folders).
- [ ] **M3** — validate in beatoraja (drag into BmsONE; confirm the
      convert -> chart workflow).
- [ ] **M4** — remaining refinements: sub-row precision (note delay), macOS CI.
      (Windows CI + prebuilt `.exe` artifact: done.)

## Building

Requires a C++17 compiler, CMake ≥ 3.16, libopenmpt and libvorbis.

```sh
sudo apt-get install -y libopenmpt-dev libvorbis-dev cmake ninja-build  # Debian/Ubuntu
sudo pacman -S --needed libopenmpt libvorbis cmake ninja gcc            # Arch
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

`nlohmann/json` and `dr_wav` are vendored under `third_party/`, so libopenmpt
and libvorbis are the only external dependencies.

### GUI (early / minimal)

An optional desktop front-end (Dear ImGui, vendored) is built with
`-DC2B_BUILD_GUI=ON` (needs GLFW: apt `libglfw3-dev` + `libgl1-mesa-dev`, or
MSYS2 `mingw-w64-ucrt-x86_64-glfw`). It wraps the same conversion as the CLI:
point it at a module (type/paste a path, **Browse…**, or drag one onto the
window), set the options (WAV/OGG, keysound naming, max-loops, volume ramping,
output folder) and Convert. A SoundFont (`.sf2`) picker is present for the
forthcoming MIDI backend (inert for tracker input).

Input methods degrade gracefully by environment: the **path field** always
works (no dependencies); **Browse…** needs a system dialog helper
(`zenity`/`kdialog`); **drag-and-drop** needs GLFW's X11 backend — the app
prefers X11 (XWayland under Wayland), but a native Wayland session with no
XWayland cannot deliver file drops, so use the path field there.

CI publishes prebuilt binaries: `circus2bmson-gui.exe` in the
`circus2bmson-windows-x64` artifact, and a Linux
**`circus2bmson-x86_64.AppImage`** (CLI + GUI bundled) in the
`circus2bmson-linux-x86_64` artifact. Run the AppImage directly (it needs FUSE —
`fuse2` on most distros; otherwise
`./circus2bmson-x86_64.AppImage --appimage-extract-and-run`).

```sh
cmake -S . -B build -G Ninja -DC2B_BUILD_GUI=ON && cmake --build build
./build/gui/circus2bmson-gui
```

### Windows

A prebuilt `circus2bmson.exe` (plus its DLLs and the sample modules) is
published by CI as the **`circus2bmson-windows-x64`** artifact on each run —
download it from the Actions tab, no toolchain required.

To build it yourself, use [MSYS2](https://www.msys2.org/) in the **UCRT64**
shell:

```sh
pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-libopenmpt mingw-w64-ucrt-x86_64-libvorbis
cmake -S . -B build -G Ninja && cmake --build build
```

### Convert

```sh
./build/cli/circus2bmson song.it -o out      # any libopenmpt format
./build/cli/circus2bmson song.mod            # -> song/song.bmson beside the file
```

With no `-o`, output goes to a folder named after the module, beside it — so on
Windows you can just **drag a module onto `circus2bmson.exe`** and get a
`song/song.bmson` folder next to the file. Options:

```sh
# circus2bmson <module> [-o DIR] [options]
# --format wav|ogg      keysound container (default wav; ogg is ~70 % smaller)
# --name-by channel     channel1_001.wav       (default; simple, per channel)
# --name-by instrument  s05_bass_ch01_A-2.wav  (descriptive, group by instrument)
# --name-by lane        ch01_s05_bass_A-2.wav  (descriptive, group by channel)
# --volume-ramping      keep libopenmpt's anti-click ramp (yields more keysounds)
# --no-audio    emit the bmson skeleton only (structure, no keysounds)
# --max-loops N unroll a looping section N times
```

By default keysounds are named `channel{N}_{seq}`, so they group by pattern
channel in an editor's sound list with no clutter. `--name-by instrument`/`lane`
instead encode the instrument number, its name and the note.

Identical notes are deduplicated by exact audio: onsets are pinned to the sample
so repeats are byte-identical, and libopenmpt's volume ramping is off by default
(more authentic to Amiga, and it stops a note's attack from depending on the
previous note). The remaining distinct keysounds reflect genuine differences —
pitch, volume, effects, hold length and stereo panning.

Format notes: IT *new-note actions* let an old note ring past the next note-on
on the same channel; that tail is baked into the start of the following
keysound (playback is still faithful — it just makes that keysound less
"clean" in isolation).

### Try the de-risking spike

```sh
./build/tools/stem_sum_check tests/fixtures/neurosys.xm
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
nlohmann/json (MIT), dr_wav (public domain / MIT-0), Dear ImGui (MIT, GUI only).
