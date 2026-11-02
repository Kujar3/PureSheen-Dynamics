#include "assist/AssistFeatures.h"
#include "assist/OfflineChain.h"
#include "dsp/Params.h"
#include "dsp/ToneBalance.h"
#include "dsp/Types.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <iomanip>
#include <iostream>
#include <sstream>

// PureSheenDev assist ... (owner: assistant). Fitting and checking the Assistant from the command line.
//   assist <in.wav> --features [--start <s>] [--seconds <s>]   JSON: profile, octave curve, the four processing bands,
//                                                              the sub-band grid summary
//   assist --targets                                            JSON: every tone target curve at the curve points
//   assist <in.wav> [--start s] [--seconds s] [--profile <n>] [--loudness <lufs>] [--intensity 0|1|2]
//          [--excerpt <s>] [--suggestion <k>] [--render <out.wav>] [--tables] [--all]
//       builds the suggestions from (part of) the file, refines the Maximizer gain on the loudest --excerpt seconds
//       (0 = all, the default) for suggestion k (default 0; every one with --all), builds k's loudness tables with
//       --tables, prints every suggestion's chain and k's values that differ from our defaults (`id = value`, like
//       `import --values`); --render renders suggestion k with `render`.
int runRender (const juce::File& input, const juce::File& output, const juce::StringArray& options);   // Render.cpp

namespace
{
std::string num (double v)
{
    std::ostringstream s;
    s << std::setprecision (6) << v;
    return s.str();
}

void printBand (std::ostream& o, const ps::assist::BandFeatures& b)
{
    o << "{\"level\":" << num (b.levelDb) << ",\"p50\":" << num (b.p50Db) << ",\"p90\":" << num (b.p90Db)
      << ",\"p99\":" << num (b.p99Db) << ",\"width\":" << num (b.widthPct) << ",\"corr\":" << num (b.correlation)
      << ",\"onset\":" << num (b.onsetDb) << ",\"crest\":" << num (b.crestDb) << "}";
}

void printCurve (std::ostream& o, const ps::Curve& c)
{
    o << "[";
    for (int i = 0; i < ps::kCurvePoints; ++i)
        o << (i > 0 ? "," : "") << num (c[(size_t) i]);
    o << "]";
}

bool readAudio (const juce::File& file, double startSeconds, double seconds, juce::AudioBuffer<float>& out, double& rate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->numChannels == 0)
        return false;

    rate = reader->sampleRate;
    const auto start = (juce::int64) juce::jmax (0.0, startSeconds * rate);
    auto length = reader->lengthInSamples - start;
    if (seconds > 0.0)
        length = juce::jmin (length, (juce::int64) (seconds * rate));
    if (length <= 0)
        return false;

    out.setSize (2, (int) length);
    reader->read (&out, 0, (int) length, start, true, reader->numChannels > 1);
    if (reader->numChannels == 1)
        out.copyFrom (1, 0, out, 0, 0, (int) length);
    return true;
}

int printFeatures (const juce::File& file, double start, double seconds)
{
    juce::AudioBuffer<float> audio;
    double rate = 0.0;
    if (! readAudio (file, start, seconds, audio, rate))
    {
        std::cout << "assist: cannot read " << file.getFullPathName() << "\n";
        return 1;
    }

    const auto a = ps::assist::analyse (audio.getReadPointer (0), audio.getReadPointer (1), audio.getNumSamples(), rate);
    const auto& p = a.profile;
    auto& o = std::cout;
    o << "{\"valid\":" << (a.valid ? "true" : "false") << ",\"seconds\":" << num (a.seconds)
      << ",\"lufs\":" << num (p.integratedLufs) << ",\"lra\":" << num (p.loudnessRangeLu)
      << ",\"truePeak\":" << num (p.truePeakDb) << ",\"plr\":" << num (p.plrDb) << ",\"crest\":" << num (p.crestDb)
      << ",\"width\":" << num (p.widthPct) << ",\"corr\":" << num (p.correlation)
      << ",\"lowCorr\":" << num (p.lowEndCorrelation) << ",\"bands6\":[";
    for (int b = 0; b < ps::kNumBands; ++b)
        o << (b > 0 ? "," : "") << "{\"level\":" << num (p.bands[(size_t) b].levelDb) << ",\"width\":"
          << num (p.bands[(size_t) b].widthPct) << ",\"corr\":" << num (p.bands[(size_t) b].correlation) << "}";
    o << "],\"proc\":[";
    for (int b = 0; b < ps::assist::kProcBands; ++b)
    {
        o << (b > 0 ? "," : "");
        printBand (o, a.procBands[(size_t) b]);
    }
    o << "],\"sub\":[";
    for (int b = 0; b < ps::assist::kSubBands; ++b)
    {
        o << (b > 0 ? "," : "");
        printBand (o, ps::assist::bandFeatures (a, ps::assist::subBandLowHz (b), ps::assist::subBandLowHz (b + 1)));
    }
    o << "],\"curveHz\":[";
    for (int i = 0; i < ps::kCurvePoints; ++i)
        o << (i > 0 ? "," : "") << num (ps::curveFrequency (i));
    o << "],\"curve\":";
    printCurve (o, p.curveDb);
    o << ",\"octave\":";
    printCurve (o, a.octaveDb);
    o << "}\n";
    return 0;
}

int printTargets()
{
    auto& o = std::cout;
    o << "{";
    for (int t = 0; t < (int) ps::ToneTarget::Count; ++t)
    {
        o << (t > 0 ? "," : "") << "\"" << t << "\":{\"db\":[";
        for (int i = 0; i < ps::kCurvePoints; ++i)
            o << (i > 0 ? "," : "") << num (ps::toneTargetDb ((ps::ToneTarget) t, ps::curveFrequency (i)));
        o << "],\"tol\":[";
        for (int i = 0; i < ps::kCurvePoints; ++i)
        {
            double tol = 0.0;
            ps::toneTargetDb ((ps::ToneTarget) t, ps::curveFrequency (i), &tol);
            o << (i > 0 ? "," : "") << num (tol);
        }
        o << "]}";
    }
    o << "}\n";
    return 0;
}
int runSuggestions (const juce::File& file, double start, double seconds, const juce::StringArray& args)
{
    juce::AudioBuffer<float> audio;
    double rate = 0.0;
    if (! readAudio (file, start, seconds, audio, rate))
    {
        std::cout << "assist: cannot read " << file.getFullPathName() << "\n";
        return 1;
    }

    ps::assist::Context ctx;
    double excerptSeconds = 0.0;
    int which = 0;
    bool tables = false;
    juce::String renderPath;
    for (int i = 0; i < args.size(); ++i)
    {
        const auto& a = args[i];
        const bool hasNext = i + 1 < args.size();
        if (a == "--profile" && hasNext)
            ctx.settings.profile = args[++i].getIntValue();
        else if (a == "--loudness" && hasNext)
        {
            ctx.settings.autoLoudness = false;
            ctx.settings.loudnessLufs = args[++i].getFloatValue();
        }
        else if (a == "--intensity" && hasNext)
            ctx.settings.intensity = (ps::assist::Intensity) juce::jlimit (0, 2, args[++i].getIntValue());
        else if (a == "--excerpt" && hasNext)
            excerptSeconds = args[++i].getDoubleValue();
        else if (a == "--suggestion" && hasNext)
            which = args[++i].getIntValue();
        else if (a == "--render" && hasNext)
            renderPath = args[++i];
        else if (a == "--tables")
            tables = true;
    }

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    ctx.analysis = ps::assist::analyse (audio.getReadPointer (0), audio.getReadPointer (1), audio.getNumSamples(), rate);
    float distance = 0.0f;
    const auto detected = ps::assist::detectTarget (ctx.analysis, &distance);
    auto suggestions = ps::assist::buildSuggestions (ctx);
    const auto t1 = juce::Time::getMillisecondCounterHiRes();

    const auto excerpt = ps::assist::loudestExcerpt (audio.getReadPointer (0), audio.getReadPointer (1),
                                                     audio.getNumSamples(), rate,
                                                     excerptSeconds > 0.0 ? excerptSeconds : audio.getNumSamples() / rate);
    for (size_t k = 0; k < suggestions.size(); ++k)
        if ((int) k == which || args.contains ("--all"))
            ps::assist::refineLoudness (suggestions[k], excerpt);
    const auto t2 = juce::Time::getMillisecondCounterHiRes();
    if (tables && which >= 0 && which < (int) suggestions.size())
        ps::assist::buildLoudnessTables (suggestions[(size_t) which], excerpt, ctx.analysis);
    const auto t3 = juce::Time::getMillisecondCounterHiRes();

    std::cout << "assist: " << file.getFileName() << ", " << std::fixed << std::setprecision (1) << ctx.analysis.seconds
              << " s analysed, input " << ctx.analysis.profile.integratedLufs << " LUFS; detected "
              << ps::assist::targetName (detected) << " (distance " << std::setprecision (2) << distance << ")\n"
              << "  timing: analysis + rules " << std::setprecision (0) << (t1 - t0) << " ms, loudness "
              << (t2 - t1) << " ms, tables " << (t3 - t2) << " ms (excerpt " << std::setprecision (1)
              << (double) excerpt.size() / rate << " s)\n";

    const auto& specs = ps::paramSpecs();
    for (size_t k = 0; k < suggestions.size(); ++k)
    {
        const auto& s = suggestions[k];
        std::cout << "SUGGESTION " << k << " \"" << s.name << "\" (" << s.description << ") target "
                  << ps::assist::targetName (s.target) << " loudness " << std::setprecision (1) << s.loudnessLufs
                  << " gainPerLufs " << std::setprecision (2) << s.gainPerLufs << "\n  chain:";
        for (auto t : s.order)
            std::cout << " " << ps::moduleInfo (t).idPrefix;
        std::cout << "\n";
        if (tables && s.tablesReady)
            for (int m = 0; m < ps::assist::kNumMacros; ++m)
                if (ps::assist::macroInfo ((ps::assist::Macro) m).loudnessMatched)
                {
                    std::cout << "  table " << ps::assist::macroInfo ((ps::assist::Macro) m).id << ":";
                    for (int p = 0; p < ps::assist::Suggestion::kTablePoints; ++p)
                        std::cout << " " << num (s.tablePositions[(size_t) m][(size_t) p]) << "/"
                                  << num (s.tableDeltaLu[(size_t) m][(size_t) p]);
                    std::cout << "\n";
                }
        if ((int) k != which)
            continue;
        for (size_t i = 0; i < specs.size(); ++i)
            if (s.values[i] != specs[i].defaultValue)
                std::cout << "     " << specs[i].id << " = " << num (s.values[i]) << "\n";
    }

    if (renderPath.isNotEmpty() && which >= 0 && which < (int) suggestions.size())
    {
        const auto& s = suggestions[(size_t) which];
        juce::StringArray chain, options;
        for (auto t : s.order)
            chain.add (ps::moduleInfo (t).idPrefix);
        options.add ("--chain");
        options.add (chain.joinIntoString (","));
        for (size_t i = 0; i < specs.size(); ++i)
            if (s.values[i] != specs[i].defaultValue)
                options.add (juce::String (specs[i].id) + "=" + juce::String (s.values[i], 6));
        options.add ("--bits");
        options.add ("32");
        return runRender (file, juce::File::getCurrentWorkingDirectory().getChildFile (renderPath), options);
    }
    return 0;
}
} // namespace

int runAssist (const juce::StringArray& args)
{
    if (args.contains ("--targets"))
        return printTargets();

    juce::String input;
    double start = 0.0, seconds = 0.0;
    bool features = false;
    for (int i = 0; i < args.size(); ++i)
    {
        const auto& a = args[i];
        if (a == "--features")
            features = true;
        else if (a == "--start" && i + 1 < args.size())
            start = args[++i].getDoubleValue();
        else if (a == "--seconds" && i + 1 < args.size())
            seconds = args[++i].getDoubleValue();
        else if (! a.startsWith ("--") && input.isEmpty())
            input = a;
        else if (a == "--profile" || a == "--loudness" || a == "--intensity" || a == "--excerpt" || a == "--suggestion"
                 || a == "--render")
            ++i;
        else if (a != "--tables" && a != "--all")
        {
            std::cout << "assist: unknown option " << a << "\n";
            return 2;
        }
    }

    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (input);
    if (features && input.isNotEmpty())
        return printFeatures (file, start, seconds);
    if (input.isNotEmpty())
        return runSuggestions (file, start, seconds, args);

    std::cout << "usage: PureSheenDev assist <in.wav> [--features] [--start s] [--seconds s] [--profile n] [--loudness l]\n"
                 "       [--intensity 0|1|2] [--excerpt s] [--suggestion k] [--render out.wav] [--tables] | --targets\n";
    return 2;
}
