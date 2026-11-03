#include "core/Loudness.h"
#include "core/RefCompare.h"
#include "plugin/PluginProcessor.h"
#include "plugin/TrackReferencing.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <iomanip>
#include <iostream>

// PureSheenDev reference <master> <reference> [--chain default] [--loop <start> <end>] [--listen <out.wav>]
//   Track Referencing on real files, the way a host plays them: <master> streams through the processor (an empty chain
//   unless --chain default, so the master is the file itself), <reference> is loaded into the reference list (decoded,
//   resampled to the master's rate, analysed once). Prints both profiles, the comparison and its hints, then listens to
//   the reference for 30 s with Integrated level match and checks that it is heard as loud as the master.
namespace
{
struct PlayHead final : juce::AudioPlayHead
{
    long long time = 0;
    double rate = 48000.0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (true);
        info.setTimeInSamples ((juce::int64) time);
        info.setTimeInSeconds ((double) time / rate);
        return info;
    }
};

std::string num (float v, int decimals = 1)
{
    return ps::hasValue (v) && std::isfinite (v) ? ps::formatValue (v, decimals, nullptr) : std::string ("--");
}

void printProfile (const char* title, const ps::TrackProfile& p)
{
    std::cout << title << ": " << (p.valid ? "valid" : "NOT valid") << ", " << std::fixed << std::setprecision (1)
              << p.analysedSeconds << " s analysed\n"
              << "  integrated " << num (p.integratedLufs) << " LUFS, short-term " << num (p.shortTermLufs)
              << ", LRA " << num (p.loudnessRangeLu) << " LU, true peak " << num (p.truePeakDb, 2) << " dBTP, sample peak "
              << num (p.samplePeakDb, 2) << " dBFS, PLR " << num (p.plrDb) << " dB\n"
              << "  correlation " << num (p.correlation, 2) << ", width " << num (p.widthPct, 0) << " %, low-end correlation "
              << num (p.lowEndCorrelation, 2) << "\n  bands (dB rel. spectral loudness " << num (p.spectralLoudnessDb) << "):";
    for (int b = 0; b < ps::kNumBands; ++b)
        std::cout << " " << ps::bandInfo (b).shortName << " "
                  << num (p.bands[(size_t) b].levelDb - p.spectralLoudnessDb) << " (w " << num (p.bands[(size_t) b].widthPct, 0)
                  << ")";
    std::cout << "\n";
}

bool readFile (const juce::File& file, juce::AudioBuffer<float>& out, double& rate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return false;
    rate = reader->sampleRate;
    out.setSize (2, (int) reader->lengthInSamples);
    reader->read (&out, 0, (int) reader->lengthInSamples, 0, true, reader->numChannels > 1);
    if (reader->numChannels == 1)
        out.copyFrom (1, 0, out, 0, 0, out.getNumSamples());
    return true;
}

bool waitForLoads (PureSheenProcessor& proc, double seconds)
{
    auto& refs = proc.getTrackReferencing();
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < seconds * 1000.0)
    {
        proc.flushPendingChanges (false);
        if (refs.waitUntilIdle (0))
        {
            proc.flushPendingChanges (false);
            bool loading = false;
            for (const auto& e : refs.getInfo().entries)
                loading = loading || e.state == psplug::TrackReferencing::State::Loading;
            if (! loading)
                return true;
        }
        juce::Thread::sleep (10);
    }
    return false;
}

// Streams [from, to) of `audio` through the processor in 512-sample blocks; the analysis runs synchronously every
// second, so a faster-than-real-time run never overflows its FIFO. `record` (optional) gets the output.
void stream (PureSheenProcessor& proc, PlayHead& head, const juce::AudioBuffer<float>& audio, int from, int to,
             juce::AudioBuffer<float>* record)
{
    juce::AudioBuffer<float> block (2, 512);
    juce::MidiBuffer midi;
    int sinceAnalysis = 0;
    for (int pos = from; pos < to; pos += 512)
    {
        const int n = juce::jmin (512, to - pos);
        block.setSize (2, n, false, false, true);
        block.copyFrom (0, 0, audio, 0, pos, n);
        block.copyFrom (1, 0, audio, 1, pos, n);
        proc.processBlock (block, midi);
        head.time += n;
        if (record != nullptr)
        {
            record->copyFrom (0, pos - from, block, 0, 0, n);
            record->copyFrom (1, pos - from, block, 1, 0, n);
        }
        sinceAnalysis += n;
        if (sinceAnalysis >= (int) head.rate)
        {
            proc.getTrackReferencing().runAnalysisNow();
            sinceAnalysis = 0;
        }
    }
    proc.getTrackReferencing().runAnalysisNow();
}
} // namespace

int runReference (const juce::StringArray& args)
{
    if (args.size() < 2)
    {
        std::cout << "usage: PureSheenDev reference <master> <reference> [--chain default] [--loop <s> <e>] "
                     "[--listen <out.wav>]\n";
        return 2;
    }

    const auto cwd = juce::File::getCurrentWorkingDirectory();
    const auto masterFile = cwd.getChildFile (args[0]);
    const auto refFile = cwd.getChildFile (args[1]);
    const bool defaultChain = args.contains ("--chain") && args[args.indexOf ("--chain") + 1] == "default";
    juce::File listenFile;
    if (args.contains ("--listen"))
        listenFile = cwd.getChildFile (args[args.indexOf ("--listen") + 1]);
    double loopStart = -1.0, loopEnd = -1.0;
    if (args.contains ("--loop") && args.indexOf ("--loop") + 2 < args.size())
    {
        loopStart = args[args.indexOf ("--loop") + 1].getDoubleValue();
        loopEnd = args[args.indexOf ("--loop") + 2].getDoubleValue();
    }

    juce::AudioBuffer<float> master;
    double rate = 0.0;
    if (! readFile (masterFile, master, rate))
    {
        std::cout << "reference: cannot read " << masterFile.getFullPathName() << "\n";
        return 1;
    }

    PureSheenProcessor proc;
    PlayHead head;
    head.rate = rate;
    proc.setPlayHead (&head);
    proc.prepareToPlay (rate, 512);
    if (! defaultChain)
        proc.setChainOrder ({});

    auto& refs = proc.getTrackReferencing();
    auto settings = refs.getSettings();
    settings.averaging = ps::Averaging::SinceReset;   // the whole master, like the reference
    settings.levelMatch = ps::LevelMatch::Integrated;
    refs.setSettings (settings);

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const int id = refs.addReference (refFile);
    if (id == 0 || ! waitForLoads (proc, 300.0))
    {
        std::cout << "reference: the reference did not load\n";
        return 1;
    }
    const double loadSeconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;

    const auto info = refs.getInfo();
    const auto& e = info.entries.front();
    std::cout << "reference: " << e.name << " (" << (e.state == psplug::TrackReferencing::State::Ready ? "ready" : "NOT ready")
              << (e.error.isNotEmpty() ? ", " + e.error : juce::String()) << ") " << std::fixed << std::setprecision (2)
              << e.lengthSeconds << " s, file " << e.fileSampleRate << " Hz / " << e.fileChannels << " ch, decoded at "
              << rate << " Hz, loaded and analysed in " << loadSeconds << " s\n";
    if (e.state != psplug::TrackReferencing::State::Ready)
        return 1;

    if (loopStart >= 0.0 && loopEnd > loopStart)
    {
        refs.setLoop (id, 0, loopStart, loopEnd);
        waitForLoads (proc, 120.0);
    }

    std::cout << "master: " << masterFile.getFileName() << (defaultChain ? " through the default chain" : " (empty chain)")
              << ", " << (double) master.getNumSamples() / rate << " s\n";
    stream (proc, head, master, 0, master.getNumSamples(), nullptr);

    const auto mix = refs.getMixProfile();
    const auto ref = refs.getReferenceProfile();
    printProfile ("master", mix);
    printProfile ("reference (active region)", ref);

    const auto c = refs.getComparison();
    std::cout << "comparison: " << (c.valid() ? "valid" : "NOT valid") << ", loudness " << num (c.loudnessDiffLu)
              << " LU, PLR " << num (c.plrDiffDb) << " dB, LRA " << num (c.lraDiffLu) << " LU\n  band differences:";
    for (int b = 0; b < ps::kNumBands; ++b)
        std::cout << " " << ps::bandInfo (b).shortName << " " << ps::formatSigned (c.bands[(size_t) b].levelDiffDb, 1)
                  << " [" << (int) c.bands[(size_t) b].severity << "]";
    std::cout << "\n  hints:\n";
    for (const auto& h : c.hints)
        std::cout << "    [" << (int) h.severity << "] " << h.title << "\n";

    // A/B: listen to the reference with Integrated level match while the master keeps playing.
    refs.setListening (true);
    const int listenLength = juce::jmin (master.getNumSamples(), (int) (30.0 * rate));
    juce::AudioBuffer<float> heard (2, listenLength);
    stream (proc, head, master, 0, listenLength, &heard);
    ps::LoudnessMeter meter;
    meter.prepare (rate);
    const int skip = (int) rate;   // the A/B fade and the gain glide
    meter.process (heard.getReadPointer (0, skip), heard.getReadPointer (1, skip), listenLength - skip);
    const float heardLufs = meter.integratedLufs();
    const float wantLufs = mix.integratedLufs;
    std::cout << "listen: match gain " << num (refs.getMatchGainDb(), 2) << " dB; the reference is heard at "
              << num (heardLufs, 2) << " LUFS (" << (listenLength - skip) / rate << " s; it plays its own loudness "
              << num (ref.integratedLufs, 2) << " over the whole region), the master is " << num (wantLufs, 2) << " LUFS\n";
    std::cout << "dropouts: " << refs.getDropoutCount() << "\n";

    if (listenFile != juce::File())
    {
        listenFile.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out (listenFile.createOutputStream().release());
        const auto options = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24);
        if (auto w = wav.createWriterFor (out, options))
            w->writeFromAudioSampleBuffer (heard, 0, heard.getNumSamples());
        std::cout << "listen: wrote " << listenFile.getFullPathName() << "\n";
    }

    proc.releaseResources();
    return 0;
}
