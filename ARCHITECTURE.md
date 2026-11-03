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
- Preset importer (done, see "Preset importer" below): .xml presets of the reference suite, labelled generically
- Phase 3: the Assistant (done, see "Assistant (phase 3)" below), Track Referencing (done, from RefMatch, see
  "Track Referencing (phase 3)" below), analog-style
  compressor / EQ / limiter / tape modules, a de-limiter, and one plug-in per module ("PureSheen EQ", ...)
- Phase 4: MCP bridge

Never built: stem-separation EQ / focus, mix rebalancing, codec preview.

**Naming rule.** Never use another vendor's product, module or algorithm names in code, comments, UI text, test
names, file names or docs. The (private) research doc and the git-ignored `out/` notes may mention them. Foreign
strings the importer must recognise are matched by key, never written (see "Preset importer").

## Repository layout

```
CMakeLists.txt            targets: PureSheen (VST3, Standalone, + PureSheen_CLAP), CoreTests, PureSheenDev; sources
                          globbed per folder (a new .cpp is picked up on the next configure)
scripts/build.bat         scripts\build.bat <buildDir> <targets...>   (Ninja + MSVC 2022 x64 Release)
scripts/fetch_clap.bat, clap-juce-extensions-state-rescan.patch, install-plugin.ps1, build-installer.bat
                          (adapted from RefMatch at the end of Core)
src/core/                 measurement core copied from RefMatch (pure C++17): Common.h (dB helpers),
                          Loudness (BS.1770 M/S/I/LRA, realtime safe), TruePeak (4x), Fft (radix-2 forward)
                          (phase 3) TrackAnalysis: RefMatch's whole-track analyser (profile, 1/3-octave curve, bands);
                          RefCompare (master vs reference comparison + hints), Resampler (reference files)
src/assist/               (phase 3) the Assistant (pure C++17): AssistFeatures (analysis), AssistEngine + AssistProfiles
                          (suggestions, macros, targets, detector), OfflineChain (loudness by offline render)
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
src/plugin/Assistant.*    (phase 3) the Assistant on the processor side: capture, worker, state child, macro edits
src/plugin/TrackReferencing.*  (phase 3) Track Referencing: reference list, loader + analysis threads, A/B playback
src/plugin/import/        preset importer: ImportCommon / ImportEngine (pure C++, also in CoreTests), mapping tables
                          ImportMapEq / ImportMapCore / ImportMapPhase2, XmlPresetImport (JUCE side, file chooser)
src/ui/                   Theme (from RefMatch), Widgets (+ Spectrum, Options, ModulePanel, ModulePanels),
                          PluginEditor, HeaderBar, ChainStrip, IoPanel, DitherPanel, HelpOverlay,
                          EqPanel, DynamicsPanel, ImagerPanel, ExciterPanel, MaximizerPanel, MultibandDisplay
                          (phase 2) DynamicEqPanel (+ DynamicEqDisplay, DynamicEqModel), MatchEqPanel (+ MatchDisplay,
                          MatchEqModel), ToneBalancePanel / DefinitionPanel / SmootherPanel (+ SpectralDisplay,
                          SpectralWidgets, SpectralCommon), PunchPanel (+ PunchViews), LowContrastPanel (+
                          LowRegionDisplay), BassShaperPanel (+ BassScope), LowEndWidgets
                          (phase 3) AssistantPanel (the Assistant page), the header's Assistant | Chain | Reference switch;
                          ReferencePanel, RefWaveform, RefComparison (Track Referencing)
tests/                    CoreTests (TestFramework.h, namespace pstest; TestSignals.h generators);
                          ParamCompatTests.cpp freezes the 0.1.1 parameter table (count + fingerprint)
tools/dev/                PureSheenDev: selftest (processor-level + realtime allocation checks via
                          AllocationTracking), snapshot <dir> (editor PNGs), render <in.wav> <out.wav> (--preset,
                          --module-preset, --chain, --import, id=value), memory <rate> [ts] [editor] (private bytes),
                          import / importkey / importtest (ImportTool.cpp, see "Preset importer"),
                          assist (AssistTool.cpp: features, suggestions, renders of a card)
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
float rounding, < -135 dBFS; bit-exact where the mask is 0). One broadband gain mask per channel (the channels are
split independently), measured on the reference: per-band detection (complementary 4-band split at 120 / 800 / 5000
Hz) of fast vs slow band power, a per-band transient state that an onset sets and that decays over ~120 ms, combined
as a power mean weighted by the bands' current power. Controls: `<prefix>.ts.sens` (0..100 %, default 50) and
`<prefix>.ts.len` (5..100 ms, default 20), our own addition (the T/S popover behind the gear in the channel tabs);
the defaults are the measured split. L = 0.206 ms (9 samples at 44.1 kHz, 10 at 48 kHz).

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
- Not done: listening tests in FL Studio (the preset importer is done, see below).

## Preset importer

"Import preset (.xml)..." (header preset list, header "..." menu, each module's preset menu) reads the .xml presets of
the reference suite, so users can bring their presets over. Files: `src/plugin/import/` (see Repository layout), the
hooks `PureSheenProcessor::applyImportedPreset`, `MatchEqState::beginImportedReference`, the HeaderBar / ModulePanel
menus, and the dev tool's `import` / `importkey` / `importtest` and `render --import`.

**Design.**
- Pipeline: file (<= 16 MB, valid UTF-8) -> `prescanXmlText` (one root, matched tags, no DOCTYPE / ENTITY, no NUL
  bytes, nesting <= 16, <= 200k elements, <= 64 attributes per element) -> `juce::XmlDocument` -> neutral `XmlNode`
  tree -> `buildImportPlan` (pure C++) -> `applyImportPlan` (JUCE side, message thread only). A missing, empty or
  damaged chain blob refuses a whole-chain file (stale sections would otherwise leak in).
- Recognition is structural, never by root tag: a root with `PresetVer` / `PluginVer` attributes whose children are
  module sections holding `<Param ElementID ParamID Value>` and `<ExtraBytes ElementID Data>`. Exactly one enabled
  module section = a single-module file; otherwise a whole chain in the order of the decoded chain blob (sections not
  in the chain hold stale settings and are never read). Modules we do not have are skipped and named in our words
  (analog-style compressor / EQ / limiter, tape emulation, de-limiter, stem-separation EQ, mix rebalancing). A
  well-formed empty chain is refused as holding no modules (`ImportPlan::holdsNoModules`).
- Module power (whole-chain files) comes from the section's own power switch (`kSectionBypassKey`, any module type,
  >= 0.5 = off, absent = on; counted as an imported setting), handled once in `ModuleImport::run`. The chain entries'
  flag byte is parsed but is not power (a test save proved it stays 0 for a switched-off module). Single-module files
  ignore the switch (our module presets never carry power).
- One table per module (`ImportMap*.cpp`): a row per foreign control with an action (Map / ChannelMode / Hook / Ignore
  / Unsupported), numbering, band layout, our set A / set B ids, a transform, the foreign default and a label in our
  words; optional prepare / finish hooks for cross-parameter rules (multiband layouts with gaps closed, version
  renames, unit conversions). "Aux: " ParamIDs are channel set B. Every value is clamped / snapped into our range.
- Apply = ONE undo step. Whole chain: every preset-scope value and the chain (I/O gain, mono, swap and dither reset to
  our defaults, gain match untouched); the preset is named after the file and shows as modified. Module file: that
  module's values; a module not in the chain is inserted at our usual position and switched on. A module menu accepts
  only that module's files (EQ 1 / EQ 2 interchangeable). A Match EQ reference spectrum stored in the file (zlib +
  JSON records) is decoded on the JUCE side, mapped onto our 540-point grid (`setMatchSpectrumFromBins`) and applied
  to both parts in the same step (source "import:<name>"). Our Match EQ matches against a captured Current spectrum
  (the foreign one analyses live), so the note asks the user to capture the track as Current.
- `applyImportedPreset` validates the whole request (scope, values in range, order, reference) before touching any
  state, takes the edit lock for the whole transaction, records exactly one step (also for an unchanged import), and
  a host save re-entered from a parameter notification during the import gets the complete previous state.
- `XmlImportChooser` (one per menu owner): one dialog at a time, its one-shot completion is dropped when the owner
  is destroyed (the dialog is closed with it); the last folder is shared by live choosers.
- The result line goes to the notice pill (two lines when long) or the panel hint: `"<name>": Imported 4 modules,
  46 settings (1 not imported). Skipped: tape emulation.` Approximation notes stay in the report (dev tool).

**Hash rule.** Foreign tags, ElementIDs, ParamIDs and record member names are never written in the repo. They are
matched by a 64-bit FNV-1a key (`ps::imp::importKey`, constexpr) of a TEMPLATE: "Aux: " removed, the first run of
digits replaced by '#' (kept as the number). The source holds the numeric constant plus a comment in our words; keys
are computed at dev time with `PureSheenDev importkey "<string>"`. Tests use small synthetic trees whose generic names
map to the keys through `ImportOptions::aliases`. No corpus file or excerpt is ever copied into the repo; corpus-wide
checks run only through `PureSheenDev import <local folders>`.

**Defaults and confidence.** Files are sparse (only values that differ from the WRITER version's own default are
stored), so an absent value takes the foreign default of that writer version (`PluginVer` eras), never ours. Each
row's default is `known` (proven from sparse storage, documented, or confirmed in the real product), `likely`,
`guess`, `byVersion (fn, conf)` or `noDefault()`; that table column is the one place to correct them. The defaults
of writer 120000 were confirmed from Reset screenshots and one-change test saves of the real product (and the enum
lists: EQ shapes 0..16, Dynamics detection, limiter algorithms and styles, band phase scale, knee units); a confirmed
value that no file of any writer stores is used for every writer, an older writer the files prove different keeps its
own. `PureSheenDev import --defaults` lists the 40 still uncertain (16 likely, 24 guess: EQ band 1 / 7 / 8 shape and
Q, band frequency layouts, multiband crossovers, Dynamic EQ timing, Tone Balance cut mode, a few never-shown
controls). `PureSheenDev importtest` also compares the user's local check saves with the confirmed results when that
git-ignored folder exists (skipped otherwise; the files are never copied).

**Limits.**
- Not imported (counted as such): stem-separation mode, the per-module auto gain of Tone Balance / Definition, the
  Dynamics linear-phase crossover steepness. Controls the files never carry keep our defaults (Dynamic EQ ratio /
  knee / sidechain, EQ cut slopes at 12 dB/oct, Punch stereo link / crossover type, T/S split controls).
- Approximations (with a note in the report): Dynamic EQ automatic timing multipliers on our base times, the older
  Smoother threshold -> amount, surgical shelves -> analog shelves, Punch sync notes beyond 1/2 dotted, Dynamics set B
  output gain / module mix folded into the band gains / mixes.
- Uncertain meanings: the chain flag byte (parsed, not power), limiter algorithm index 4 before writer 120000 (read as
  low latency), the Bass Shaper thresholds (1:1 dB), the Exciter drive calibration. The Dynamics knee is stored in
  0..10 display units and read 1:1 as dB (our choice).
- Dither and I/O are never stored in the files.

Verification (build-impint, all five targets, 0 warnings in our sources), after an independent review-and-fix (two
passes; fixes: stricter XML / JSON / base64 / zlib checks, enum and overflow guards in the tables, refusing a damaged
chain, transaction validation and save consistency, chooser lifetime, chain-flag power, sparse Exciter / Tone Balance
era defaults, set-B Match EQ references, limiter activation): CoreTests 512 test cases, 0 failed (75 "import."
cases); `PureSheenDev selftest` 236 passed, 0 failed; `PureSheenDev importtest` all passed (81 checks: text path,
fuzz, hostile spectrum records, transactions, one undo step, Match EQ reference); corpus run over 706 local files:
595 imported, 111 not importable (only modules we do not have), 0 failed, 595 / 595 play finite audio, 25 / 25
stored Match EQ references imported, 15 values not imported (all intended, see Limits); renders of 10 varied
imported global presets:
no NaN, peaks within the imported Maximizer ceiling; snapshots of the menus and result notes at 980x600 / 1180x720;
pluginval strictness 10 with GUI (VST3): SUCCESS.

Real-product confirmation round (build-impfix, CoreTests / PureSheenDev / VST3, 0 warnings in our sources): section
power switch, confirmed enum tables and writer-120000 defaults. CoreTests 517 test cases, 0 failed (80 "import."
cases); selftest 236 passed, 0 failed; importtest all passed (incl. 54 local check saves compared); corpus over the
same 706 files: identical per-file counts (the corpus never stores the power switch), 48353 parameters now set from
foreign defaults (was 47338).

## Assistant (phase 3)

One feature for both "build me a master" and quick mastering with a few macros. The header's sparkle switch
(Assistant | Chain) shows the Assistant page in the module-panel area (EditorContext selection kAssistantSelection;
kChainSelection returns to the module selected before); the chain strip and the I/O panel stay visible.

**Flow.**
- Start page: Quick (listen now, detected target) or Custom: target list (Auto, a reference track, the 36 tone
  targets), the modules the Assistant may use, intensity (Gentle / Balanced / Strong), Maximizer on / off, loudness
  (Auto = the target's), output level, true peak, analysis time (5-30 s, default 8).
- Listen: `PureSheenProcessor::processBlock` hands every input block to `psplug::Assistant::pushAudio` (before the
  chain: the Assistant hears the unprocessed mix). While armed, blocks above -70 dBFS are copied into a buffer the
  message thread allocated (atomic pointer + busy flag, no locks, no allocation; the selftest checks it). "Analyse a
  file" takes the loudest part of a file instead.
- Analyse (worker thread): `ps::assist::analyse` (TrackProfile + a 1/6-octave mid / side STFT grid + the four
  processing bands 120 / 2k / 10k), `buildSuggestions`, the default card's Maximizer gain (`refineLoudness`), which is
  applied at once as ONE undo step ("Assistant: Suggested (Rock)", the importer's transaction path), then the other
  cards' gains and every card's loudness-match tables. Results reach the message thread through the processor's 30 Hz
  housekeeping (`Assistant::tick`).
- Results: the cards (Suggested = the reference-like default, or Match reference when a reference is loaded; Gentle,
  Loud, Warm, Bright & Wide, Punchy), the target list (click = rebuild from the kept audio and apply the new default),
  Tone (Tone Match, Tilt, the target band with the track's own balance), Loudness, the macros, Relearn, Bypass (held:
  global bypass with classic gain match), Open chain.

**Default card** (fitted black-box to the reference assistant's choices on seventeen test tracks, features from the
first 8 s it listens to; method and numbers at
the top of AssistEngine.cpp / AssistProfiles.cpp): chain EQ 1 > Punch > Imager > Definition > Tone Balance > Dynamic
EQ > Maximizer; the tone target from an example-based detector; an EQ leaning toward the target (+-1.8 dB, shelves +
up to four broad bells); Punch / Imager band settings from the per-band crest / width (both split at 120 / 2k / 10k);
a Dynamic EQ on the low-mid and presence resonances; Definition and Tone Balance at 25 %; the target's Maximizer
settings and loudness, the gain measured by an offline render of the captured audio (`OfflineChain`: only the
card's modules, built with `ps::createModule`, Stereo, set A). Intensity scales the amounts (0.6 / 1 / 1.5).

**Macros** map absolutely onto module controls and are read back from the parameters (so undo, snapshots and hand
edits keep them right): Tone Match = EQ 1 Amount, Tilt = two Baxandall shelves on EQ 1 bands 6 / 7, Dynamics Match =
Punch Amount, Width Match = Imager Amount, band widths = offsets on the card's Imager widths, Compress = a one-band
glue Dynamics inserted before Tone Balance on first use, the Definition / Tone Balance amounts, Loudness = the
Maximizer target (gain along the measured slope). A drag writes only that macro's parameters (unrecorded while
dragging) and is one undo step on release. Loudness matched (default on): the Maximizer gain also moves by minus the
pre-Maximizer loudness change, interpolated from per-macro tables measured at five positions.

**Reference hook.** `ps::assist::ReferenceTarget` (tonal balance relative to pink, width per processing band,
integrated loudness, PLR, name, path) is all the engine needs from a reference. It comes from Track Referencing's list
(one analysis per file, see "Track Referencing (phase 3)").

**State.** Child "Assistant" (version 1) of the plug-in state: settings, loudness matching, the reference target, the
cards (values stored sparse, against our defaults), the applied card, the detected target, the track's balance curve.
Built on the message thread and cached, so a host save from another thread copies it safely; a restore from another
thread is queued for the next tick (saves meanwhile return the restored tree). Jobs and reference loads carry a
generation: results of a superseded one are dropped. Not part of undo,
snapshots or presets; a state without it (older versions) resets the Assistant. The captured audio is not saved, so
after reopening a project new targets need a new listen. No automatable parameters were added (the 841-entry table and
its fingerprint are unchanged).

**Verification** (build-assist): CoreTests "ta." (the analyser's RefMatch checks) and "assist." (features, engine,
macros, offline loudness); selftest "-- Assistant" (allocation-free capture, the default card as one step, macro drags
as one step, undo restoring macro positions, loudness matching on / off, the Compress insert, the state round trip, an
old state); snapshots `15_assistant_*` (start, custom, listening, results at 980x600 / 1180x720 / 1800x1100, a macro,
back to the chain), including a real listen of the snapshot song. The measurement against the reference on the
seventeen test tracks is kept in private notes: the same chain and target on all seventeen; our cards reach the target
within 0.1 LU on what they heard, the reference's own renders land within 1.2 LU of its target, so whole-track loudness
differs by up to 1.3 LU; band balance 1.46 dB rms, of which our modules give 1.11 dB with the reference's own settings
(mostly the top octaves on dark and ambient material).

Known limits: the reference's target choice does not follow the genre closely and none of our feature sets predicts
it from the other examples better than about one in three (the detector reproduces the seventeen known cases; more
renders improve it; the target list overrides it); the Punch band amounts are
the weakest rule; Dynamic EQ frequencies come from resonance peaks, not the reference's fixed grid; Tilt shares EQ 1's
Amount (Tone Match scales it too); every card's loudness tables take a few seconds each on a worker thread (the cards
and macros work meanwhile, unmatched until their tables arrive).

## Track Referencing (phase 3)

Comparing the master with released songs: up to 10 references, five loops (A-E) each, level-matched A/B switching and
a plain-language comparison. Ported from RefMatch (the family's reference-track matcher): its analyser
(`src/core/TrackAnalysis`), comparison and hints (`src/core/RefCompare`, the advice rewritten for PureSheen's modules),
resampler (`src/core/Resampler`) and its processor's playback engine and threads (`src/plugin/TrackReferencing`).

**Files.**
- `src/core/RefCompare.h/.cpp`: `referenceMatchGainDb`, `Comparison` (band differences at equal loudness, severities
  with hysteresis, curve difference), ranked `Hint`s (setup, true peak, low-end mono, loudness, PLR, tone per band,
  width). CoreTests `ref.`.
- `src/core/Resampler.h/.cpp`: Kaiser-windowed sinc (48 zero crossings, beta 9.5, cut-off at 95 % of the lower
  Nyquist), exact polyphase for small rational ratios, 4096 phases otherwise. CoreTests `ref. resampler`.
- `src/plugin/TrackReferencing.h/.cpp`: `psplug::TrackReferencing`, owned by the processor
  (`PureSheenProcessor::getTrackReferencing()`); the header is the contract for the UI.
- `tools/dev/RefTool.cpp`: `PureSheenDev reference <master> <ref> [--chain default] [--loop s e] [--listen out.wav]`.
- UI: `src/ui/ReferencePanel.*` (the page), `src/ui/RefWaveform.*` (waveform strip with loops A-E),
  `src/ui/RefComparison.*` (tonal balance, loudness & dynamics, stereo, what to try), plus the header, editor, Options
  and spectrum-display hooks below.

**Signal path.** `processBlock` runs the chain, then `TrackReferencing::process` on the final output: the master is
copied to the analysis FIFO, the selected reference is rendered (unity gain), Swap and Mono of the I/O panel are
applied to it (ramped over 20 ms), it is measured as heard, and it replaces the master through an equal-power A/B fade
(30 ms) with the level-match gain (250 ms multiplicative glide; Off is 0 dB at once). Settled on the master the buffer is
untouched (bit-exact); with no playable reference Listen is ignored; offline renders always pass the master. The I/O
meters keep measuring the master. No parameters were added (the 841-entry table and its fingerprint are unchanged).

**Playback** (RefMatch's engine): 8 read heads; every discontinuity (host jump, loop wrap, seek, loop or reference
change, start / stop) starts a new head that fades in over 8 ms while the others fade out. Sync on: while the host
plays, position = region start + (host position + seek offset) mod region length, host wobble within 5 ms ignored.
Sync off, or the host stopped with Play when stopped on: the reference runs on its own; Play when stopped off: silent.
A region that ends at the end of the file wraps 8 ms early into the lead-in. The track and region reach the audio
thread through a try-locked slot (32 attempts; a failure silences that chunk and counts a dropout, expected 0); replaced
tracks are parked for their fade-out and freed on another thread.

**List and loading.** Each reference is decoded (JUCE's basic formats: WAV, AIFF, FLAC, MP3, Ogg Vorbis; 0.5 s - 20 min;
non-finite samples become silence, more than 1 % is an error), resampled to the host rate and analysed ONCE on the
loader thread: `ps::assist::analyse` gives the whole-file `TrackProfile` (the comparison) and the Assistant's
`ReferenceTarget`. Loop regions get their own `TrackProfile` (`TrackAnalyser`, Since reset), cached per letter until the
loop changes; while one is measured the previous comparison stays. Loads run one at a time: the selected reference
first, then a pending region analysis, then the other loads in order. Errors: "File not found" (state Missing, kept
with its loops until relinked), "Unsupported or damaged audio file", too short / long, out of memory (state Error).
A host-rate change drops the decoded audio at the old rate; the selected reference decodes again (the analysis is kept).

**Memory.** Decoded audio (stereo float at the host rate, about 11 MB per minute at 48 kHz) stays for the selected
reference and, least recently selected out first, for the others within 768 MB (`setMemoryBudget`). An evicted
reference keeps its analysis, overview, loudness curve and loops, and is decoded again when selected (the master plays
meanwhile).

**Analysis thread** (every 15 ms, low priority; fed only while the list is not empty): a `TrackAnalyser` on the master
(Recent 30 s or Since reset), the `Comparison` against the selected reference's active region (band hysteresis held
while the same region and averaging stay), and the level-match target: Integrated = master integrated vs region
integrated; Short-term = the two 3 s windows as heard, used while both hold 3.2 s of continuous non-silent audio, else
the last one is held (the Integrated match before the first).

**The Assistant.** One list, one analysis: the Assistant has no file loader of its own any more. Its "+ Reference
track..." adds the file to the list and calls `Assistant::awaitReference`; the processor (`connectReferencing`) calls
`setReference` with the entry's `ReferenceTarget` when its analysis is done (`referenceFailed` when it is not), and
`updateReference` (numbers only, no rebuild) when the Assistant's current reference file is analysed again (a project
reopened). `ReferenceTarget::path` links the two; removing that reference from the list returns the Assistant to Auto.
Selecting a reference to listen to never changes the chain.

**State.** Child `<TrackReferencing version="1" levelMatch sync playWhenStopped averaging selected="index">` with one
`<Ref path activeLoop loopA="start,end" ... loopE/>` per reference (seconds). The audio is not saved; restoring reloads
the files (entries for the same files are kept as they are, so a host restoring the state it just saved reloads
nothing). Listen always restores to the master. A host-thread save copies a tree cached on the message thread; a
host-thread restore is queued for the next tick (saves meanwhile return it). Not part of undo, snapshots or presets.

**Verification** (build-ref): CoreTests `ref.` (23: the comparison and hints ported from RefMatch with the new texts,
the resampler); selftest "-- Track Referencing" (bit-exact master with and without a reference, sample-exact reference
at the host position, loops, seek, Sync off, Play when stopped, no clicks on loop wraps / switches / seeks / A/B fades,
Integrated and Short-term level match within 0.2 LU of the master, Mono / Swap, offline renders, allocation-free
playback while loads / switches / loop edits run, resampling, the 10-reference limit, eviction and re-decode, the state
round trip, a missing file and relink, a host-thread restore, the Assistant's target from the shared analysis, a
sample-rate change); `PureSheenDev reference` on real music: FLAC, Ogg, MP3 and WAV references load in 1-3 s for 2-7
minute songs, integrated loudness and LRA within 0.1 LU of ffmpeg's ebur128.

**UI.**
- Header: the view switch has a third segment (Assistant | Chain | Reference) that opens the Reference page in the
  module-panel area (`kReferenceSelection`; the chain strip and the I/O panel stay). The REF pill toggles Listen from
  anywhere; it fills lavender with the A/B fade and is dimmed while no reference is ready.
- Page: the reference list (up to 10; loading progress, error, "Missing - click to find it" = relink; remove on hover;
  + Add takes several files), the waveform strip (overview, short-term loudness, playhead, ruler, click to seek; loops
  A-E as coloured regions: click an empty letter to arm it and drag on the wave, click a set letter to play it and
  again for the whole track, drag its edges, right-click a letter to clear it; a drag with nothing armed draws into
  the first empty letter), the MASTER | REFERENCE switch with the match gain, the Match / Window chips, Sync, Play when
  stopped, Reset. The comparison is a 2 x 2 grid when there is room (1800 x 1100), tonal balance next to tabs at the
  default size, four tabs at 980 x 600 (a dot marks a tab with something worth a look).
- Files dropped anywhere on the editor are added (an overlay while dragging; a notice for a full list or an
  unsupported file) and open the page.
- Reference spectrum overlay (Options: "Show the reference in the analysers", on by default): the selected reference's
  active-region curve in lavender on the EQ, Dynamic EQ, Match EQ, multiband and spectral displays, behind the curves.
  It is shown at the master's loudness (reference curve - normalisationOffsetDb, which is reference minus master) with
  the analyser's tilt, and shifted onto the output analyser by the mean difference (100 Hz - 10 kHz) between the
  analyser and the master's own curve (smoothed per tick, held while the output is silent).
- The Assistant's target list shows one "Ref: name" row per ready reference; its x clears the Assistant's target only.
- Snapshots `16_reference_*` (`PURESHEEN_SNAPSHOT_ONLY=reference`): empty, loading, loaded at three sizes and per tab,
  listening, missing, drop overlays, the Assistant's target list, the overlay on EQ 1 (synthetic WAVs only).

**Known limits.** Integrated level match uses the whole region's loudness, so a quiet intro of the reference plays
quieter than the master (Short-term follows what is heard). The reference plays without the chain's latency offset
(a different song; a few ms do not matter).
