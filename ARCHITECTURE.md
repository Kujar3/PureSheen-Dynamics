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
  "Track Referencing (phase 3)" below), the vintage-style compressor / EQ / limiter / tape modules (done, fitted to the
  reference, see "Vintage-style modules (phase 3)" below), a de-limiter (done, see "De-limiter (phase 3)" below), and one plug-in per module
  (done, see "Single-module plugins" below)
- Phase 4: MCP bridge (see "Connection to Claude")

Never built: stem-separation EQ / focus, mix rebalancing, codec preview.

**Naming rule.** Never use another vendor's product, module or algorithm names in code, comments, UI text, test
names, file names or docs. The (private) research doc and the git-ignored `out/` notes may mention them. Foreign
strings the importer must recognise are matched by key, never written (see "Preset importer").

## Repository layout

```
CMakeLists.txt            targets: PureSheen (VST3, Standalone, + PureSheen_CLAP), PureSheen_<id> (VST3, CLAP; one per
                          single-module plugin, not in "all"; PureSheenSingles builds them), CoreTests; sources globbed
                          per folder (a new .cpp is picked up on the next configure)
cmake/common/             PureSheenCommon (everything the plugins share, compiled once) and PureSheenDev
installer/products.csv    the one list of plugins: id, name, plugin code, module, categories, installer id, description
scripts/build.bat         scripts\build.bat <buildDir> <targets...>   (Ninja + MSVC 2022 x64 Release)
scripts/fetch_clap.bat, clap-juce-extensions-state-rescan.patch, install-plugin.ps1, build-installer.bat
                          (adapted from RefMatch at the end of Core)
scripts/package-all.ps1   every plugin's installer and manual zip, from the static-runtime build
src/entry/PluginEntry.cpp createPluginFilter, compiled per plugin with its product id
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
  (phase 3) ClassicComp, PassiveEq (+ PassiveEqDesign.h), ClassicLimiter, Tape .h/.cpp   the vintage-style modules
src/plugin/               PluginProcessor.h/.cpp (+ PluginState, UndoHistory, Presets, FactoryPresets, MatchEqState;
                          MatchEqStateTests.cpp is compiled into the dev tool only)
src/plugin/Assistant.*    (phase 3) the Assistant on the processor side: capture, worker, state child, macro edits
src/plugin/TrackReferencing.*  (phase 3) Track Referencing: reference list, loader + analysis threads, A/B playback
src/plugin/import/        preset importer: ImportCommon / ImportEngine (pure C++, also in CoreTests), mapping tables
                          ImportMapEq / ImportMapCore / ImportMapPhase2 / ImportMapPhase3 (vintage-style family),
                          XmlPresetImport (JUCE side, file chooser)
src/ui/                   Theme (from RefMatch), Widgets (+ Spectrum, Options, ModulePanel, ModulePanels),
                          PluginEditor, HeaderBar, ChainStrip, IoPanel, DitherPanel, HelpOverlay,
                          EqPanel, DynamicsPanel, ImagerPanel, ExciterPanel, MaximizerPanel, MultibandDisplay
                          (phase 2) DynamicEqPanel (+ DynamicEqDisplay, DynamicEqModel), MatchEqPanel (+ MatchDisplay,
                          MatchEqModel), ToneBalancePanel / DefinitionPanel / SmootherPanel (+ SpectralDisplay,
                          SpectralWidgets, SpectralCommon), PunchPanel (+ PunchViews), LowContrastPanel (+
                          LowRegionDisplay), BassShaperPanel (+ BassScope), LowEndWidgets
                          (phase 3) AssistantPanel (the Assistant page), the header's Assistant | Chain | Reference switch;
                          ReferencePanel, RefWaveform, RefComparison (Track Referencing); ClassicCompPanel,
                          PassiveEqPanel, ClassicLimiterPanel (+ ClassicLimiterViews), TapePanel (+ TapeViews)
                          (vintage-style modules)
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

### Automation and MIDI control

A host lists every parameter of the plugin it loaded (925 in the suite, the module's and the I/O panel's in a
single-module plugin). FL Studio's "Browse parameters" shows that list, highlights the last control touched, and makes
a clip from it. Nothing about FL is stored in the plugin.
- Every control, custom-drawn ones included, edits through a parameter inside begin / end gestures (one undo step, and
  what tells the host which control was touched). A new control must do the same.
- A clip arrives as value changes without a gesture, at most once per block. The processor marks such a parameter
  `automated` (in `parameterValueChanged`, possibly on the audio thread, so atomics only). A marked control belongs to
  the clip: it stays out of the undo history and out of the preset's "modified", and undo leaves it alone, until the
  user takes hold of it again. A value within `kHostEchoMs` of a gesture does not count, since a host may echo the end
  of a drag. A restored state clears the marks. Clip values are not smoothed by the processor: each module glides or
  crossfades its own changes.
- No MIDI learn in the plugin (decided for 0.9.2, can be added if people ask for it). The plugin takes no MIDI: `acceptsMidi() == false`,
  `NEEDS_MIDI_INPUT FALSE`. FL links a hardware knob to any parameter itself (Tools > Last tweaked > Link to controller)
  and saves the link in the project. The reason: in VST3, CCs reach a JUCE plugin only through 16 x 130 extra parameters
  that JUCE adds to the plugin's list when it wants MIDI (`JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS`), which could
  fill the list above with 2080 rows; and the reference offers no right-click learn to match. If plugin-side learn is
  ever wanted, check that first (a plugin built with MIDI input, such as Chordbloom, shows whether FL lists those rows).
- `PureSheenDev automationtest` checks the lists of all 20 plugins (automatable, unique names, 0 and 100 are the
  control's ends, every choice reachable) and the behaviour above. `PureSheenDev automationscan <out.tsv>` measures one
  parameter of every kind under a clip: output sharpness moving against held still, and whether the panel follows.

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
  (stem-separation EQ, mix rebalancing). A
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
Maximizer target (Gain above 0 dB and Trim below it, along the measured slope). A drag writes only that macro's parameters (unrecorded while
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

## Vintage-style modules (phase 3)

Four modules modelled on classic analog circuit classes, under our own names: a feedback compressor, a passive LC
program EQ with a passive midrange EQ and a tube make-up stage, a two-stage limiter with a true-peak guard, and a
two-head tape machine. Clean room: public documentation and black-box renders only. The models are
fitted to those renders, with every fitted coefficient in one named constant block (`k...` in an anonymous namespace
at the top of its .cpp). The ranges, defaults, choice lists and units are the reference's, confirmed from the user's
screenshots, min-max notes, saved presets and measurements (table below).

| ModuleType | prefix | display name (short) | channel modes | T/S twin | params | latency (44.1 / 48 kHz) | owner |
|---|---|---|---|---|---|---|---|
| ClassicComp | ccmp | Classic Compressor (Classic Comp) | St, M/S | no | 29 | 0 | mod-ccmp |
| PassiveEq | peq | Passive EQ | St, M/S, L/R, T/S | yes | 36 | 3 (`kPassiveEqLatency`) | mod-peq |
| ClassicLimiter | clim | Classic Limiter | St | no | 7 | 104 / 112 (`ClassicLimiter::latencyFor`) | mod-clim |
| Tape | tape | Tape | St | no | 7 | 5 (`kTapeLatency`) | mod-tape |

(params = slot params incl. `.ts.*` + module params; 79 entries appended after `bass.autoGain`, indices 841..919,
`ccmp.on` .. `tape.highEmph`; 920 entries in all.)

**Parameters.** The vintage appendix follows the phase 2 rules: every module in ModuleType order, slot params first
(`.on`; `.chMode`, `.a.on`, `.b.on`, `.link` for the multi-mode ones; `.ts.sens` / `.ts.len` for the Passive EQ), then
its own params in struct order (shared params first, then set A, then set B). The Classic Compressor's controls are all
per channel set; the Passive EQ's Drive is shared (the tube stage), its two sections per set. The Passive EQ frequency
selectors are choices indexing `kPassiveEq*Hz` (Types.h); `ClassicCompMode`, `ClassicLimiterMode` and `TapeSpeed`
(`kTapeSpeedIps`) are their choice enums. Unlike the phase 2 modules, the defaults are the reference's, not neutral
settings: each header documents its neutral settings. The processor gives the appendix APVTS version hint 3
(`kNumParams_0_5_0` = 841 marks its start). `tests/ParamCompatTests.cpp` freezes the 0.2 .. 0.5 table (841 entries,
count + fingerprint) and the vintage appendix (79 entries, count + fingerprint of its own entries; the appendix is
unreleased, so a change to it updates that hash, the test prints it).

Confirmed ranges and defaults (every control shows the number the reference's control shows for the same setting;
per channel set where the module has two; TBC = read off a screenshot curve or not seen at an end stop):

| module | control (id) | range / choices | default | unit |
|---|---|---|---|---|
| Classic Compressor | Threshold (`thr`) | -60 .. 0 | -12 | dB |
| | Ratio (`ratio`) | 1 .. 20 (skew centre 4) | 1.5 | :1 |
| | Attack (`attack`) | 0.1 .. 100 (log) | 20 | ms |
| | Release (`release`) | 30 .. 150 (log), the transient release | 50 | ms |
| | Mode (`mode`) | Sharp, Balanced, Smooth (0..2) | Balanced | |
| | Gain (`makeup`) | -30 .. +30 | 0 | dB |
| | Auto Gain (`autoGain`) | off / on | on | |
| | detection high-pass (`sc.hpf`) | 20 .. 400 (log), fixed shape | 40 (measured; the curve reads ~45) | Hz |
| | detection bell (`sc.bellHz` / `sc.bellDb`) | 800 .. 9000 (log) / -8 .. +8, fixed shape | 2000 (TBC) / 0 | Hz / dB |
| | detection high shelf (`sc.shelfHz` / `sc.shelfDb`) | 4000 .. 12000 (log) / -8 .. +8, fixed shape | 6000 (TBC) / 0 | Hz / dB |
| Passive EQ | Low frequency (`lowHz`) | 20, 30, 45, 60, 100 | 45 | Hz |
| | Low Boost / Low Cut (`lowBoost` / `lowCut`) | 0 .. 10 (the labels; the gain follows the knob law) | 0 | dB |
| | High Boost frequency (`highHz`) | 3, 4, 5, 8, 10, 12, 16 | 8 | kHz |
| | High Boost (`highBoost`) | 0 .. 10 (label) | 0 | dB |
| | High Q (`highQ`) | 0 (broad) .. 10 (narrow) | 0 | |
| | High Cut frequency / High Cut (`highCutHz` / `highCut`) | 5, 10, 20 kHz / 0 .. 10 (label) | 10 kHz / 0 | kHz / dB |
| | Low-Mid (`lmHz` / `lmBoost`) | 200, 300, 500, 700, 1000 Hz / 0 .. 10 (label) | 500 Hz / 0 | Hz / dB |
| | Mid (`midHz` / `midCut`) | 200 Hz .. 7 kHz (11 steps) / 0 .. 10 (label) | 1.5 kHz / 0 | Hz / dB |
| | High-Mid (`hmHz` / `hmBoost`) | 1.5, 2, 3, 4, 5 kHz / 0 .. 10 (label) | 3 kHz / 0 | Hz / dB |
| | Drive (`drive`, ours, shared) | 0 .. 10 | 0 | |
| Classic Limiter | Threshold (`thr`) | -12 .. 0 (range TBC) | -2 | dB |
| | Ceiling (`ceiling`) | -12 .. 0 (range TBC) | 0 | dBFS |
| | Character (`character`) | 0 (fast) .. 10 (slow) | 2 | |
| | Mode (`mode`) | Analog, Tube, Modern (0..2) | Tube | |
| | True Peak / Link (`truePeak` / `link`) | off / on | off / off (Link TBC) | |
| Tape | Speed (`speed`) | 7.5, 15, 30 ips (ours 0..2; the reference stores 0 = 30, 1 = 15, 2 = 7.5) | 30 ips | ips |
| | Drive (`drive`) | -30 (TBC) .. +30 | 0 | dB |
| | Bias (`bias`) | -10 .. +10 | 0 | |
| | Harmonics (`harmonics`) | 0 .. 10 | 0 | |
| | Low Emphasis (`lowEmph`, head bump) | 0 .. 10 | 2 | |
| | High Emphasis (`highEmph`) | 0 .. 10 (4 = flat) | 4 | |

**Contracts** (headers are contracts, rule 1):
- `ClassicComp.h` (fitted): feedback detector (the gain-stage output, one sample of loop delay; Stereo: the mean of
  L and R, one gain; M/S: per set) through the sidechain EQ (1st-order high-pass at `sc.hpf`, a wide bell with half the
  knob's dB, a high shelf with twice it); a diode peak detector with a slow second capacitor (onset delay, slow creep,
  fast release after a short hit, slower after a long hold); c = ratio - 1; knees Sharp / Balanced quadratic, Smooth a
  softplus knee 10 dB above the threshold; no saturator (the odd harmonics are the detector ripple); auto gain = the
  ratio of 0.5 s averages of |x| and |v|, clamped 0 .. 20 dB, before makeup; latency 0; neutral (bit-exact) at ratio 1 /
  makeup 0 / auto gain off; `soloDetectionFilter` = hear the sidechain; meters per set (reduction, 1 s hold, detector
  level, auto gain); panel helpers `detectorResponseDb`, `staticCurveDb`.
- `PassiveEq.h` (fitted): seven sections per channel (first-order low boost and low cut with their measured
  interaction, bilinear; a high boost peak at the position's measured centre with its upper skirt carried by the high
  cut section; a first-order high cut; low-mid / mid / high-mid peaks: matched designs), the measured amount laws
  (labels 0 .. 10; low boost 5 = +8.7 dB, high boost 5 = +3.3 dB peak), then our 2x minimum-phase oversampled tube stage
  (Drive; at 0 an exact delay); latency 3 at every rate whatever the drive; neutral at the defaults (= input delayed
  by 3); twin per the Module.h rules; `getMagnitudeDb (set, hz)` for the panel (both sets designed by instance A,
  published through a seqlock).
- `ClassicLimiter.h` (fitted): input gain ceiling - threshold plus the mode's measured make-up (+0.35 dB Analog / Tube,
  +0.125 dB Modern, faded to 0 at threshold = ceiling); per channel (unlinked) and per mode (all three run, a mode change
  crossfades 20 ms) a slow stage on the overshoot over ceiling - 0.01 dB (measured depth and release laws per mode and
  Character) and a fast stage held over a short window W (mode, Character), the gain a box average over W; no knee and
  no colour below the ceiling; then `maxi::Guard` (0.5 ms, true peak at ceiling - 0.02 dB when on) and a sample clamp;
  latency `latencyFor (rate)` = (Wmax - 1) + the guard (104 / 112 / 210 at 44.1 / 48 / 96 kHz); Delta excludes the
  Threshold / Ceiling gain and the make-up (handlesDelta); meters (slow stage, the rest, peak reduction, output peak)
  and a reduction history FIFO.
- `Tape.h` (fitted): 4x minimum-phase oversampled: bias tilt -> record pre-emphasis (2nd-order high shelf +1.58 dB at
  1.32 kHz x speed / 30 ips) -> drive and the bias drive -> a memoryless quadratic soft clipper (no hysteresis),
  Harmonics shifting its operating point -> de-emphasis and the speed's reproduce bells -> high emphasis (4 = flat) ->
  top roll-off; at the base rate the speed's low end and the head bump (15 / 25 / 44 Hz) -> the speed's level times the
  measured drive compensation (high drives come out louder; bias uncompensated); no noise, wow or flutter; latency 5;
  no neutral setting; meters (drive level, saturation); `responseDb` (relative to 1 kHz) for the panel.

**Registration** (done in the contracts wave): Chain (`createModule`, the Passive EQ twin), the Add menu (its own
section), state / presets / undo (generic by id prefix), the chain strip (from 15 modules the compact tiles pack to
44 px with 4 px gaps, so all 18 fit at 980 px), colours (Classic Comp lime, Passive EQ pink, Classic Limiter sage,
Tape silver), Help ("More modules"), GenericModulePanel stubs in `src/ui/<Name>Panel.h/.cpp`, selftest
("-- Vintage-style modules": added before the Maximizer at their defaults, realtime groups with every parameter moved in every channel
mode, the contract latency in every mode at 44.1 / 48 kHz, nulls at the neutral settings, a 0.5.0 state loading
unchanged), snapshots (`17_*`, `PURESHEEN_SNAPSHOT_ONLY=vintage`), one test file per module. The CMake globs pick the
new files up (no CMakeLists change). The integration added the importer maps (see "Status (vintage integration)").

Ownership for the module wave (only edit your files; ask the lead for contract changes):
- mod-ccmp: ClassicComp.h/.cpp, tests/ClassicCompTests.cpp ("ccmp."), ui/ClassicCompPanel.h/.cpp
- mod-peq: PassiveEq.h/.cpp, tests/PassiveEqTests.cpp ("peq."), ui/PassiveEqPanel.h/.cpp
- mod-clim: ClassicLimiter.h/.cpp, tests/ClassicLimiterTests.cpp ("clim."), ui/ClassicLimiterPanel.h/.cpp
- mod-tape: Tape.h/.cpp, tests/TapeTests.cpp ("tape."), ui/TapePanel.h/.cpp
- lead (contracts) / integration: Types.h, Params.h, ParamTable.cpp, Chain.cpp, Widgets.cpp, ChainStrip.cpp,
  ModulePanels.cpp, HelpOverlay.cpp, FactoryPresets.cpp, SelfTest.cpp, Snapshot.cpp, ParamTests / ParamCompatTests,
  the importer maps; module owners report needed changes. Factory presets (3-5 per module), the importer map and
  the status section came with the integration (below); version 0.6.0 comes with the release after the fit.

## Status (vintage integration)

The current models, remaining errors and verification are in "Status (vintage fit)" below.

### Historical baseline before the measured fit

The implementation descriptions, measurements and verification results below record the earlier first-principles
integration. They are retained as history, not as the current model contracts or current fit results.

Done: the four vintage-style modules with real DSP, their own tests and a dedicated panel each (no stubs left), 18
factory presets (Classic Compressor 5, Passive EQ 5, Classic Limiter 4, Tape 4), the importer map, selftest coverage
and snapshots. **Not yet fitted to the reference's renders:** every module is built from first principles of its
circuit class, with every coefficient in the constant block at the top of its .cpp. The parameter ranges, defaults,
choice lists and units are the confirmed ones (table above). Nothing is installed; the version is unchanged.

What is real (per module; numbers from its CoreTests on the module owner's build and this integration's rerun):
- Classic Compressor (`ccmp.`, 18 tests): feedback gain stage with gain-referred RMS / peak followers (they average
  d / g and are read at the latest g; output-referred followers overshot a hit by 26 dB), knee 2 / 6 / 12 dB, c =
  ratio - 1, attack divided by 1 + the local slope so the knob is the closed-loop attack at any ratio (1.3 / 10.7 /
  50.7 ms measured for 1 / 10 / 50 ms, the same at ratio 2 and 20), program-dependent release (knob 200 ms: 278 ms
  after a short hit, 643 ms after 3 s of reduction), saturator above -30 dBFS scaled by the reduction (Sharp below
  -117 dBc, Balanced about -62, Smooth about -52 dBc), auto gain over 3 s (within 0.008 dB), detector listen. Neutral
  bit-exact; steady tones on `staticCurveDb` within 0.047 dB; glides per sample (block-size independent); click
  ratio <= 1.94; about 300x realtime. Latency 0.
- Passive EQ (`peq.`, 23 tests): seven matched sections per channel with 10 ms glides (low boost +13.7 dB at 10,
  boost 10 + cut 5 = +5.9 dB at 30 Hz / -4.3 dB at 100 Hz; high boost 16.02 dB, 0.38 .. 4.91 octaves over the
  High Q knob), the 2x oversampled tube stage (Drive 10: H2 -30.4 dBc, H3 -53.1 dBc, aliasing below -91 dBc; only
  its residual fades when Drive crosses 0). Neutral = input delayed by exactly 3 samples in every mode; the panel curve
  (shared designs, `dsp/PassiveEqDesign.h`) matches the audio within 7e-7 dB; parameter-step splatter <= -83.5 dB;
  about 140x realtime at 96 kHz. Latency 3 at every rate and drive; T/S twin.
- Classic Limiter (`clim.`, 22 tests): input gain Ceiling - Threshold (20 ms glide), stereo-linked variable-mu
  feedback stage g = 1 / (1 + u^2) with the 10 dB soft knee (effective ratio 1.34 .. 2.38, static curve within 0.04
  dB), attack 0.17 .. 15 ms and release 63 .. 2538 ms over the modes and Character, Tube / Modern saturation at 2x
  (+3 dBFS Tube: H2 -37, H3 -46 dBc; Analog H2 below -90 dBc; worst alias -105 dBc), then the Maximizer's brickwall
  (1.5 ms) and guard (0.5 ms), run per sample so the history display is exact (about 20 % CPU; 111x realtime). 216
  torture runs: the sample peak never above the ceiling, worst true peak -0.020 dB re ceiling. Neutral = input
  delayed exactly in all three modes. Latency 109 / 117 / 213 samples at 44.1 / 48 / 96 kHz.
- Tape (`tape.`, 20 tests): all at 4x minimum phase: record pre-emphasis at the speed's equal-distortion corner
  (500 Hz / 1 kHz / 2 kHz), a blend of three saturator curves over bias with rate-independent hysteresis on the
  residual, the even-harmonic offset (its DC removed in output units), exact reciprocal de-emphasis, the head bump
  (40 / 60 / 110 Hz bell on its roll-off; +10.00 dB at Low Emphasis 10), one net high shelf (High Emphasis 4 = exactly
  flat) and a static make-up for -18 dBFS pink noise (within 0.145 dB over 90 drive / speed / bias / harmonics
  settings up to drive +20; 0.32 dB at drive +30, the top of the confirmed range). Flat settings 200 Hz .. 10 kHz
  at +0.295 dB +-0.0006; measured response vs `responseDb` within 0.14 dB;
  THD of a 200 Hz tone at -6 dBFS -96.8 / -37.8 / -13.9 dB at drive -30 / 0 / +20; H2 -36.5 .. -26.6 dBc over
  Harmonics 2.5 .. 10; glides at fixed control positions (block sizes 1 .. 512 identical), click ratio <= 1.49;
  non-finite or huge input samples are zeroed before the oversampler (they poisoned its recursion), about 72x realtime.
  Latency 5 at every rate (the group delay 1.5 .. 4 kHz within 0.2 sample).

Integration checks (selftest "-- Vintage-style modules" and "-- Presets"):
- every module alone in every channel mode it offers reports its contract latency and nulls at its neutral settings
  (44.1 / 48 kHz); the latency reported to the host equals the MEASURED delay (group delay of two quiet tones at
  2.0 / 2.1 kHz, both channels, every mode, 44.1 / 48 / 96 kHz: worst 0.056 sample);
- a chain of the four (Classic Compressor in M/S, Passive EQ in T/S, every module and set on) with random values
  round-trips through the state, plays the same non-silent audio (within 1e-6) and reports the same latency (135
  samples at 48 kHz);
- realtime: every parameter of the four automated at 96 kHz with odd block sizes (1 .. 512), the detector listen and
  the limiter's Delta toggled: allocation free and finite; the existing groups (every parameter, every channel mode;
  T/S of the Passive EQ at 44.1 / 96 kHz) pass too;
- every vintage module preset loads as exactly one undo step, changes its module and plays finite audio; undo and
  redo restore the parameter values and chain order.

Importer (`src/plugin/import/ImportMapPhase3.cpp`, CoreTests "import.vint.", 8 cases): the four sections map by
hash key as the other families; every foreign default sits in ONE marked block (`vdef`) at the top of the file (Known
where sparse storage proves it, documented or confirmed by the user; Guess = read off a curve; the 4 Guess rows, the
compressor's three detection-filter frequencies and the limiter's Link, are listed in
`out/importer/defaults-to-confirm.md`). Decisions: our ranges, units and defaults are the foreign ones, so values map
1:1 (a value outside the range is clamped and counted); the Passive EQ amounts are the labelled dB on both sides; High Q
1:1 (same direction, 0 = broad); tape speed index reversed (theirs 0 = 30 ips). Corpus: the reference's 56 factory
presets of the four modules (writer 9030) import with 0 unmapped / 0 clamped values and play finite audio; the user's
105 vintage check saves of render rounds V1 + V2 (writer 120000) import with 0 unmapped / 0 clamped. A single-module
import inserts the Classic Compressor and Passive EQ after Dynamics, Tape after the Exciter, the Classic Limiter
before the Maximizer.

Final review-and-fix pass over the whole diff (independent reviewer): corrupt input samples (non-finite or beyond
1e6) are zeroed before the recursive state of the Classic Compressor, Passive EQ and Classic Limiter (Tape already
did so); the Classic Compressor's auto-gain clock now runs every 64 samples across host blocks (identical output for
blocks of 1 / 17 / 64 / 511); the integration checks were tightened (audible delay probes, an active-chain state
round trip, all 79 vintage parameters automated, exact one-step undo / redo of every module preset). It found no
defect in channel-mode resets, M/S / L/R routing, the T/S twin, the limiter's Delta, the detector listen, latency,
the importer tables or the panels.

Verification (after that pass; build-v-int CoreTests / PureSheenDev, build PureSheen_VST3 / PureSheen_CLAP; 0
warnings in our sources): CoreTests 675 test cases, 0 failed (83 "ccmp." / "peq." / "clim." / "tape." cases, 8
"import.vint."; the CPU benchmarks of other modules fail while the machine is busy and pass when rerun alone);
`PureSheenDev selftest` 326 passed, 0 failed; `PureSheenDev importtest` all passed (191 checks); `PureSheenDev
import` of the 56 factory presets: 56 imported, 0 unmapped, 0 clamped, all finite; `PureSheenDev snapshot`
(`PURESHEEN_SNAPSHOT_ONLY=vintage`): the four panels at 980x600 / 1180x720 / 1800x1100, Passive EQ in M/S and T/S,
Classic Compressor in M/S, the strip with all 18 modules, Help, reviewed by eye.

Known limitations of that pre-fit baseline:
- Not yet fitted to the reference's renders: curves, ballistics, saturation and knob laws are first-principles
  values. The factory presets were chosen from the models' behaviour, not by listening, and need a listening pass
  after the fit.
- Ranges and defaults are confirmed except: the compressor's detection-filter default frequencies (45 Hz / 2 kHz /
  6 kHz, read off a curve), the limiter's Threshold / Ceiling ranges (-12 .. 0 kept) and Link default, the tape
  drive's low end (-30 kept). The Passive EQ amounts are labelled in dB, but until the fit the DSP's knob law is still
  the first-principles one (the measured law gives e.g. +8.7 dB of low shelf at label 5); the Classic Compressor
  presets' releases were moved into 30 .. 150 ms (Bus Glue 120, Slow Leveler 150, the M/S one 100 / 150).
- Classic Compressor: the saturator is not oversampled (latency 0); its harmonics are bounded (>= 40 dB down at 0 dBFS),
  not filtered.
- Passive EQ: Drive is our addition (no foreign counterpart; the importer leaves it at 0).
- Classic Limiter: the per-sample brickwall costs about 20 % more CPU than a block-wise one (still 111x realtime).
- Not done: pluginval / clap-validator on this build, renders against the reference, listening tests in FL Studio.

## Status (vintage fit)

All four modules are fitted to black-box renders of the reference (17 .. 36 renders per module, two render rounds);
the contracts above and the DSP headers describe the measured models, every fitted coefficient is in the constant
block at the top of its .cpp. Version 0.6.0. Not installed.

Remaining error against the reference (worst render of each group, dB; FR = max magnitude error of the -30 / -10 dBFS
sweeps at 1/24 octave, static = max staircase gain error, env = gain-envelope error on the step blocks, null = music
residual, worst (best) render):

| module | group (renders) | FR max | static max | env rms / max | music null |
|---|---|---|---|---|---|
| Classic Compressor | auto gain off, round 2 (14) | 0.06 | 0.13 | 0.02 / 0.40 | -38.7 (-73.1) |
| | auto gain off, round 1 (1) | 0.03 | 0.03 | 0.01 / 0.10 | -45.4 |
| | auto gain on (9) | 2.49 (-30 dBFS at 20 Hz; -10 dBFS 0.05) | 0.03 | 0.10 / 0.50 | -37.6 (-47.9) |
| Passive EQ | low boost / cut (15) | 0.030 | 0.012 | 0.002 / 0.21 | -45.3 (-63.3) |
| | high boost (12) | 0.154 | 0.074 | 0.043 / 0.35 | -22.8 (-52.8) |
| | high cut (4) | 0.182 | 0.069 | 0.032 / 0.55 | -38.7 (-50.8) |
| | mid bands (4) | 0.070 | 0.035 | 0.004 / 0.19 | -41.0 (-62.6) |
| Classic Limiter | Analog, Character 0 .. 10, ceiling, True Peak (8) | 0.015 | 0.076 | 0.06 / 0.18 | -42.2 (-47.5) |
| | Tube default (2) | 0.004 | 0.134 | 0.01 / 0.06 | -79.7 (-86.1) |
| | Tube Character 0 / 5 (2) | 0.015 | 0.079 | 0.05 / 0.18 | -41.5 (-46.6) |
| | Tube Character 10 (1) | 0.010 | 0.198 | 0.15 / 0.20 | -35.4 |
| | Modern Character 0 / 5 / 10 (3) | 0.004 | 0.453 | 0.03 / 0.14 | -48.6 (-55.7) |
| | Analog threshold -12 (1) | 0.159 | 0.17 | 0.02 / 0.36 | -27.4 |
| Tape | speed / emphasis (14) | 0.14 | 0.10 | 0.07 / 0.07 | -9.4 (-11.7) |
| | drive (7) | 0.29 | 0.09 | 0.07 / 0.07 | -9.6 (-11.2) |
| | bias (4) | 0.13 | 0.07 | 0.06 / 0.07 | -9.6 (-11.2) |
| | harmonics (3) | 0.12 | 0.07 | 0.07 / 0.07 | -10.0 (-11.2) |

Decisions the fit made:
- Classic Compressor: detection high-pass default 40 Hz (the fit; the reference's curve reads about 45), parameter and
  importer default. No saturator, the odd harmonics are the detector ripple (h3 within 0.5 dB at 1 kHz and 100 Hz).
- Classic Limiter: no colour below the ceiling, no `maxi::LimiterCore`, channels unlinked; the panel now shows Slow /
  Fast stages. Latency 104 / 112 / 210 / 408 samples at 44.1 / 48 / 96 / 192 kHz (was 109 / 117 / 213). The make-up
  below 2 dB of gain was not measured: it fades to 0 at threshold = ceiling so that setting stays a pass-through.
- Tape: memoryless curve (no hysteresis was measured), head bump 15 / 25 / 44 Hz, high drives come out louder (the
  reference's law), bias uncompensated; latency stays 5.
- Passive EQ: Drive remains our addition (the reference is fully linear); the importer leaves it at 0.
- State: saves write each value's fixed point of the float -> normalised -> float trip, so save -> load -> save is
  byte-identical (clap-validator `state-reproducibility-binary` failed on one ulp of a log-range value); the live
  parameter keeps its value.

Verification (build-v-int CoreTests / PureSheenDev, build PureSheen_VST3 / PureSheen_CLAP; 0 warnings in our sources):
CoreTests 688 test cases, 0 failed; selftest 334 passed, 0 failed; importtest all passed; snapshots of the four panels
at 980x600 / 1180x720 / 1800x1100, reviewed by eye; pluginval strictness 10 with GUI tests (VST3): SUCCESS;
clap-validator: 31 passed, 0 failed, 12 skipped, 1 warning (`state-invalid-random`, see Core). Fit scripts rerun after
the final review-and-fix pass (independent reviewer: state fixed-point cutoff, leftover colour claims in Help): no fitted
constant changed.

Known limitations (vintage):
- Classic Compressor: at ratio >= 10 and attack <= 5 ms, a tone at or below the high-pass corner modulates the gain
  irregularly (bounded and finite; filtering the input instead fit clearly worse, so the reference probably does the
  same). Bell / shelf cuts, attack outside 1 .. 50 ms and other sample rates are not covered by any render. The auto-gain-on
  renders start with leftover host auto gain, hence their 20 Hz error.
- Passive EQ: phase drifts 5 .. 20 degrees near the top on big high boosts and cuts (the reference follows analog
  filters to about 1 degree), which limits those nulls; closing it needs oversampled filters. The reference's
  roll-off above 17 kHz is not modelled. Amounts between the measured ones, low cut at 30 / 45 Hz, high boost amounts
  at positions other than 8 kHz, High Q at amounts other than 5, mid bands at other positions: smooth interpolation.
- Classic Limiter: threshold -12 holds 0.4 .. 1.4 dB less slow reduction than the reference on dense overs; Tube
  Character 10 overshoot and two-part release partly reproduced (level-step max error 4.3 dB); Modern Character 0
  releases 2x slower just after a hold, its harmonics are up to 21 dB off; the reference's 0.13 dB of reduction just
  under the ceiling is not modelled.
- Tape: music null about -10 dB and true peak off by up to 1.1 dB: the reference leads by a constant 15 degrees from
  20 Hz to 2 kHz with no delay and rolls its top off in linear phase. Correcting our render's phase offline reaches -38 dB,
  so the level and saturation model holds. Matching it live would need 74+ samples of latency (not taken). At 192 kHz
  the reproduce filters' phase is 0.74 sample (documented, tested).
- Factory presets were chosen on the pre-fit models and passed an isolated level check only (worst Tape +1.41 dB RMS);
  they need a listening pass. Limiter Threshold / Ceiling ranges and Link default, tape drive's low end, and the
  compressor's bell / shelf default frequencies remain unconfirmed (TBC in the table above).
- Not done: listening tests in FL Studio.

## De-limiter (phase 3)

The last module on the list: `ModuleType::DeLimiter`, prefix `dlim`, display name De-limiter, Stereo only, no T/S twin.
Appended after the vintage appendix: 5 parameters (`dlim.on`, `amount`, `input`, `output`, `thr`), indices 920..924,
APVTS version hint 4, `kNumParams_0_6_0` = 920 marks the start, `tests/ParamCompatTests.cpp` freezes count + fingerprint.
Colour sky. Add inserts it first in the chain (it works on what a limiter left behind), the importer ranks it first too.

The reference uses a neural network. Ours is a small DSP model of what that network was measured to do, fitted to black-box
renders; `src/dsp/DeLimiter.h` is the contract, the constants are one block at the top of `DeLimiter.cpp`.

| control (id) | range | default | unit |
|---|---|---|---|
| Amount (`amount`) | 0 .. 200 | 0 | % |
| Input Gain (`input`) | -9 .. +9 | 0 | dB |
| Output Gain (`output`) | -9 .. +9 | 0 | dB |
| Threshold (`thr`) | -12 .. 0 | 0 | dB |

Measured on the reference and matched exactly:
- Output = input x (1 + Amount / 100 x (g - 1)), with one gain curve g >= 1 for both channels (L and R agree within
  0.000001 dB), Output Gain a plain gain afterwards. Base and a fresh instance null exactly: it is deterministic.
- Only the level against the threshold matters: Input Gain +6 scaled back nulls with Threshold -12 at Threshold -6 to -90 dB.
- The detector follows the mid: hits in the side only get no lift. Below about 14 dB under the threshold, no lift.
- Learn: waits without limit for the first audio, then a fixed clock of 6.00 s runs on through silence or a pause. The result
  is the highest sample peak of the louder channel in that window, after Input Gain, clamped to -12 .. 0. Stored as a plain
  value.

Modelled, not matched: the curve itself. See `DeLimiter.h` for the model. Against the reference (Amount 100 %, Threshold
at the ceiling, seven 8 s music blocks, all errors in dB of lift): rms 0.99, mean +0.09, 95th percentile -0.57, largest lift
-1.23; with Amount 50 / 150 / 200 % rms 0.55 / 1.36 / 1.69; Threshold 0 / -3 / -12 rms 1.20 / 1.05 / 1.09. Trained
without the song it is scored on, rms is about 1.1. Restoration against the audio before limiting is about as close as
the reference's on all 11 blocks. Learned threshold: worst difference 0.0004 dB over 28 files. Latency 80 samples at
44.1 kHz; the curves line up at lag 0. Tables: `out/delimit/fit-summary.md`.

Importer (`ImportMapPhase3.cpp`): the section maps 1:1 (all four defaults confirmed), no longer in the skipped list.
Tests: CoreTests `dlim.`, `import.vint.`, `params.`; selftest "-- De-limiter learn" (through the processor while a host
plays: waits for audio, allocation free, progress, runs on through a pause, one undo step, redo, cancel, removal cancels) and
the vintage group (latency, null, realtime, presets); snapshots `17_dlim_*`. `PureSheenDev delimitlearn <file>` runs Learn on a
file. Factory presets: Gentle Restore, Full Restore, Hot Master, Punch Restore.

Verification (build-dlim CoreTests / PureSheenDev, build PureSheen_VST3 / PureSheen_CLAP, 0 warnings in our sources): CoreTests
703 test cases: every `dlim.`, `import.vint.` and `params.` case passes; 4 CPU speed checks of other modules (Bass Shaper, Dynamic EQ,
Exciter, Low Contrast) fail with the machine under load, and the build from before this module fails the same ones in the same
conditions; selftest 356 passed, 0 failed; importtest all passed; the user's saved presets import with 0 unmapped, 0 clamped;
pluginval strictness 10 (VST3): SUCCESS; clap-validator: 31 passed, 0 failed, 12 skipped, 1 warning (`state-invalid-random`, as
before); snapshots of the panel reviewed by eye, all 19 modules fit the strip at 980 x 600. An independent review found four defects, all fixed:
the history FIFO's storage moved on a re-prepare, Learn's length was read across threads unprotected, a result captured before the
module's removal could still apply, and a result arriving during an open edit joined that edit's undo step (it now waits).

Known limits:
- The sample-by-sample curve follows a network's choices that this model cannot predict: correlation with the reference's
  curve is about 0.5 on music. Sizes and behaviour match; individual spikes do not. Largest lifts run about 1 dB short, and
  about 3 dB short at Threshold -12.
- At Threshold 0 on a -6 dB master the reference gives broad lift and ours 0.6 dB less on average (1.7 dB less on the worst
  block). Steady tones are the weakest case: the reference's response to them is irregular and we did not chase it.
- Only one clipped music block was rendered, so the flat-top boost rests on it and on the clipped hits.
- Fitted at 44.1 kHz; other rates scale the times, no renders cover them. Delta is the slot's generic one (the reference's
  was not measured). A host that stops calling the plug-in pauses Learn's clock until it resumes; the button cancels it.
- The reference may have far more latency than ours (the threshold changed about 1.1 s after the window on FL's clock, not
  measurable from the renders, since FL compensates). Our output is unaffected.
- Not done: listening tests in FL Studio.

## Maximizer Trim and film targets (1.1.0)

Output Level (`max.ceiling`) is a ceiling, not an output gain: material below it passes untouched, so lowering it does not
bring a track down to -23 LUFS. Gain's range (0 .. +20) cannot grow, because hosts store automation normalised. So the way
down is a new parameter, `max.trim` (Maximizer Trim, -24 .. 0 dB, default 0), appended after `dlim.thr` at index 925, APVTS
version hint 5, `kNumParams_1_0_0` = 925 marks the start, `tests/ParamCompatTests.cpp` checks it. `Maximizer::totalGainDb`
(gain + trim, clamped to -24 .. +20) drives the gain ramp, and old sessions load with trim 0 and sound the same.

Learn and the Assistant treat the two as one line: above 0 dB the amount goes on Gain, below it on Trim. `takeLearned` keeps
its signature and returns a negative value for trim, and `applyMacros` and `OfflineChain::refineLoudness` split the same way.
The Assistant's lowest target is `kMinTargetLufs` = -24. Going below the reference's range is deliberate. Factory presets
"Film and TV -23 LUFS" and "US Broadcast -24 LUFS" (category Film and Broadcast) use only the Maximizer, because a film mix
should stay as mixed, and trim sets their level; "Film Target -23" and "Broadcast Target -24" are the module presets.

## Monitoring options, band dynamics and macOS (1.2)

From a user's feedback. Two options in the Options menu, stored with the other UI options (per instance, in the
session) and applied to the chain by the processor (`applyMonitoringOptions`), so they hold with no editor open:
- Gain Match > "Meters show the output without the match gain" (`Chain::setMetersIgnoreGainMatch`): the output taps
  (IoMeters' output values and the output FIFO, so the I/O panel, analysers and the connector) get the same mix without
  either match gain. Modern mode then levels what is heard while the meters read the master. They leave out the dither.
- "Keep the Maximizer on while soloing a module" (`Chain::setSoloKeepsMaximizer`): a powered Maximizer stays enabled
  while another module is soloed. Gain match stays frozen (solo is still monitoring).

`TrackAnalyser` keeps, per analysis band, a histogram of the band's level in each frame (0.25 dB steps, since reset and
forgetting like Recent). `BandStats::dynamicRangeDb` (95th - 10th percentile) and `peakOverMedianDb` (99th - 50th) are
read from it, frames more than 20 dB below the band's mean power left out. The connector's `get_comparison` gives
them for master and reference, and `get_meters` the multiband Dynamics' per-band detector and gain values.

macOS: `CMAKE_OSX_DEPLOYMENT_TARGET` 11.0, Audio Units added on Apple, `.github/workflows/macos.yml` builds universal
binaries, packages them with `scripts/package-mac.sh`, and runs the tests and auval. Apple's C++ library has
floating-point `to_chars` / `from_chars` only from macOS 13.3 / 26, so `core/NumberText.h` uses a classic-locale stream
there. The helper is `PureSheenBridge` without `.exe`, found in `/Applications/PureSheen Dynamics`, and checks for dead
instances with `kill (pid, 0)`.

## Connection to Claude (phase 4)

Lets Claude Desktop or Claude Code control the suite over MCP. Suite only: a single-module plugin has no bridge. Three parts:

- `src/plugin/Bridge.h/.cpp`: the suite's end. Off until the user turns on Options > Claude > Allow Claude to connect, a flag
  file shared by every instance and checked once a second. While on, a listener takes connections on 127.0.0.1 only, on a
  free port, and a registration file holds the port and this instance's random token. A connection that does not start
  with the token is closed, so a web page that reaches the port gets nothing. Threads only read, parse and write.
- `src/plugin/BridgeCommands.cpp`: the commands, on the message thread (queued through an AsyncUpdater), through the
  processor's public calls. So nothing touches the audio thread, the editor follows, and a command is one undo step.
  `beginClaudeRequest` / `endClaudeRequest` label those steps "Claude: " and number them. Pending user edits are flushed
  first, so they keep their own name.
- `tools/bridge/BridgeMain.cpp`, `PureSheenBridge.exe`: what Claude starts. It speaks MCP on stdin / stdout, holds the tool
  list (so Claude sees the tools while no plugin is open), reads the registration files, removes those of dead processes,
  and passes calls on. juce_core only. `BridgeProtocol.h` is what both sides share.

What it leaves out on purpose: deleting presets (no undo), monitoring (solo, delta, A/B, and the parameters
`isMonitoringParam` names, such as Bypass and Gain Match: undo does not restore those, and a forgotten one changes what the
user hears), and editing another program's settings (the menu copies text the user pastes). A preset or snapshot in use is
replaced only with `overwrite: true`. Claude cannot hear or start playback, which the server's `instructions` say.

Each command that changes state gets a request number, returned as `request` in its reply and shown on its steps in
`get_history` (not in the plugin's UI). Results that arrive after the command (the Assistant's default card, Learn) are
recorded later by the housekeeping timer, so a job started by Claude stores its number (`maxLearnRequest`, `delimLearnRequest`,
`assistantRequest`) and `ScopedRequest` restores it when the result records. Those entries keep the "Claude: " label and the
number of the request that started them, while a job the user started stays theirs. The helper handles one call at a time and
does not read stdin during a call, so MCP cancellation has no effect: a call ends within its timeout.

Replies are written to save a reader from guessing: `set_parameters` returns `previous`, `limited` when a value hit the end of
its range and `channel_note` when a set has no effect (set B in Stereo, Channel Link, which only ties UI edits);
`assistant_set_macro` returns the Gain and Trim it moved and `limited` when capped; `get_parameters` with `changed_only` keeps
a band's `shape` and `on`, and lists a long choice list once per reply (`choices_as`); `get_meters` gives
`host_position_seconds`, `peak_window_seconds` and `seconds_since_last_change`; `get_comparison` gives
`settings_changed_during_analysis` and the host's position, and its profile gives `sample_peak_db` and `peak_to_loudness_db`,
because PLR uses sample peak and three sessions read it against the true peak as a fault. `reset_comparison` starts that
analysis again. The helper rounds its JSON to 4 decimals.

Every line is checked for size and nesting before `juce::JSON::parse`, which recurses: otherwise anything that can reach the
port could overflow the stack and take the host down, token or not.

The Maximizer's `gainReductionDb` is one block's value, usually 0 between peaks, and the panel takes `peakGainReductionDb`, so
the chain keeps a third value for the bridge (`bridgePeakGainReductionDb`): a read here must not take the panel's peak.
`assistant_settings` rebuilds only with `rebuild: true`, unlike the panel's Start, because Claude changing one setting should
not replace the settings the user is hearing.

`get_meters` reports true peak but no sample peak, so it also says which peak the ceiling limits (`ceiling_applies_to`).
Otherwise an AI client reads the Assistant's normal result as a fault and fixes it unprompted: suggestions start with True Peak
off and a -0.1 dB ceiling, as the reference's do, so their true peak can read above the ceiling.

`get_meters` reads the true peak meters with `exchange` (nothing else reads them), and leaves out `IoMeters::inputPeak` and
`outputPeak`: the chain does not write them, as the I/O panel measures from its own stream. The spectrum FIFOs have one consumer,
the editor, so the bridge does not offer a live spectrum.

Tests: `PureSheenDev bridgetest` (refusals and what commands do, in process) and `tools/bridge/test_bridge.py` (MCP through
the real helper against `PureSheenDev bridgehost`, on a temporary folder set by `PURESHEEN_BRIDGE_DIR`). It also checks that
the suite knows every tool the helper lists.

## Single-module plugins

Each module also ships as a plugin of its own, for people who want one tool and not the suite. 19 plugins: one per
module type except Equalizer 2 (the same module as Equalizer 1), plus Dither. The Assistant and Track Referencing are
not plugins: the Assistant builds chains across modules, and RefMatch already is Track Referencing as a plugin.
`installer/products.csv` is the one list; CMake reads it for the targets and for `ProductTable.inc` (the `psplug::Product`
table), `scripts/package-all.ps1` for the packages. CMake stops on a repeated id, name, plugin code or installer id.

**The section in FL Studio's plugin list comes from the VST3 category, under two conditions found by testing:** the bundle
has no `moduleinfo.json` (FL records no category for one that does), and the category has exactly two words, because FL
names the section after all of it (`Fx EQ Mastering` makes `Fx-EQ-Mastering`). So the plugins set `VST3_AUTO_MANIFEST FALSE`,
CMake stops on any other number of categories, and `package-all.ps1` stops on a bundle with the file. A host then reads
the categories by loading the plugin instead of from that file.

FL reads the category only the first time it sees a plugin. Its master list, `Installed\.Plugins.ini` (hidden) in the
plugin database, keeps the entry through updates and rescans, so a PC that had a version before 0.8.1 stays unsorted until
the plugin's entries are gone from that list. A fresh install sorts itself. When a category seems ignored, the Plugin
Manager log in `Support\Logs` tells the two apart: `Skipped` means FL did not load the plugin, `Verify plugin` means it did.

**It is the suite's code, locked at runtime.** `PureSheenProcessor (product)`, `plugin/Product.h`:
- Chain: fixed to the one module (`Chain::lockToModule`): only that module, its T/S twin and its slot are prepared, so
  the others take no buffers and start no worker threads. Their objects still exist, because `Chain` hands them out by
  type. The processor forces the order in `setOrder`, so a state, an undo step or a snapshot cannot change it.
- Parameters: the host sees the module's and the I/O panel's (`io.*`, `in.*`, `out.*`), with the suite's ids (VST3 hashes the id). Not `<prefix>.on`: Bypass
  is the plugin's one on / off. Everything else sits at a fixed value in `Impl::fixedValues`, so the index-based `raw`
  array, `capture()` and the chain work unchanged. `params[i]` is null for those: `setParam` ignores them,
  `getParameter` returns null, and `getParameterValue` still answers.
- The Assistant and Track Referencing do not exist (null; suite only), and neither do their state children.
- Presets: `Product::presetScope()` is where the header's preset bar works, -1 in the suite, the module in a plugin. The
  preset name and "modified" follow that scope. The folders are the suite's (`Documents/PureSheen Dynamics/Presets/<prefix>`),
  so a module preset saved in either shows in both with no export step; PureSheen EQ also lists `eq2` (EQ presets load in
  either equalizer). Global suite presets do not load in a plugin. Host sessions do not move across: different plugin ids.
- Editor: no chain strip (window 78 px lower), no Assistant / Chain / Reference switch, no REF, no Dither shortcut, no
  reference overlay, no file drop, no preset chip on the panel (the header has the presets). Module power is gone from
  the UI with the strip. Help is cut down to the plugin.
- Dither plugin: no module and no presets. `dith.on` is fixed on, and because the chain dithers after its bypass,
  `processBlock` turns it off while Bypass is on. `dith.dc` is hidden: that filter sits before the Maximizer.

**Compiling once.** JUCE's module sources are INTERFACE sources: every target that links a module compiles them. Source
file properties are per directory, so `PureSheenCommon` (our code and JUCE) and `PureSheenDev` live in `cmake/common`, and
the top directory marks JUCE's module sources `HEADER_FILE_ONLY`. A plugin there compiles only `src/entry/PluginEntry.cpp`
and JUCE's wrapper for its format. Common defines the few `JucePlugin_*` macros module code reads; the per-plugin ones
matter only in the wrappers, which still compile per plugin. `juce_audio_plugin_client` is linked for its availability
macro (`juce_gui_basics` checks it) and has no shared sources. The Ninja pool `ps_link` allows 3 links at once, because
link-time-optimised links in parallel can crash the MSVC linker and each one uses many threads.

**Packaging.** `scripts\package-all.ps1` builds everything with the static runtime in `build-release`, fails if a binary
imports `MSVCP140` or `VCRUNTIME140`, and writes `dist\<Name>-Setup-<version>.exe` and `dist\<Name>-<version>-Manual.zip`
per plugin. The suite keeps `installer/PureSheen.iss` and its AppId; the others share `installer/PureSheenSingle.iss`.
Everything is under the AGPLv3 (`LICENSE`, and the README's Licence section). Every installer and zip carries `LICENSE.txt`
and `installer/THIRD-PARTY-NOTICES.txt` (the notices of what is compiled in; update it when a dependency changes).

**Checks.** `tools/dev/ProductTests.cpp`, part of `selftest` and on its own as `PureSheenDev producttest [id]`: the
parameter set, the chain lock, state and snapshots, presets both ways, imports, realtime safety, the bypass null, the
sound against the suite with only that module, the editor. `PureSheenDev productsnapshot <dir> [id]` renders each window at
three sizes and its help. `PureSheenDev memory 48000 product=<id>` measures one plugin.

Verification (0.8.0; build-single CoreTests / PureSheenDev, build-release for all 20 plugins, 0 warnings in our sources):
CoreTests 703 test cases, 0 failed; `PureSheenDev selftest` 357 passed, 0 failed, including `producttest` (1147 checks over
the 19 plugins, among them each one against the suite with only that module in the chain, under -90 dB difference);
`importtest` all passed. pluginval strictness 10 with GUI tests: SUCCESS on all 20. clap-validator: 0 failed on all 20
(the known `state-invalid-random` warning; `process-audio-denormals` warned on some plugins while six validators ran at
once, and did not repeat alone). The suite: class ids equal to the 0.7.0 build, and `render` of three global presets
bit-identical to the 0.7.0 tool. Memory after 2 s of audio at 48 kHz (`memory`): suite +157 MB, PureSheen EQ +30 MB,
Match EQ +29 MB, Maximizer +12 MB, Dither +7 MB. A full build of all 20 links about 40 binaries, about 15 minutes after
the one compile. One unpacked zip passes pluginval, and one test installer installs files identical to the build.
Not tested: a clean PC without the VC++ runtime (imports checked with dumpbin instead), and FL Studio itself.
