# PureSheen Dynamics: architecture & team contract

PureSheen Dynamics is a full mastering suite (VST3 + Standalone + CLAP, JUCE 8, C++17, Windows / FL Studio first) whose
feature set matches a top-tier commercial mastering suite module for module and control for control. It has its own
name, its own algorithms and its own UI, in the visual family of Chordbloom, RefMatch and Duckling (dark slate, Inter, mint
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
- Phase 2 (done, see "Phase 2" and "Status (phase 2 integration)" below): Dynamic EQ, Match EQ, Tone Balance,
  Definition, Smoother, Punch, Low Contrast, Bass Shaper, and the Transient/Sustain channel mode
- Next: the preset importer (.xml presets of the reference suite, labelled generically in the UI)
- Phase 3: Vintage modules, Unlimiter, rule-based Assistant, Track Referencing (from RefMatch), and one plug-in per
  module ("PureSheen EQ", "PureSheen Maximizer", ...)
- Phase 4: MCP bridge

Never built: stem-separation EQ / focus, mix rebalancing, codec preview.

**Naming rule.** Never use another vendor's product, module or algorithm names in code, comments, UI text or docs.
The (private) research doc may mention them.

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
                          gain, gain match, mono / swap, bypass, dither, meters, FIFOs, latency; T/S twins
  (phase 2)
  TransientSplit.h/.cpp   ps::TransientSplitter: the Transient/Sustain split of the chain (real)
  SpectralEngine.h/.cpp   residual STFT engine shared by Tone Balance, Definition, Smoother (real)
  Quadrature.h/.cpp       ripple-free envelope for Punch / Bass Shaper
  DynamicEq, MatchEq (+ MatchAnalysis, CurveFir), ToneBalance (+ ToneTargets), Definition, Smoother, Punch,
  LowContrast, BassShaper .h/.cpp   one contract header + implementation per module
src/plugin/               PluginProcessor.h/.cpp (+ PluginState, UndoHistory, Presets, FactoryPresets, MatchEqState;
                          MatchEqStateTests.cpp is compiled into the dev tool only)
src/ui/                   Theme (from RefMatch), Widgets (+ Spectrum, Options, ModulePanel, ModulePanels),
                          PluginEditor, HeaderBar, ChainStrip, IoPanel, DitherPanel, HelpOverlay,
                          EqPanel, DynamicsPanel, ImagerPanel, ExciterPanel, MaximizerPanel, MultibandDisplay
                          (phase 2) DynamicEqPanel (+ DynamicEqDisplay, DynamicEqModel), MatchEqPanel (+ MatchDisplay,
                          MatchEqModel), ToneBalancePanel / DefinitionPanel / SmootherPanel (+ SpectralDisplay,
                          SpectralWidgets, SpectralCommon), PunchPanel (+ PunchViews), LowContrastPanel (+
                          LowRegionDisplay), BassShaperPanel (+ BassScope), LowEndWidgets
tests/                    CoreTests (TestFramework.h, namespace pstest; TestSignals.h generators);
                          ParamCompatTests.cpp freezes the 0.1.1 parameter table (count + fingerprint)
tools/dev/                PureSheenDev: selftest (processor-level + realtime allocation checks via
                          AllocationTracking), snapshot <dir> (editor PNGs), render <in.wav> <out.wav> (--preset,
                          --module-preset, --chain, id=value), memory <rate> [ts] [editor] (private bytes)
tools/audio/              make_test_mix.py, check_clicks.py, render_presets.sh (Core), render_phase2.py (phase 2)
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
10. **Feature parity.** Where the research documents a behaviour, match it. Where it does not, choose what a mastering
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
- **Worker threads:** the EQ's FIR designer, one per EQ, owned by the module. Phase 2: the Dynamic EQ (Digital
  kernels) and Match EQ (kernels + capture analysis) own one each; the processor owns the Match EQ file loader thread.
  T/S twins are full instances, so they own their own workers.

## Parameters

`ps::paramSpecs()` is the single source of truth. The processor builds the APVTS from it, the UI binds widgets to it
by id, and presets store it by id. The ID scheme is documented in `Params.h`.

The table is append-only: never rename, reorder or remove an entry once released. A new phase appends at the end.
- 0.1.1 = the first 479 entries (`ps::kNumParams_0_1_1`, last "dith.dc"); `tests/ParamCompatTests.cpp` checks their
  count and an FNV fingerprint of every field (id, name, kind, range, default, skew, step, choices, unit, module).
  The processor gives them APVTS version hint 1, the phase 2 appendix version hint 2.
- Phase 2 appendix (after "dith.dc"): the Core T/S additions in ModuleType order (eq1 / eq2 `.ts.sens` / `.ts.len`,
  the Imager's channel slot `img.chMode` / `.a.on` / `.b.on` / `.link` / `.ts.*` and its set B `img.b.b<n>.width`,
  `img.b.stz.*`, `img.b.rec.*`, then `exc.ts.on`, `exc.ts.*`), then every phase 2 module in ModuleType order (slot
  params, `.ts.*` when it offers T/S, then its own params in struct order). ParamTable.cpp builds the Core part with
  the channel-mode lists as released (`releasedChannelModes`) and the appendix with the final ones.
- The Exciter's released `exc.chMode` choice list { Stereo, Mid/Side } is kept (so 0.1.x host automation keeps its
  meaning); its T/S mode is the appended switch `exc.ts.on` (on = T/S, overrides exc.chMode).
  `ps::channelModeChoices` / `ps::channelModeParamValues` hide this from the UI and the importer.
- Saved states and presets store values by id and the chain by id prefix, so 0.1.x states load unchanged (every
  phase 2 parameter at its neutral default; selftest "state: a 0.1.x state ...").

## Transient/Sustain channel mode (phase 2, implemented in the chain host)

Per module, like M/S: the slot splits its own input. `ps::TransientSplitter` (TransientSplit.h) gives a transient
part T and a sustain part S, both stereo, with T + S == input delayed by L (S is computed as x_L - T: exact up to one
float rounding, < -135 dBFS; bit-exact where the mask is 0). The mask is per band (complementary 4-band split at 120 /
800 / 5000 Hz, no all-pass phase), stereo-linked (the image of T is kept), from the ratio of a fast and a slow
envelope with a baseline tracker. Controls: `<prefix>.ts.sens` (0..100 %, default 50) and `<prefix>.ts.len`
(5..100 ms, default 20), our own addition (the T/S popover behind the gear in the channel tabs). L = 64 samples per
48 kHz family (1.33..1.45 ms).

Every type that offers T/S has a second instance (the "twin", `Chain::getModule (type, 1)`, `Module::getParamSet()
== 1`): instance A processes T as Stereo with set A, the twin processes S as Stereo with set B; both get the same
parameter array every block. Rule for modules (Module.h): with paramSet 1 read set B for both channels (the Core EQ,
Exciter and Imager copy set B over set A in setParams); latency only from shared params; no shared mutable state.
- Slot latency in T/S = L + max (latency A, latency B) (the parts are padded to the common value).
- Channel power (a.on = Transient, b.on = Sustain), channel solo, delta, band solo / audition (selected-set band solo
  and audition go only to that instance, the other part is muted meanwhile), power and module solo work per part; every crossfade
  toward "dry" uses the latency-aligned dry parts Td (T delayed) and Sd = dry - Td, so it reconstructs the input.
- A channel-mode change that changes the slot latency also dips the slot output, global bypass and delta bus
  (20 ms out, switch at silence, 20 ms in): no click from their alignment-delay jumps. Match EQ phase / FIR size,
  Dynamic EQ Digital / lookahead, and Punch crossover latency changes use the same dip before committing params;
  `Module::getLatencySamplesForParams` predicts the shared-parameter latency without changing DSP state. The module
  resets at silence and warms its new history with aligned dry output before the wet signal fades back in.
- Meters: `IoMeters::moduleTsActivity` (mask activity, drives the glow of the Transient tab) and `moduleTsShareDb`
  (transient energy share). `Chain::setTapPart` lets the module taps carry one part ("show the selected part").
- Channel modes per module: EQ 1 / 2, Dynamic EQ, Match EQ: Stereo, M/S, L/R, T/S. Exciter, Tone Balance,
  Definition, Smoother, Low Contrast: Stereo, M/S, T/S. Dynamics, Punch: Stereo, M/S. Imager: Stereo, T/S.
  Maximizer, Bass Shaper: Stereo.
- CPU: a slot in T/S runs its module twice (plus < 0.2 % for the splitter). Memory: every module type and every twin
  is prepared with the chain, in the chain or not (the Digital EQ preallocates its FIR sizes), so the chain contents
  and channel modes barely change it. Measured with `PureSheenDev memory` (see "Status (phase 2 integration)"): under
  the 300 MB budget at 48 kHz, so the twins stay eagerly prepared; lazy preparation is the fallback for 192 kHz.

## Phase 2: modules, contracts and ownership

| ModuleType | prefix | display name (short) | channel modes | params | DSP owner | panel owner |
|---|---|---|---|---|---|---|
| DynamicEq | deq | Dynamic EQ (Dyn EQ) | St, M/S, L/R, T/S | 190 | dsp-deq | ui-deq |
| MatchEq | meq | Match EQ | St, M/S, L/R, T/S | 21 | dsp-meq | ui-meq |
| ToneBalance | tbal | Tone Balance | St, M/S, T/S | 27 | dsp-tone | ui-spectral |
| Definition | defn | Definition | St, M/S, T/S | 19 | dsp-defn | ui-spectral |
| Smoother | smth | Smoother | St, M/S, T/S | 21 | dsp-smth | ui-spectral |
| Punch | pnch | Punch | St, M/S | 33 | dsp-punch | ui-punch |
| LowContrast | lowc | Low Contrast | St, M/S, T/S | 16 | dsp-lowc | ui-lowend |
| BassShaper | bass | Bass Shaper | St | 13 | dsp-bass | ui-lowend |

(params = slot params incl. `.ts.*` + module params; plus 22 Core T/S additions; 841 entries in all.)

Contracts written in the contracts wave (headers are contracts, rule 1):
- `Types.h` (ModuleTypes, DynEqShape / Direction / Sidechain, MatchPhase, ToneBalanceMode, ToneTarget, SmootherMode,
  LowContrastMode, kPunchSyncFractions), `Params.h` (typed structs + read*Params, SlotParams ts fields, ImagerParams
  setB, channel-mode helpers), `Module.h` (ProcessContext::paramSet, ModuleMonitor::soloRegion / learnRegion,
  Module::setParamSet / getParamSet), `Chain.h` (twins, setTapPart, T/S meters).
- One header per module (behaviour precise enough to implement and test, from the private phase 2 specs) with a
  passthrough stub .cpp, plus `MatchAnalysis.h`, `SpectralEngine.h` (real), `Quadrature.h` (fallback),
  `TransientSplit.h` (real).
- `PluginProcessor.h`: Match EQ reference-file loading API (stub) and the "MatchEq" state child contract.
- `ui/Widgets.h`: currentChannelMode / isDualChannelMode / channelSetName / channelModeShortLabel /
  moduleForChannelSet; the header's channel-mode segments offer T/S, the tabs read "Transient | Sustain" and carry
  the T/S options gear.
- Each panel file `src/ui/<Module>Panel.h/.cpp` started as a GenericModulePanel stub; wave 2 replaced every one with a
  dedicated panel deriving from ModulePanel.

Ownership for the next wave (only edit your files; ask the lead for contract changes):
- dsp-deq: DynamicEq.h/.cpp (+ new files), tests/DynamicEqTests.cpp ("deq.")
- dsp-meq: MatchEq.h/.cpp, MatchAnalysis.h/.cpp, CurveFir.h/.cpp (new), tests/MatchEqTests.cpp ("meq.")
- dsp-tone: ToneBalance.h/.cpp, ToneTargets.cpp (new), tests/ToneBalanceTests.cpp ("tbal.")
- dsp-defn: Definition.h/.cpp, tests/DefinitionTests.cpp ("defn.")
- dsp-smth: Smoother.h/.cpp, tests/SmootherTests.cpp ("smth.")
- dsp-punch: Punch.h/.cpp, Quadrature.h/.cpp, tests/PunchTests.cpp ("pnch."), tests/QuadratureTests.cpp ("quad.")
- dsp-lowc: LowContrast.h/.cpp, tests/LowContrastTests.cpp ("lowc.")
- dsp-bass: BassShaper.h/.cpp, tests/BassShaperTests.cpp ("bass.")
- ui-deq: DynamicEqPanel.h/.cpp (+ DynamicEqDisplay.h/.cpp); ui-meq: MatchEqPanel.h/.cpp (+ MatchDisplay);
  ui-spectral: ToneBalancePanel, DefinitionPanel, SmootherPanel (+ SpectralDisplay.h/.cpp);
  ui-punch: PunchPanel; ui-lowend: LowContrastPanel, BassShaperPanel (+ LowRegionDisplay, BassScope)
- plugin-meq: the Match EQ parts of PluginProcessor.cpp (state child, snapshots / presets, undo, loader thread) in a
  new src/plugin/MatchEqState.cpp where possible
- ui-ts: factory presets with T/S (FactoryPresets.cpp); Imager set B editing and linked edits are implemented
- SpectralEngine.*, TransientSplit.*, Chain.*, ParamTable.cpp, ModulePanel.cpp, ChainStrip.cpp: lead (contracts);
  module owners report needed changes. Factory presets of the phase 2 modules: their DSP owners propose them, the
  lead merges them into FactoryPresets.cpp.

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

## Status (end of Core integration)

Done: every Core module is real (Equalizer 1/2 with Analog and linear-phase Digital modes, Dynamics, Imager, Exciter,
Maximizer with six modes, Dither), the chain host, the processor (undo history, A-D snapshots, presets, factory
presets, learn), the editor with a dedicated panel per module, and the dev tool. Verification on build-int:
- `CoreTests`: 199 test cases, 0 failed. `PureSheenDev selftest`: 177 checks, 0 failed (incl. every [dsp] check).
- pluginval strictness 10 with GUI tests: SUCCESS. clap-validator: 31 passed, 0 failed, 12 skipped (features we do not
  implement: preset discovery, 64-bit audio, note ports, port configs), 1 warning (`state-invalid-random`: JUCE's
  CLAP wrapper reports success for any state blob; the garbage is ignored and nothing crashes).
- `PureSheenDev snapshot`: every panel at 980x600 .. 1800x1100 plus mode variants (Maximizer Spectral, EQ Digital,
  Dynamics 4 bands M/S, Imager 4 bands + Stereoize, Exciter 4 bands M/S), reviewed by eye.
- Renders of a synthesized 30 s premaster (-20.2 LUFS, -4 dBFS peak) through the default chain (exact null) and every
  global factory preset: true peak within 0.02 dB under the Output Level, no NaN, no clicks found.
  Reproduce: `python tools/audio/make_test_mix.py test_audio/int_mix.wav`, then `sh tools/audio/render_presets.sh`.

## Status (phase 2 contracts wave)

Done: the contracts above; the Transient/Sustain mode for real (splitter, twins, latency, monitors, UI segments /
tabs / options popover) on EQ 1 / 2, Exciter and Imager (and the phase 2 modules once they honour paramSet); the
shared SpectralEngine; the eight modules as passthrough stubs registered everywhere (chain, Add menu, state,
presets, undo, strip with up to 14 tiles, colours, help, panels, selftest, snapshots). Verification on build-p2c:
CoreTests 224 test cases 0 failed; `PureSheenDev selftest` 208 checks 0 failed; snapshots of every new panel, the
  strip with all 14 modules at 980x600 / 1180x720 / 1800x1100 and EQ 1 / Exciter in T/S; pluginval strictness 5
(VST3): SUCCESS. Not done: the module DSP (stubs), dedicated phase 2 panels, phase 2
factory presets, the Match EQ state / file loader (stub API), the preset importer.

Phase 2 adversarial review: corrected monitor transport and mode routing, selected-part band solo and bypassed
part taps, latency-switch fades on global bypass / upstream delta, Imager set B controls, and T/S popover lifetime.
Clarified the Dynamic EQ timing, Bass Shaper peak-stage ordering and Smoother empty-neighbour contracts.
Verification: CoreTests 229 cases / 0 failed; selftest 213 passed / 0 failed; 0 compiler warnings in our sources.
An independent compiled comparison against the released HEAD table found all first 479 parameter entries unchanged.

Known limitations:
- EQ: no Notch / Band Pass / Tilt shapes yet (a narrow Bell at -30 dB acts as a notch). Digital mode designs on a
  worker thread, so faster-than-realtime offline renders can apply a changed curve a few blocks earlier or later
  than a realtime pass; after switching to Digital the EQ joins once the FIR has a full input history (~0.7 s at
  FIR 32768, 48 kHz), flat and exact until then.
- Crossover: changing the band count between 1 and N with an Analog / Hybrid crossover crossfades the bit-exact
  passthrough with the all-pass band sum for 20 ms; a tone near a crossover can dip by up to ~0.9 dB during that fade.
- Maximizer: a mode change mutes for ~20-45 ms (fade out, flush, refill at the new latency, fade in); Learn simulates
  the captured audio once on the message thread (0.1-0.3 s).
- Imager / Exciter: topology changes (band count, crossover type, oversampling) crossfade between two complete
  engines; the second engine roughly doubles their CPU cost (heaviest Exciter case still ~12x realtime).
- Dynamics: removing bands folds the removed band into the last remaining one at that band's gain.
- Installer: `scripts\build-installer.bat` -> `dist\PureSheen-Dynamics-Setup-<version>.exe` (Inno Setup 6, static
  runtime). Icon: `PureSheenDev icon resources/icon/logo_512.png 512`; `installer/puresheen.ico` holds 16-256 px renders.
- Not done: listening tests in FL Studio; the installer has been built but not run yet.

## Status (phase 2 integration)

Done: all eight phase 2 modules with real DSP and a dedicated panel each (no stubs left), the Transient/Sustain mode
on every module that offers it, the Match EQ state / undo / reference-file loader, phase 2 and T/S factory presets
(18 global presets; 2-6 per module, 4 per phase 2 module), and the integration (selftest coverage, snapshots, renders,
memory). Verification on build-p2int (all five targets, 0 warnings in our sources):
- `CoreTests`: 437 test cases, 0 failed. `PureSheenDev selftest`: 236 passed, 0 failed (after the review below; the
  Match EQ state checks inside it: 81 / 0 failed). New phase 2 checks: every
  phase 2 module alone in every channel mode it offers is a latency-aligned null at neutral settings with the latency
  reported to the host; a 14-module chain in T/S with random values round-trips through the state and plays finite
  audio at the same latency; every phase 2 module preset loads as one step, moves its module and plays finite audio
  with the module in the chain (besides the existing realtime groups per module / mode and T/S at 44.1 / 96 kHz).
- `PureSheenDev snapshot`: OK, 129 PNGs incl. every phase 2 panel at 980x600 .. 1800x1100, M/S and T/S tabs, the strip
  with 14 modules, Help; plus the panel agents' mouse checks (one undo step per drag, right-click edits nothing,
  solos cleared with their panel, no keyboard focus). Reviewed by eye.
- pluginval strictness 10 with GUI tests (VST3): SUCCESS. clap-validator: 31 passed, 0 failed, 12 skipped, 1 warning
  (`state-invalid-random`, see Core).
- Renders (`python tools/audio/render_phase2.py build-p2int test_audio/p2int_mix.wav`, 30 s, -20.2 LUFS): default
  chain exact null; every global preset no NaN, no clicks (worst 2.0 sigma), true peak <= Output Level + 0.02 dB;
  each phase 2 module alone with a factory preset in Stereo and T/S: no NaN, no clicks.
- Latency at defaults (44.1 / 48 kHz): Dynamic EQ 0 (Analog, no lookahead), Match EQ 9216 (Linear, FIR 16384), Tone
  Balance 2048, Definition 1024, Smoother 1024, Punch 0 (Analog crossover), Low Contrast 4096, Bass Shaper 115;
  + 64 per slot in T/S. All 14 modules at defaults: 18003 samples (375 ms at 48 kHz).
- Memory (`PureSheenDev memory`, process private bytes with every module and twin prepared): +142 MB at 44.1 / 48 kHz
  (+8 MB with the editor open), +265 MB at 96 kHz, +511 MB at 192 kHz; the same with T/S on or off and with the
  default chain (everything is prepared up front). Under the 300 MB budget at 48 kHz, so twin preparation stays eager.

Final adversarial review-and-fix of phase 2 (independent reviewer): all numbers above are from after its fixes. The released 479-parameter fingerprint remains unchanged. Regression coverage now includes coordinated latency
switches and long-chain alignment, selected-part region solos, capture/state/undo ordering, linked file completion
after panel closure, gesture routing across refreshes and tabs, prompt and solo teardown, and shared scope overflow.
The Match EQ capture test waits for analysis progress instead of assuming worker scheduling from fixed sleeps.

Decisions / known limitations (phase 2):
- Match EQ: the Low / High cutoff knobs appear only when the strip is >= 1120 px wide; at every editor size the cutoffs
  are edited on the display handles (drag, double-click, right-click "Enter value"). A loaded file is analysed without
  the T/S split; in T/S with Link the processor applies it to both parts as one undo step, even after the panel closes.
- Smoother: Learn lives in the panel's side column (the header Learn button is the multiband crossover learn). The
  factory presets act only where the material has resonances above their threshold (on the synthetic test mix
  "De-Harsh Master" is a bit-exact passthrough; at Amount 100 / Strong it works).
- Tone Balance: boost / cut caps are fixed by the DSP (+9 / -12 dB x Amount), shown as limit markers.
- Punch: the synced Envelope shows the note only (no host tempo in the UI); the band amount is in the overlay pill.
- Low-frequency spectra use the shared analysers (4096-point FFT by default); a larger FFT in Options resolves the
  Low Contrast / Bass Shaper range better.
- `SpectrumAnalyser::draw` ignores the alpha of the line colour; the spectral panels fade with transparency layers.
- pluginval's editor tests open the default module, so the phase 2 panels are exercised by the snapshot checks.
- Not done: listening tests in FL Studio, the preset importer.
