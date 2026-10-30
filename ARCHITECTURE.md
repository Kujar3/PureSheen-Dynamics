# PureSheen Dynamics: architecture & team contract

PureSheen Dynamics is a full mastering suite (VST3 + Standalone + CLAP, JUCE 8, C++17, Windows / FL Studio first) whose
feature set follows a top-tier commercial mastering suite module for module and control for control. It has its own name, its
own algorithms and its own UI, in the visual family of Chordbloom, RefMatch and Duckling (dark slate, Inter, mint
accent). The research notes are private.

**Scope.** Core (phase 1, this document):
- the chain host
- Equalizer 1 and 2
- Dynamics
- Imager
- Exciter
- Maximizer
- Dither
- I/O metering, gain match, delta, undo history, A-D snapshots, presets

Later phases (enums are append-only, so they slot in):
- Phase 2: Dynamic EQ, Match EQ, Tone Balance, Definition, Smoother, Punch, Low Contrast, Bass Shaper,
  Transient/Sustain mode
- Phase 3: Vintage modules, Unlimiter, rule-based Assistant, Track Referencing (from RefMatch), and one plug-in per
  module ("PureSheen EQ", "PureSheen Maximizer", ...)
- Phase 4: MCP bridge

Never built: stem-separation EQ / focus, mix rebalancing, codec preview.

**Naming rule.** Never use another vendor's product, module or algorithm names in code, UI text or docs shipped to users.
The research doc may mention them.

## Repository layout

```
CMakeLists.txt            targets: PureSheen (VST3, Standalone, + PureSheen_CLAP), CoreTests, PureSheenDev; sources
                          globbed per folder (a new .cpp is picked up on the next configure)
scripts/build.bat         scripts\build.bat <buildDir> <targets...>   (Ninja + MSVC 2022 x64 Release)
scripts/fetch_clap.bat, clap-juce-extensions-state-rescan.patch, install-plugin.ps1, build-installer.bat
                          (adapted from RefMatch at the end of Core)
src/core/                 measurement core copied from RefMatch (pure C++17): Common.h (dB helpers),
                          Loudness (BS.1770 M/S/I/LRA, realtime safe), TruePeak (4x), Fft (radix-2 forward)
src/dsp/                  the DSP (pure C++17, no JUCE, unit-tested by CoreTests)
  Types.h/.cpp            ModuleType (+ moduleInfo), ChannelMode, CrossoverType, EqShape, MaximizerMode, ... enums
  Params.h, ParamTable.cpp  parameter table (ids, ranges, defaults) + typed param structs + read*Params
  Filters.h/.cpp          matched biquad designs, Biquad, SmoothedBiquad
  Crossover.h/.cpp        stereo N-band splitter (Analog LR4 / Digital linear phase / Hybrid)
  Oversampler.h/.cpp      2x/4x/8x half-band oversampling (linear / minimum phase)
  Convolver.h/.cpp        partitioned FFT convolver with lock-free kernel swap
  Envelope.h/.cpp         LevelDetector, GainCurve, GainSmoother, DelayLine, SlidingMax, LinearSmoother
  Fifo.h                  AtomicFloat, StereoSampleFifo (audio -> UI)
  Module.h                Module base, ProcessContext, ModuleMonitor
  Equalizer.h/.cpp (+ EqDesign.cpp), Dynamics.h/.cpp, Imager.h/.cpp, Exciter.h/.cpp, Maximizer.h/.cpp (+ more
  files if needed), Dither.h/.cpp
  Chain.h/.cpp            the chain host: order, slots (channel modes, power, solo, delta, difference meters), I/O
                          gain, gain match, mono / swap, bypass, dither, meters, FIFOs, latency
src/plugin/               PluginProcessor.h/.cpp (+ UndoHistory.cpp, Presets.cpp, FactoryPresets.cpp)
src/ui/                   Theme (from RefMatch), Widgets (+ Spectrum, Options, ModulePanel, ModulePanels),
                          PluginEditor, HeaderBar, ChainStrip, IoPanel, DitherPanel, HelpOverlay,
                          EqPanel, DynamicsPanel, ImagerPanel, ExciterPanel, MaximizerPanel, MultibandDisplay
tests/                    CoreTests (TestFramework.h, namespace pstest; TestSignals.h generators)
tools/dev/                PureSheenDev: selftest (processor-level + realtime allocation checks via
                          AllocationTracking), snapshot <dir> (editor PNGs), render <in.wav> <out.wav>
resources/fonts           Inter 400/600/700 (PureSheenBinary::Inter400_ttf ...)
third_party/              JUCE 8.0.16, clap-juce-extensions (gitignored; copied from RefMatch)
tools/pluginval, tools/clap-validator (gitignored)
out/                      snapshots, renders, measurements (gitignored)
```

## Rules for everyone

1. **Headers are contracts.** The public parts of every header under `src/dsp`, `src/plugin/PluginProcessor.h`,
   `src/ui/Widgets.h` and `src/ui/PluginEditor.h` were written up front so the modules can be built in parallel.
   - Implement them as written.
   - You may *add* things: private members, helpers, new public methods, new files.
   - Do not change or remove existing public signatures. If a contract is genuinely wrong, make the smallest fix and
     report it in your final report.
2. **Only edit the files you own** (your task lists them). Other agents are editing other files at the same time.
3. **Build in your own build directory.** Use `build-<yourname>`, e.g. from Git Bash in the repo root:
   `cmd //c "scripts\\build.bat build-eq CoreTests"`.
   - The first JUCE build of a directory takes several minutes.
   - CoreTests (pure C++) builds in seconds. Develop DSP against it.
   - If the build fails in a file you do not own, that is someone else's work in progress: retry later, or test
     with a filter (`CoreTests eq.`).
4. **Audio thread.** No allocation, no locks (try-lock at most), no logging, no `juce::String` building, no file I/O,
   no `std::function` construction.
   - `prepare()` may allocate. `process()` / `setParams()` may not.
   - The selftest enforces this with AllocationTracking.
   - Worker threads (the EQ's FIR designer) never block the audio thread. Hand data over through preallocated
     slots and atomics.
5. **Determinism.** DSP is deterministic: no `rand`, no time. Dither and tests use their own LCG.
6. **Quality bar = mastering.** Double-precision filter state and design. No zipper noise, so every parameter is
   smoothed or crossfaded. No denormals: flush in `process`, or add a 1e-20 DC offset internally and remove it. No
   aliasing from nonlinear stages: oversample them. Latency is exact and constant while only parameters move. Every
   module is transparent (null test < -120 dBFS) at neutral settings, unless the module adds latency (then it equals
   the input delayed by the latency) or saturates by design.
7. **Code style.** JUCE-like: 4-space indent, braces on new lines, `camelCase` methods, `Type& name`, C++17.
   - Namespaces: `ps` for DSP and core, `ps::ui` for UI.
   - **0 warnings in our sources**: MSVC /W4 via `juce_recommended_warning_flags`; CoreTests /W4 /permissive-.
8. **Tests.**
   - Every DSP file gets CoreTests in `tests/<Area>Tests.cpp`.
   - Test names are prefixed with your area (`"eq. digital mode is linear phase"`), so a filter runs only yours.
   - Measure with real numbers (frequency response via a sine sweep or impulse + FFT, THD, null tests, true peak),
     not just "doesn't crash".
9. **User-facing text** is ASCII only and plain language. FL Studio is the main host (1920x1080).
10. **reference parity.** Where the research documents a behaviour, match it. Where it does not, choose what a mastering
    engineer would expect, and note the choice in a comment at the top of the module's .cpp.

## Threading model

- The **audio thread** runs `PureSheenProcessor::processBlock`, which:
  1. copies the parameter atomics into a float array;
  2. calls `ps::Chain::process`;
  3. passes each module its params through `setParams (ParamValues)`, then calls `process`.
- The **message thread** owns the APVTS, chain order edits (`Chain::setOrder`, lock-free publish), monitoring
  (solo / delta / band solo / audition), undo history, presets, snapshots and the editor. When
  `Chain::getLatencySamples()` changes, the processor calls `setLatencySamples` on the message thread (async update).
- The **UI** reads the meters (AtomicFloat) and pulls audio from the chain's FIFOs (input, output, module pre / post
  of the selected module). It does its FFTs on the message thread (`ps::ui::SpectrumAnalyser`, 30 Hz).
- **Worker threads:** the EQ's FIR designer, one per EQ, owned by the module.

## Parameters

`ps::paramSpecs()` is the single source of truth. The processor builds the APVTS from it, the UI binds widgets to it
by id, and presets store it by id. The ID scheme is documented in `Params.h`.

The table is append-only: never rename, reorder or remove an entry once released. A new phase appends at the end.

## Verification (definition of done for Core)

- `CoreTests` green.
- `PureSheenDev selftest` green: realtime allocation checks over every module and mode, state round trip, latency
  reporting, chain edits, undo/redo, presets.
- pluginval at strictness 10 with GUI on the release build: no failures.
- clap-validator: 0 failed.
- `PureSheenDev snapshot out/snap-core`: every panel at 1180x720 and 980x600, reviewed by eye.
- `PureSheenDev render` of test material: a null test at neutral settings, and a loudness / true-peak check of the
  Maximizer.
- Listening test by the user in FL Studio.
