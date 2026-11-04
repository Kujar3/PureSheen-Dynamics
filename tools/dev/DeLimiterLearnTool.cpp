#include "dsp/DeLimiter.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <iostream>
#include <memory>
#include <vector>

// PureSheenDev delimitlearn <file.wav> [inputGainDb]: runs the De-limiter's Learn Threshold over a file, from its start, as
// it runs while a host plays, and prints the result. For the fit against the reference's learned thresholds.
int runDeLimiterLearn (const juce::StringArray& args)
{
    if (args.isEmpty())
    {
        std::cout << "usage: PureSheenDev delimitlearn <file.wav> [inputGainDb]\n";
        return 2;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File::getCurrentWorkingDirectory().getChildFile (args[0])));
    if (reader == nullptr)
    {
        std::cout << "cannot read " << args[0] << "\n";
        return 1;
    }

    std::vector<float> values;
    for (const auto& s : ps::paramSpecs())
        values.push_back (s.defaultValue);
    if (args.size() > 1)
        values[(size_t) ps::paramIndex ("dlim.input")] = args[1].getFloatValue();

    constexpr int kBlock = 512;
    ps::DeLimiter module;
    module.prepare (reader->sampleRate, kBlock);
    module.setParams (values.data());
    module.startLearn();

    juce::AudioBuffer<float> buffer (2, kBlock);
    ps::ProcessContext context;
    float threshold = 0.0f;
    bool done = false;
    for (juce::int64 at = 0; at < reader->lengthInSamples && ! done; at += kBlock)
    {
        const int n = (int) juce::jmin ((juce::int64) kBlock, reader->lengthInSamples - at);
        reader->read (&buffer, 0, n, at, true, true);
        if (reader->numChannels < 2)
            buffer.copyFrom (1, 0, buffer, 0, 0, n);
        module.setParams (values.data());
        module.process (buffer.getArrayOfWritePointers(), n, context);
        done = module.takeLearnedThreshold (threshold);
    }

    if (! done)
    {
        std::cout << "not finished: the file ends before the 6 s (progress " << module.getLearnProgress() << ")\n";
        return 1;
    }
    std::cout << juce::String (threshold, 8) << "\n";
    return 0;
}
