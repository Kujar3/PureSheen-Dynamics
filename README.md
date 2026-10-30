# PureSheen Dynamics

A complete mastering suite in one plugin: two equalizers, multiband dynamics, a stereo imager, a harmonic exciter, a
maximizer and dither, in a chain you can reorder. It has loudness metering, gain-matched bypass, per-module solo and
delta listening, A-D snapshots, undo history and factory presets for common targets (streaming, club, radio).

VST3 + CLAP + Standalone, Windows, built with JUCE 8. Made for FL Studio, works in any DAW.

![PureSheen Dynamics](docs/screenshot.png)

## Install

**Installer:** run `PureSheen-Dynamics-Setup-<version>.exe` (built with *Building > Installer* below). It installs:
- the VST3 into `C:\Program Files\Common Files\VST3\`
- the CLAP into `C:\Program Files\Common Files\CLAP\`
- optionally, the Standalone app

If FL Studio has the plugin loaded, the installer offers to close it first.

**From a build:**

1. Build it (see *Building* below).
2. Copy it into the system plugin folders. Run this from the repository folder in PowerShell; it asks for admin once:

   ```
   powershell -ExecutionPolicy Bypass -File scripts\install-plugin.ps1
   ```

   Without admin rights, add `-CurrentUser`. That installs into `%LOCALAPPDATA%\Programs\Common\VST3` and `...\CLAP`.
   Then add those folders in FL's *Options > Manage plugins > Plugin search paths*.
3. In FL Studio, open *Options > Manage plugins* and click *Find installed plugins*. Put **PureSheen Dynamics** in the
   last effect slot of the Master mixer track.

## Using it

The **?** button in the top-right corner explains every part inside the plugin. A short version:

1. **Start from a preset.** Click the preset name in the header.
   - *Streaming Ready* aims at about -14 LUFS and -1 dBTP.
   - *Loud and Clear* aims at about -9 LUFS, *Club Loud* at about -8 LUFS.
   - Other presets: *Soft Sheen*, *Wide Supersaw Master*, *Deep Bass Anchor*, *Acoustic Air*, *Warm Glue*,
     *Limit Only*, *Radio Pop*, *Rock Drive*, *Hip-Hop Knock*, *Mid-Side Clarity*.
   - Every module also has its own presets.
2. **Set the loudness.** Play the loudest part of the song and click **Learn** on the Maximizer. It sets the input gain
   so the song lands on the target next to the button.
3. **Compare honestly.** Turn on **Gain Match**, then toggle **Bypass**. Both play at the same loudness, so you judge
   the sound, not the volume. **S** on a module solos it. The **triangle (Delta)** plays only what that module
   changes.
4. **Keep versions.** Snapshots **A-D** store complete settings. The clock icon opens the undo history.

The modules (drag them in the chain strip to reorder, **Add** for more):

- **Equalizer 1 / 2**: up to 8 bands per channel set (Stereo, Mid/Side or Left/Right).
  - Shapes: bells, shelves (analog, Baxandall, vintage, resonant, band), high / low cuts up to 48 dB/oct (flat or
    resonant), brickwall cuts, and surgical bands.
  - Analog mode has zero latency. Digital mode is linear-phase (FIR 4096-32768), with a phase blend.
  - Also: soft saturation, Amount, and band audition (Alt-drag).
- **Dynamics**: 1-4 bands, each with a compressor and a limiter.
  - Envelope, peak or RMS detection, a detection filter with tilt, lookahead.
  - Adaptive release, auto gain, and a transfer-curve view.
- **Imager**: width per band, Stereoize (two flavours), Recover Sides and Prevent antiphase.
  - A vectorscope (polar sample, polar level, Lissajous) with a correlation history.
- **Exciter**: 7 saturation models per band (Analog, Retro, Tape, Tube, Warm, Triode, Dual Triode), drive and mix.
  - 4x oversampling and a post filter.
- **Maximizer**: six limiter modes (Low Latency, Smooth, Transparent, Aggressive, Spectral, Multiband).
  - True peak, soft clip, transient emphasis, stereo independence and upward lift.
  - Learn input gain to a LUFS target.
- **Dither**: for 16-bit exports. Bit depth, noise shaping, harmonic suppression and auto-blank on silence.
- **I/O**: input / output meters and gain, and peak, RMS, momentary, short-term, integrated, true peak and LRA
  readouts.
  - Mono, Swap, Bypass and Gain Match monitoring.

## Building

You need Visual Studio 2022 with *Desktop development with C++*, plus CMake and Ninja (`pip install cmake ninja`).
JUCE 8.0.16 goes in `third_party/JUCE`:

```
git clone --depth 1 --branch 8.0.16-security https://github.com/juce-framework/JUCE.git third_party/JUCE
scripts\fetch_clap.bat
scripts\build.bat build PureSheen_VST3 PureSheen_CLAP PureSheen_Standalone CoreTests PureSheenDev
```

`fetch_clap.bat` adds the optional CLAP build. Then run the checks:

- `build\CoreTests.exe`: DSP unit tests (filters, crossovers, oversampling, convolution, every module), measured
  against reference responses, null tests and true-peak limits
- `build\PureSheenDev_artefacts\Release\PureSheenDev.exe selftest`: processor-level integration tests (latency,
  nulls, state, presets, undo, and no allocation on the audio thread)
- `build\PureSheenDev_artefacts\Release\PureSheenDev.exe snapshot out\snaps`: renders the UI to PNGs
- `build\PureSheenDev_artefacts\Release\PureSheenDev.exe render in.wav out.wav --preset "Streaming Ready"`: offline
  render with loudness / true peak / LRA readout
- `tools\pluginval\pluginval.exe --strictness-level 10 --validate "build\PureSheen_artefacts\Release\VST3\PureSheen Dynamics.vst3"`

### Installer

The installer is built with [Inno Setup 6](https://jrsoftware.org/isinfo.php) from a static-runtime build, so the
target machine needs no Visual C++ redistributable. One command builds it:

```
scripts\build-installer.bat
```

The setup lands in `dist\`. Its version is the `project()` version in `CMakeLists.txt`.
`scripts\build-installer.bat /DTestRoot=C:\Temp\puresheen-test` builds a test variant instead. It installs per user,
under that folder.

See `ARCHITECTURE.md` for how the code is organised.

## Licences

- JUCE 8: AGPLv3 or the JUCE licence. Fine for personal use; check the JUCE terms before you distribute builds.
- Inter font: SIL Open Font License 1.1.
- clap-juce-extensions: MIT.
- The logo is drawn in code (`PureSheenDev icon`).
