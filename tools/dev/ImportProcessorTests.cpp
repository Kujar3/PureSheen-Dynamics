#include "dsp/MatchEq.h"
#include "plugin/MatchEqState.h"
#include "plugin/PluginProcessor.h"
#include "plugin/PluginState.h"
#include "plugin/import/XmlPresetImport.h"

#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>

namespace
{
juce::MemoryBlock savedState (PureSheenProcessor& processor)
{
    juce::MemoryBlock state;
    processor.getStateInformation (state);
    return state;
}

ps::MatchSpectrum referenceOf (PureSheenProcessor& processor, int part)
{
    return static_cast<ps::MatchEq&> (processor.getChain().getModule (ps::ModuleType::MatchEq, part))
        .getSpectrum (ps::MatchRole::Reference);
}

ps::MatchEq& matchModule (PureSheenProcessor& processor, int part)
{
    return static_cast<ps::MatchEq&> (processor.getChain().getModule (ps::ModuleType::MatchEq, part));
}

struct SaveDuringNotification final : juce::AudioProcessorParameter::Listener
{
    explicit SaveDuringNotification (PureSheenProcessor& p) : processor (p) {}
    void parameterValueChanged (int, float) override { saved.push_back (savedState (processor)); }
    void parameterGestureChanged (int, bool) override {}

    PureSheenProcessor& processor;
    std::vector<juce::MemoryBlock> saved;
};
} // namespace

int runImportProcessorTests()
{
    int failures = 0;
    auto check = [&failures] (bool ok, const char* name)
    {
        std::cout << (ok ? "  PASS " : "  FAIL ") << "import: " << name << "\n";
        if (! ok)
            ++failures;
    };

    std::cout << "== importer: transaction boundaries\n";
    PureSheenProcessor processor;
    const auto defaults = psplug::defaultValues();
    const auto defaultOrder = psplug::defaultChain();
    const int gainIndex = ps::paramIndex ("max.gain");
    const int amountIndex = ps::paramIndex ("meq.a.amount");
    auto values = defaults;
    values[(size_t) gainIndex] = 6.0f;
    values[(size_t) amountIndex] = 37.0f;
    const std::vector<ps::ModuleType> order { ps::ModuleType::MatchEq, ps::ModuleType::Maximizer };
    const auto reference = ps::makeMatchTiltSpectrum (-3.0, 0.0, 60.0, "import:Test target");

    const auto initial = savedState (processor);
    const auto initialHistory = processor.getHistory().size();
    bool malformedRejected = true;
    auto refuses = [&] (int scope, const std::vector<float>& v, const std::vector<ps::ModuleType>& o,
                        const ps::MatchSpectrum* r = nullptr)
    {
        const bool applied = processor.applyImportedPreset (scope, v, o, "Invalid request", r);
        malformedRejected = malformedRejected && ! applied && savedState (processor) == initial
                            && processor.getHistory().size() == initialHistory;
    };
    refuses (-2, values, order);
    refuses (ps::kNumModuleTypes, values, order);
    refuses (-1, {}, order);
    refuses (-1, values, { ps::ModuleType::MatchEq, ps::ModuleType::MatchEq });
    refuses (-1, values, { (ps::ModuleType) -1 });
    refuses (-1, values, { (ps::ModuleType) ps::kNumModuleTypes });
    refuses ((int) ps::ModuleType::Imager, values, order);
    for (const float invalid : { std::numeric_limits<float>::quiet_NaN(),
                                  std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max() })
    {
        auto bad = values;
        bad[(size_t) gainIndex] = invalid;
        refuses (-1, bad, order);
    }
    auto badReference = reference;
    badReference.psdDb[0][17] = std::numeric_limits<float>::quiet_NaN();
    refuses (-1, values, order, &badReference);
    badReference = reference;
    badReference.psdDb[1][18] = std::numeric_limits<float>::max();
    refuses (-1, values, order, &badReference);
    badReference = reference;
    badReference.sampleRate = std::numeric_limits<double>::infinity();
    refuses (-1, values, order, &badReference);
    badReference = reference;
    badReference.source.fill ('x');
    refuses (-1, values, order, &badReference);
    badReference = reference;
    badReference.source[0] = '\x7f';
    refuses (-1, values, order, &badReference);
    badReference = reference;
    badReference.valid = false;
    refuses (-1, values, order, &badReference);
    refuses ((int) ps::ModuleType::Maximizer, values, order, &reference);
    check (malformedRejected, "malformed requests are refused before state, references or history change");

    bool workerApplied = true, workerPlanApplied = true;
    std::thread worker ([&]
    {
        workerApplied = processor.applyImportedPreset (-1, values, order, "Worker", &reference);
        ps::imp::ImportPlan plan;
        plan.ok = true;
        plan.scope = (int) ps::ModuleType::MatchEq;
        plan.values = values;
        plan.order = { ps::ModuleType::MatchEq };
        workerPlanApplied = psplug::applyImportPlan (processor, std::move (plan), "Worker plan").ok;
    });
    worker.join();
    check (! workerApplied && ! workerPlanApplied && savedState (processor) == initial
               && processor.getHistory().size() == initialHistory,
           "worker-thread application is refused without changing state or history");

    const bool noChange = processor.applyImportedPreset (-1, defaults, defaultOrder, "Unchanged");
    processor.flushPendingChanges (true);
    check (noChange && processor.getHistory().size() == initialHistory + 1,
           "an accepted import with unchanged values still has exactly one undo step");

    const auto beforeReference = savedState (processor);
    SaveDuringNotification listener (processor);
    auto* gain = processor.getParameter ("max.gain");
    gain->addListener (&listener);
    const auto historyBeforeReference = processor.getHistory().size();
    const bool referenceApplied = processor.applyImportedPreset (-1, values, order, "With target", &reference);
    gain->removeListener (&listener);
    processor.flushPendingChanges (true);
    bool savesComplete = ! listener.saved.empty();
    for (const auto& state : listener.saved)
        savesComplete = savesComplete && state == beforeReference;
    check (referenceApplied && savesComplete,
           "host saves reentered by parameter notifications see the complete pre-import state");
    check (processor.getHistory().size() == historyBeforeReference + 1
               && psplug::sameSpectrum (referenceOf (processor, 0), reference)
               && psplug::sameSpectrum (referenceOf (processor, 1), reference),
           "parameters, chain and both reference spectra are one history entry");

    processor.undo();
    processor.flushPendingChanges (true);
    check (processor.getChainOrder() == defaultOrder && std::abs (processor.getParameterValue ("max.gain")) < 0.001f
               && ! referenceOf (processor, 0).valid && ! referenceOf (processor, 1).valid,
           "one undo restores all parameters, chain and both references together");
    processor.redo();
    processor.flushPendingChanges (true);
    const auto beforeRepeat = processor.getHistory().size();
    const bool repeated = processor.applyImportedPreset (-1, values, order, "With target", &reference);
    processor.flushPendingChanges (true);
    check (repeated && processor.getHistory().size() == beforeRepeat + 1,
           "reimporting the same reference and values adds exactly one step");

    auto secondValues = values;
    secondValues[(size_t) gainIndex] = 2.0f;
    secondValues[(size_t) amountIndex] = 71.0f;
    const auto secondReference = ps::makeMatchTiltSpectrum (-4.5, 0.0, 60.0, "import:Second target");
    const std::vector<ps::ModuleType> secondOrder { ps::ModuleType::Equalizer1, ps::ModuleType::MatchEq };
    processor.applyImportedPreset (-1, secondValues, secondOrder, "Second", &secondReference);
    const auto secondState = savedState (processor);
    processor.applyImportedPreset (-1, values, order, "With target", &reference);
    const auto firstState = savedState (processor);
    std::atomic<bool> saving { true };
    std::atomic<bool> started { false };
    bool concurrentComplete = true;
    int concurrentSaves = 0;
    std::thread saver ([&]
    {
        auto observe = [&]
        {
            const auto state = savedState (processor);
            concurrentComplete = concurrentComplete && (state == firstState || state == secondState);
            ++concurrentSaves;
        };
        observe();
        started.store (true);
        while (saving.load())
        {
            observe();
            std::this_thread::yield();
        }
    });
    while (! started.load())
        std::this_thread::yield();
    bool alternated = true;
    for (int i = 0; i < 12; ++i)
    {
        alternated = processor.applyImportedPreset (-1, secondValues, secondOrder, "Second", &secondReference) && alternated;
        alternated = processor.applyImportedPreset (-1, values, order, "With target", &reference) && alternated;
    }
    saving.store (false);
    saver.join();
    check (alternated && concurrentComplete && concurrentSaves > 0,
           "concurrent host saves contain a complete parameter, chain and reference transaction");

    std::cout << "== importer: reference-only transactions\n";
    for (const int scope : { -1, (int) ps::ModuleType::MatchEq })
    {
        PureSheenProcessor p;
        const std::vector<ps::ModuleType> matchOrder { ps::ModuleType::MatchEq };
        p.applyImportedPreset (-1, defaults, matchOrder, "Initial settings");
        const ps::MatchSpectrum preceding[2][2] {
            { ps::makeMatchTiltSpectrum (-2.0, 0.0, 60.0, "test:Reference A"),
              ps::makeMatchTiltSpectrum (-4.0, 0.0, 60.0, "test:Current A") },
            { ps::makeMatchTiltSpectrum (-2.5, 0.0, 60.0, "test:Reference B"),
              ps::makeMatchTiltSpectrum (-4.5, 0.0, 60.0, "test:Current B") }
        };
        for (int part = 0; part < 2; ++part)
            for (int role = 0; role < 2; ++role)
                matchModule (p, part).setSpectrum ((ps::MatchRole) role, preceding[part][role]);
        p.flushPendingChanges (true);

        auto spectraMatch = [&] (bool imported)
        {
            bool same = true;
            for (int part = 0; part < 2; ++part)
            {
                same = same && psplug::sameSpectrum (referenceOf (p, part),
                                                    imported ? reference : preceding[part][0]);
                same = same && psplug::sameSpectrum (matchModule (p, part).getSpectrum (ps::MatchRole::Current),
                                                    preceding[part][1]);
            }
            return same;
        };
        const auto before = p.getHistory().size();
        matchModule (p, 0).startCapture (ps::MatchRole::Reference);
        matchModule (p, 1).startCapture (ps::MatchRole::Current);
        const bool applied = p.applyImportedPreset (scope, defaults, matchOrder, "Only the target changes", &reference);
        p.flushPendingChanges (true);
        const bool capturesCorrect = ! matchModule (p, 0).isCapturing() && matchModule (p, 1).isCapturing();
        matchModule (p, 0).stopCapture();   // A delayed Stop from the old reference capture cannot erase the import.
        p.flushPendingChanges (true);
        check (applied && capturesCorrect && spectraMatch (true) && p.getHistory().size() == before + 1,
               scope < 0 ? "a whole-chain reference-only import preserves Current and is one step"
                         : "a module reference-only import preserves Current and is one step");
        p.undo();
        p.flushPendingChanges (true);
        check (spectraMatch (false) && ! matchModule (p, 0).isCapturing() && ! matchModule (p, 1).isCapturing(),
               scope < 0 ? "undo of a whole-chain reference-only import restores distinct previous parts"
                         : "undo of a module reference-only import restores distinct previous parts");
        p.redo();
        p.flushPendingChanges (true);
        check (spectraMatch (true) && p.getHistory().size() == before + 1,
               scope < 0 ? "redo restores both imported references and both Current spectra without a new step"
                         : "module redo restores both imported references and both Current spectra without a new step");

        // A spectrum edit that has not reached housekeeping belongs to the preceding history entry, not the import.
        const auto pending = ps::makeMatchTiltSpectrum (-5.0, 0.0, 60.0, "test:Pending reference");
        matchModule (p, 1).setSpectrum (ps::MatchRole::Reference, pending);
        const auto beforePending = p.getHistory().size();
        const bool pendingApplied = p.applyImportedPreset (scope, defaults, matchOrder, "After pending edit", &secondReference);
        p.flushPendingChanges (true);
        p.undo();
        p.flushPendingChanges (true);
        check (pendingApplied && p.getHistory().size() == beforePending + 2
                   && psplug::sameSpectrum (referenceOf (p, 0), reference)
                   && psplug::sameSpectrum (referenceOf (p, 1), pending)
                   && psplug::sameSpectrum (matchModule (p, 0).getSpectrum (ps::MatchRole::Current), preceding[0][1])
                   && psplug::sameSpectrum (matchModule (p, 1).getSpectrum (ps::MatchRole::Current), preceding[1][1]),
               scope < 0 ? "whole-chain import preserves an unpolled spectrum edit as its own undo step"
                         : "module import preserves an unpolled spectrum edit as its own undo step");
    }
    return failures;
}
