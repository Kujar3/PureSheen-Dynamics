#include "core/Loudness.h"
#include "core/TruePeak.h"
#include "plugin/PluginProcessor.h"
#include "plugin/import/XmlPresetImport.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

// PureSheenDev render <in> <out> [options] (owner: plugin)
//
// Plays an audio file through PureSheenProcessor like a host would (fake playhead "playing", fixed block size), then
// writes the result with the processor latency removed (the output lines up sample for sample with the input and has
// its length). Options:
//   --preset <name>                 global preset (factory or user), case-insensitive
//   --module-preset <prefix>:<name> module preset, e.g. "max:Loud Modern"
//   --import <file.xml>             a foreign .xml preset through the preset importer (after the presets)
//   --chain <prefix,prefix,...>     chain order after the presets, e.g. "--chain deq" or "--chain eq1,lowc,max"
//   --bits 16|24|32                 output format (default 32 = float)
//   --block <n>                     host block size (default 512)
//   id=value                        parameter override after the presets, plain value; choices also by label
//                                   (e.g. max.mode=Aggressive), bools as on/off/true/false/1/0
// Prints integrated loudness, LRA, true peak and sample peak of input and output, the latency, and the peak
// difference output - input (the null test at neutral settings).
namespace
{
struct PlayingHead final : juce::AudioPlayHead
{
    long long time = 0;
    double sampleRate = 48000.0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (true);
        info.setBpm (120.0);
        info.setTimeInSamples ((juce::int64) time);
        info.setTimeInSeconds ((double) time / sampleRate);
        return info;
    }
};

struct Measurement
{
    float integrated = -200.0f, lra = 0.0f, truePeak = -200.0f, samplePeak = -200.0f;
};

Measurement measure (const juce::AudioBuffer<float>& b, double rate)
{
    ps::LoudnessMeter lm;
    lm.prepare (rate);
    ps::TruePeakMeter tp;
    tp.prepare (rate);

    const int n = b.getNumSamples();
    for (int start = 0; start < n; start += 4096)
    {
        const int len = juce::jmin (4096, n - start);
        lm.process (b.getReadPointer (0, start), b.getReadPointer (1, start), len);
        tp.process (b.getReadPointer (0, start), b.getReadPointer (1, start), len);
    }

    Measurement m;
    m.integrated = lm.integratedLufs();
    m.lra = lm.loudnessRange();
    m.truePeak = tp.truePeakDb();
    m.samplePeak = tp.samplePeakDb();
    return m;
}

std::string db (float v, const char* unit)
{
    std::ostringstream s;
    if (v <= -150.0f || ! std::isfinite (v))
        s << "-inf " << unit;
    else
        s << std::fixed << std::setprecision (2) << v << " " << unit;
    return s.str();
}

bool parseValue (const juce::RangedAudioParameter& p, const juce::String& text, float& plain)
{
    const auto t = text.trim();
    if (auto* choice = dynamic_cast<const juce::AudioParameterChoice*> (&p))
    {
        const int i = choice->choices.indexOf (t, true);
        if (i >= 0)
        {
            plain = (float) i;
            return true;
        }
    }
    if (dynamic_cast<const juce::AudioParameterBool*> (&p) != nullptr)
    {
        const auto l = t.toLowerCase();
        if (l == "on" || l == "true") { plain = 1.0f; return true; }
        if (l == "off" || l == "false") { plain = 0.0f; return true; }
    }
    if (t.containsOnly ("0123456789.-+eE") && t.isNotEmpty())
    {
        plain = t.getFloatValue();
        return true;
    }
    return false;
}

bool findPreset (PureSheenProcessor& proc, int scope, const juce::String& name, PureSheenProcessor::PresetInfo& out)
{
    for (const auto& p : proc.getPresets (scope))
        if (p.name.equalsIgnoreCase (name))
        {
            out = p;
            return true;
        }
    return false;
}
} // namespace

int runRender (const juce::File& input, const juce::File& output, const juce::StringArray& options)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (input));
    if (reader == nullptr)
    {
        std::cout << "render: cannot read " << input.getFullPathName() << "\n";
        return 1;
    }

    const double rate = reader->sampleRate;
    const auto length = (int) juce::jmin<juce::int64> (reader->lengthInSamples, std::numeric_limits<int>::max() / 2);
    const int fileChannels = (int) reader->numChannels;
    if (length <= 0 || fileChannels <= 0 || rate <= 0.0)
    {
        std::cout << "render: empty or invalid file\n";
        return 1;
    }

    juce::AudioBuffer<float> in (2, length);
    reader->read (&in, 0, length, 0, true, fileChannels > 1);
    if (fileChannels == 1)
        in.copyFrom (1, 0, in, 0, 0, length);

    // Options
    int bits = 32, block = 512;
    juce::String globalPreset, modulePreset, chainText, importPath;
    bool setChain = false;
    juce::StringArray overrides;
    for (int i = 0; i < options.size(); ++i)
    {
        const auto& o = options[i];
        if (o == "--preset" && i + 1 < options.size())
            globalPreset = options[++i];
        else if (o == "--module-preset" && i + 1 < options.size())
            modulePreset = options[++i];
        else if (o == "--import" && i + 1 < options.size())
            importPath = options[++i];
        else if (o == "--chain" && i + 1 < options.size())
        {
            chainText = options[++i];
            setChain = true;
        }
        else if (o == "--bits" && i + 1 < options.size())
            bits = options[++i].getIntValue();
        else if (o == "--block" && i + 1 < options.size())
            block = juce::jlimit (16, 65536, options[++i].getIntValue());
        else if (o.containsChar ('='))
            overrides.add (o);
        else
        {
            std::cout << "render: unknown option " << o << "\n";
            return 2;
        }
    }
    if (bits != 16 && bits != 24 && bits != 32)
    {
        std::cout << "render: --bits must be 16, 24 or 32\n";
        return 2;
    }

    PureSheenProcessor proc;
    PlayingHead head;
    head.sampleRate = rate;
    proc.setPlayHead (&head);
    proc.prepareToPlay (rate, block);

    if (globalPreset.isNotEmpty())
    {
        PureSheenProcessor::PresetInfo info;
        if (! findPreset (proc, -1, globalPreset, info) || ! proc.loadPreset (-1, info))
        {
            std::cout << "render: no global preset \"" << globalPreset << "\"\n";
            return 2;
        }
        std::cout << "preset: " << info.category << " / " << info.name << "\n";
    }

    if (modulePreset.isNotEmpty())
    {
        const auto prefix = modulePreset.upToFirstOccurrenceOf (":", false, false);
        const auto name = modulePreset.fromFirstOccurrenceOf (":", false, false);
        int scope = -1;
        for (int t = 0; t < ps::kNumModuleTypes; ++t)
            if (prefix == ps::moduleInfo ((ps::ModuleType) t).idPrefix)
                scope = t;
        PureSheenProcessor::PresetInfo info;
        if (scope < 0 || ! findPreset (proc, scope, name, info) || ! proc.loadPreset (scope, info))
        {
            std::cout << "render: no module preset \"" << modulePreset << "\"\n";
            return 2;
        }
        std::cout << "module preset: " << prefix << " / " << info.name << "\n";
    }

    if (importPath.isNotEmpty())
    {
        const auto result = psplug::importXmlPreset (proc, juce::File::getCurrentWorkingDirectory().getChildFile (importPath));
        std::cout << "import: " << result.message << "\n";
        if (! result.ok)
            return 2;
    }

    if (setChain)
    {
        std::vector<ps::ModuleType> order;
        for (const auto& token : juce::StringArray::fromTokens (chainText, ",", {}))
        {
            int type = -1;
            for (int t = 0; t < ps::kNumModuleTypes; ++t)
                if (token.trim() == ps::moduleInfo ((ps::ModuleType) t).idPrefix)
                    type = t;
            if (type < 0)
            {
                std::cout << "render: unknown module \"" << token << "\" in --chain\n";
                return 2;
            }
            order.push_back ((ps::ModuleType) type);
        }
        proc.setChainOrder (order);
    }

    for (const auto& o : overrides)
    {
        const auto id = o.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto text = o.fromFirstOccurrenceOf ("=", false, false);
        auto* p = proc.getParameter (id);
        float plain = 0.0f;
        if (p == nullptr || ! parseValue (*p, text, plain))
        {
            std::cout << "render: bad override \"" << o << "\" (unknown id or value)\n";
            return 2;
        }
        proc.setParameterValue (id, plain);
        std::cout << "set " << id << " = " << p->getCurrentValueAsText() << "\n";
    }
    proc.flushPendingChanges (true);

    // Play: the input, then zeros until the latency is flushed out.
    juce::AudioBuffer<float> buffer (2, block);
    juce::MidiBuffer midi;
    std::vector<float> outL, outR;
    outL.reserve ((size_t) length + 65536);
    outR.reserve ((size_t) length + 65536);

    int fed = 0, latency = -1, latencyChanges = 0;
    const double t0 = juce::Time::getMillisecondCounterHiRes();

    while (true)
    {
        if (latency >= 0 && (long long) outL.size() >= (long long) length + latency)
            break;

        const int n = block;
        for (int c = 0; c < 2; ++c)
        {
            auto* dst = buffer.getWritePointer (c);
            for (int i = 0; i < n; ++i)
                dst[i] = fed + i < length ? in.getSample (c, fed + i) : 0.0f;
        }
        proc.processBlock (buffer, midi);
        head.time += n;
        fed += n;
        for (int i = 0; i < n; ++i)
        {
            outL.push_back (buffer.getSample (0, i));
            outR.push_back (buffer.getSample (1, i));
        }

        proc.flushPendingChanges (true);
        const int l = proc.getCurrentLatencySamples();
        if (latency >= 0 && l != latency)
            ++latencyChanges;
        if (fed >= length || latency < 0)
            latency = l;
    }

    const double seconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;

    juce::AudioBuffer<float> out (2, length);
    for (int i = 0; i < length; ++i)
    {
        out.setSample (0, i, outL[(size_t) (i + latency)]);
        out.setSample (1, i, outR[(size_t) (i + latency)]);
    }

    // Write
    output.getParentDirectory().createDirectory();
    output.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (new juce::FileOutputStream (output));
    juce::WavAudioFormat wav;
    auto writerOptions = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (bits);
    if (bits == 32)
        writerOptions = writerOptions.withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = wav.createWriterFor (stream, writerOptions);
    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (out, 0, length))
    {
        std::cout << "render: cannot write " << output.getFullPathName() << "\n";
        return 1;
    }
    writer.reset();

    // Report
    const auto mi = measure (in, rate), mo = measure (out, rate);
    float diff = 0.0f;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < length; ++i)
            diff = juce::jmax (diff, std::abs (out.getSample (c, i) - in.getSample (c, i)));

    std::cout << "render: " << input.getFileName() << " -> " << output.getFullPathName() << "\n"
              << "  " << length << " samples at " << rate << " Hz, block " << block << ", " << bits << " bit, "
              << std::fixed << std::setprecision (2) << seconds << " s (" << (seconds > 0 ? length / rate / seconds : 0.0)
              << "x realtime)\n"
              << "  latency " << latency << " samples" << (latencyChanges > 0 ? " (CHANGED during the render)" : "")
              << "\n"
              << "  chain:";
    for (auto t : proc.getChainOrder())
        std::cout << " " << ps::moduleInfo (t).shortName;
    std::cout << "\n";
    if (proc.isModuleInChain (ps::ModuleType::Maximizer) && proc.getParameterValue ("max.on") > 0.5f)
        std::cout << "  maximizer output level " << std::fixed << std::setprecision (2)
                  << proc.getParameterValue ("max.ceiling") << " dB, true peak "
                  << (proc.getParameterValue ("max.truePeak") > 0.5f ? "on" : "off") << "\n";
    std::cout << "  input : integrated " << db (mi.integrated, "LUFS") << ", LRA " << std::setprecision (2) << mi.lra
              << " LU, true peak " << db (mi.truePeak, "dBTP") << ", sample peak " << db (mi.samplePeak, "dBFS") << "\n"
              << "  output: integrated " << db (mo.integrated, "LUFS") << ", LRA " << mo.lra << " LU, true peak "
              << db (mo.truePeak, "dBTP") << ", sample peak " << db (mo.samplePeak, "dBFS") << "\n"
              << "  difference output - input: peak "
              << db (diff > 0.0f ? 20.0f * std::log10 (diff) : -200.0f, "dBFS") << "\n";
    return 0;
}
