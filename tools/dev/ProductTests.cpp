#include "AllocationTracking.h"

#include "plugin/PluginProcessor.h"
#include "plugin/PluginState.h"
#include "ui/ChainStrip.h"
#include "ui/DitherPanel.h"
#include "ui/ModulePanel.h"
#include "ui/PluginEditor.h"

#include <cmath>
#include <iostream>
#include <set>
#include <sstream>

// PureSheenDev producttest [id]          the single-module plugins (installer/products.csv); also part of selftest
// PureSheenDev productsnapshot <outDir> [id]
//
// Every single-module plugin is the processor locked to one module. These checks cover what the lock promises: the
// parameter set, the chain, the state, the presets it shares with the suite, realtime safety, and that it sounds like
// the suite with only that module in the chain. The snapshots show each panel at the three window sizes.
namespace juce::detail
{
bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages);
}

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

int numPassed = 0, numFailed = 0;

void check (bool ok, const juce::String& name, const juce::String& detail = {})
{
    if (ok)
    {
        ++numPassed;
        std::cout << "PASS " << name << "\n";
    }
    else
    {
        ++numFailed;
        std::cout << "FAIL " << name << (detail.isEmpty() ? juce::String() : ": " + detail) << "\n";
    }
}

struct Noise
{
    juce::uint32 state = 1;
    float next() noexcept
    {
        state = state * 1664525u + 1013904223u;
        return (float) (state >> 8) / 8388608.0f - 1.0f;
    }
    // Music-like enough for the modules to act on: a low tone, a pulse and quiet noise.
    void fill (juce::AudioBuffer<float>& b, juce::int64& sample)
    {
        for (int i = 0; i < b.getNumSamples(); ++i, ++sample)
        {
            const double t = (double) sample / kRate;
            const double beat = std::fmod (t, 0.5);
            const float tone = (float) (0.3 * std::sin (juce::MathConstants<double>::twoPi * 110.0 * t)
                                        + 0.5 * std::exp (-beat * 14.0) * std::sin (juce::MathConstants<double>::twoPi * 60.0 * t));
            b.setSample (0, i, tone + 0.08f * next());
            b.setSample (1, i, tone + 0.08f * next());
        }
    }
};

juce::String prefixOf (const psplug::Product& p)
{
    return p.isDither() ? juce::String ("dith") : juce::String (ps::moduleInfo (p.module).idPrefix);
}

std::vector<ps::ModuleType> lockedOrder (const psplug::Product& p)
{
    return p.isDither() ? std::vector<ps::ModuleType>() : std::vector<ps::ModuleType> { p.module };
}

// A float parameter of the product that the test may move away from its default (not a crossover: they keep an order).
const ps::ParamSpec* movableFloat (const psplug::Product& p)
{
    const auto prefix = prefixOf (p);
    for (const auto& s : ps::paramSpecs())
    {
        const juce::String id (s.id);
        if (s.kind == ps::ParamKind::Float && id.startsWith (prefix + ".") && ! id.contains ("xover")
            && s.maxValue > s.minValue)
            return &s;
    }
    return nullptr;
}

float movedValue (const ps::ParamSpec& s) { return s.defaultValue + (s.maxValue - s.defaultValue) * 0.5f; }

bool nearlyEqual (float a, float b, const ps::ParamSpec& s)
{
    return std::abs (a - b) <= juce::jmax (1.0e-3f * (s.maxValue - s.minValue), s.interval);
}

void testParameters (const psplug::Product& product)
{
    const juce::String tag (product.id);
    PureSheenProcessor proc (product);
    const auto prefix = prefixOf (product);

    check (proc.getName() == product.name, tag + ": host name is " + product.name);

    std::set<juce::String> shown;
    for (auto* p : proc.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
            shown.insert (ranged->getParameterID());

    std::set<juce::String> expected;
    for (const auto& s : ps::paramSpecs())
    {
        const juce::String id (s.id);
        const bool own = id.startsWith (prefix + ".") && id != prefix + ".on" && ! (product.isDither() && id == "dith.dc")
                         && (product.isDither() || s.module == (int) product.module);
        // The I/O panel: every global parameter that is not the dither's. (Not by id prefix: its gains are in. and out.)
        if (own || (s.module < 0 && ! id.startsWith ("dith.")))
            expected.insert (id);
    }
    check (shown == expected, tag + ": the host sees the module's and the I/O parameters, nothing else",
           juce::String ((int) shown.size()) + " shown, " + juce::String ((int) expected.size()) + " expected");

    for (const auto* gain : { "in.gainL", "in.gainR", "out.gainL", "out.gainR" })
        check (proc.getParameter (gain) != nullptr, tag + ": the gain fader " + gain + " is a parameter");

    check (proc.getParameter (prefix + ".on") == nullptr, tag + ": the power switch is not a host parameter");
    check (proc.getParameterValue (prefix + ".on") >= 0.5f, tag + ": the module stays on");
    check (proc.getBypassParameter() != nullptr, tag + ": Bypass is the host's bypass");
}

void testChainLock (const psplug::Product& product)
{
    const juce::String tag (product.id);
    PureSheenProcessor proc (product);
    const auto order = lockedOrder (product);

    check (proc.getChainOrder() == order, tag + ": the chain is the one module");

    const auto other = product.module == ps::ModuleType::Tape ? ps::ModuleType::Imager : ps::ModuleType::Tape;
    proc.addModule (other);
    check (proc.getChainOrder() == order, tag + ": adding a module does nothing");
    if (! order.empty())
    {
        proc.removeModule (order.front());
        check (proc.getChainOrder() == order, tag + ": removing the module does nothing");
    }
    proc.setChainOrder ({ ps::kDefaultChain.begin(), ps::kDefaultChain.end() });
    check (proc.getChainOrder() == order, tag + ": setting a chain does nothing");
    check (! proc.canUndo(), tag + ": none of that made an undo step");
}

void testState (const psplug::Product& product)
{
    const juce::String tag (product.id);
    const auto* spec = movableFloat (product);
    check (spec != nullptr, tag + ": has a float parameter to test with");
    if (spec == nullptr)
        return;

    const juce::String id (spec->id);
    const float value = movedValue (*spec);

    juce::MemoryBlock saved;
    {
        PureSheenProcessor proc (product);
        proc.setParameterValue (id, value);
        proc.setParameterValue ("io.mono", 1.0f);
        proc.storeSnapshot (1);
        proc.setParameterValue (id, spec->defaultValue);
        check (proc.canUndo(), tag + ": a change is an undo step");
        proc.undo();
        check (nearlyEqual (proc.getParameterValue (id), value, *spec), tag + ": undo restores the value");
        proc.getStateInformation (saved);
    }

    PureSheenProcessor restored (product);
    restored.setStateInformation (saved.getData(), (int) saved.getSize());
    check (nearlyEqual (restored.getParameterValue (id), value, *spec), tag + ": the state restores the module's parameter");
    check (restored.getParameterValue ("io.mono") >= 0.5f, tag + ": the state restores the I/O parameters");
    check (restored.hasSnapshot (1), tag + ": the state restores the snapshots");
    check (restored.getChainOrder() == lockedOrder (product), tag + ": the state keeps the chain");

    restored.recallSnapshot (1);
    check (nearlyEqual (restored.getParameterValue (id), value, *spec), tag + ": a snapshot recalls");

    // A state that names other modules (hand-made, or the suite's) cannot change the lock.
    const auto xml = juce::String ("<PureSheen version=\"1\"><Chain><Module type=\"max\"/><Module type=\"tape\"/></Chain></PureSheen>");
    restored.setStateInformation (xml.toRawUTF8(), (int) xml.getNumBytesAsUTF8());
    check (restored.getChainOrder() == lockedOrder (product), tag + ": a state with another chain cannot change the lock");
}

void testPresets (const psplug::Product& product, const juce::File& root)
{
    const juce::String tag (product.id);
    PureSheenProcessor single (product);
    single.setPresetRoot (root);

    if (product.isDither())
    {
        check (single.getPresets (-1).empty() && ! single.savePreset (-1, "No"), tag + ": no presets");
        return;
    }

    const int scope = (int) product.module;
    check (product.presetScope() == scope, tag + ": the preset bar works on the module");
    check (single.getPresets (-1).empty() && ! single.savePreset (-1, "No"), tag + ": no global presets");
    const int otherScope = scope == (int) ps::ModuleType::Tape ? (int) ps::ModuleType::Imager : (int) ps::ModuleType::Tape;
    check (single.getPresets (otherScope).empty() && ! single.savePreset (otherScope, "No"), tag + ": no presets of other modules");

    const auto* spec = movableFloat (product);
    const juce::String id (spec->id);
    const float value = movedValue (*spec);

    auto findPreset = [] (const std::vector<PureSheenProcessor::PresetInfo>& list, const juce::String& name) -> const PureSheenProcessor::PresetInfo*
    {
        for (const auto& p : list)
            if (p.name == name)
                return &p;
        return nullptr;
    };

    PureSheenProcessor suite;
    suite.setPresetRoot (root);

    // Suite -> single
    const juce::String fromSuite ("From suite " + tag);
    suite.setParameterValue (id, value);
    check (suite.savePreset (scope, fromSuite), tag + ": the suite saves a module preset");
    const auto listed = single.getPresets (scope);
    const auto* inSingle = findPreset (listed, fromSuite);
    check (inSingle != nullptr, tag + ": the plugin lists the suite's module preset");
    if (inSingle != nullptr)
    {
        check (single.loadPreset (scope, *inSingle), tag + ": the plugin loads it");
        check (nearlyEqual (single.getParameterValue (id), value, *spec), tag + ": with its value");
        check (single.getCurrentPresetName() == fromSuite && ! single.isPresetModified(), tag + ": the preset bar shows its name");
        single.setParameterValue ("io.swap", 1.0f);
        single.setParameterValue ("out.gainL", -3.0f);
        check (! single.isPresetModified(), tag + ": the I/O panel is not part of the preset");
        single.setParameterValue ("io.swap", 0.0f);
        single.setParameterValue ("out.gainL", 0.0f);
        single.setParameterValue (id, spec->defaultValue);
        check (single.isPresetModified(), tag + ": a change shows as modified");
    }

    // Single -> suite
    const juce::String fromSingle ("From plugin " + tag);
    single.setParameterValue (id, value);
    check (single.savePreset (scope, fromSingle), tag + ": the plugin saves a preset");
    check (single.getCurrentPresetName() == fromSingle && ! single.isPresetModified(), tag + ": the preset bar follows the save");
    suite.setParameterValue (id, spec->defaultValue);
    const auto inSuiteList = suite.getPresets (scope);
    const auto* inSuite = findPreset (inSuiteList, fromSingle);
    check (inSuite != nullptr, tag + ": the suite lists the plugin's preset");
    if (inSuite != nullptr)
    {
        check (suite.loadPreset (scope, *inSuite), tag + ": the suite loads it");
        check (nearlyEqual (suite.getParameterValue (id), value, *spec), tag + ": with its value");
        check (suite.getCurrentPresetName() == "Default", tag + ": as a module preset in the suite (the global name stays)");
    }

    // The arrows walk the module's list.
    const auto before = single.getCurrentPresetName();
    single.loadNextPreset (1);
    check (listed.size() < 2 || single.getCurrentPresetName() != before, tag + ": the arrows change the preset");

    // Other parameters survive a module preset.
    single.setParameterValue ("io.mono", 1.0f);
    single.loadNextPreset (1);
    check (single.getParameterValue ("io.mono") >= 0.5f, tag + ": a preset leaves the I/O panel alone");

    if (product.module == ps::ModuleType::Equalizer1)
    {
        const auto eq2Id = id.replaceFirstOccurrenceOf ("eq1.", "eq2.");
        suite.setParameterValue (eq2Id, value);
        check (suite.savePreset ((int) ps::ModuleType::Equalizer2, "From EQ 2"), tag + ": the suite saves an EQ 2 preset");
        const auto list = single.getPresets (scope);
        const auto* eq2 = findPreset (list, "From EQ 2");
        check (eq2 != nullptr, tag + ": the plugin also lists the suite's EQ 2 presets");
        if (eq2 != nullptr)
        {
            single.setParameterValue (id, spec->defaultValue);
            check (single.loadPreset (scope, *eq2) && nearlyEqual (single.getParameterValue (id), value, *spec),
                   tag + ": and loads them");
        }

        // A name in both folders is listed once, the EQ 1 one, so Delete by name can only mean that file.
        suite.savePreset ((int) ps::ModuleType::Equalizer1, "Same name");
        suite.savePreset ((int) ps::ModuleType::Equalizer2, "Same name");
        int count = 0;
        juce::File sameNameFile;
        for (const auto& p : single.getPresets (scope))
            if (p.name == "Same name")
            {
                ++count;
                sameNameFile = p.file;
            }
        check (count == 1 && sameNameFile.getParentDirectory().getFileName() == "eq1",
               tag + ": a name in both EQ folders is listed once, the EQ 1 preset");
    }

    // Imports: this module only.
    auto values = psplug::defaultValues();
    values[(size_t) ps::paramIndex (id.toStdString())] = value;
    check (! single.applyImportedPreset (-1, values, { ps::kDefaultChain.begin(), ps::kDefaultChain.end() }, "No"),
           tag + ": a whole-chain import is refused");
    check (! single.applyImportedPreset (otherScope, values, { (ps::ModuleType) otherScope }, "No"), tag + ": another module's import is refused");
    check (single.getChainOrder() == lockedOrder (product), tag + ": the refused imports left the chain alone");
    check (single.applyImportedPreset (scope, values, lockedOrder (product), "Imported")
               && single.getCurrentPresetName() == "Imported",
           tag + ": the module's import works and names the preset");
}

void testAudio (const psplug::Product& product)
{
    const juce::String tag (product.id);
    PureSheenProcessor single (product);
    PureSheenProcessor suite;

    suite.setChainOrder (lockedOrder (product));
    if (product.isDither())
    {
        suite.setParameterValue ("dith.on", 1.0f);
    }
    else
    {
        // The first factory preset, or the defaults: something the module acts on.
        const auto presets = single.getPresets ((int) product.module);
        for (const auto& p : presets)
            if (p.factory)
            {
                single.loadPreset ((int) product.module, p);
                suite.loadPreset ((int) product.module, p);
                break;
            }
    }

    single.prepareToPlay (kRate, kBlock);
    suite.prepareToPlay (kRate, kBlock);
    check (single.getLatencySamples() == suite.getLatencySamples(), tag + ": the same latency as the suite with only this module",
           juce::String (single.getLatencySamples()) + " against " + juce::String (suite.getLatencySamples()));

    juce::AudioBuffer<float> a (2, kBlock), b (2, kBlock);
    juce::MidiBuffer midi;
    Noise noiseA, noiseB;
    juce::int64 sampleA = 0, sampleB = 0;
    double sumDiff = 0.0, sumLevel = 0.0, peakDiff = 0.0;
    bool finite = true;
    const auto saved = psdev::getAllocationCounts();

    for (int block = 0; block < (int) (3.0 * kRate / kBlock); ++block)
    {
        noiseA.fill (a, sampleA);
        noiseB.fill (b, sampleB);
        psdev::setAllocationTracking (true);
        single.processBlock (a, midi);
        psdev::setAllocationTracking (false);
        suite.processBlock (b, midi);
        if (block < (int) (kRate / kBlock))
            continue;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i)
            {
                const float x = a.getSample (c, i), y = b.getSample (c, i);
                finite = finite && std::isfinite (x);
                sumDiff += (double) ((x - y) * (x - y));
                sumLevel += (double) (y * y);
                peakDiff = juce::jmax (peakDiff, (double) std::abs (x - y));
            }
    }

    const auto after = psdev::getAllocationCounts();
    check (after.allocations == saved.allocations && after.frees == saved.frees, tag + ": no allocation in processBlock");
    check (finite, tag + ": the output is finite");
    check (sumLevel > 1.0e-6, tag + ": the output is not silent");
    const double diffDb = 10.0 * std::log10 ((sumDiff + 1.0e-30) / (sumLevel + 1.0e-30));
    check (diffDb < -90.0, tag + ": sounds the same as the suite with only this module",
           juce::String (diffDb, 1) + " dB difference, peak " + juce::String (peakDiff, 7));
}

void testBypass (const psplug::Product& product)
{
    const juce::String tag (product.id);
    PureSheenProcessor proc (product);
    if (product.isDither())
        proc.setParameterValue ("dith.bits", 2.0f);   // 16 bit
    proc.setParameterValue ("io.bypass", 1.0f);
    proc.prepareToPlay (kRate, kBlock);
    const int latency = proc.getLatencySamples();

    const int total = (int) (3.0 * kRate);
    std::vector<float> in ((size_t) total), out ((size_t) total);
    juce::AudioBuffer<float> buffer (2, kBlock);
    juce::MidiBuffer midi;
    Noise noise;
    juce::int64 sample = 0;
    for (int pos = 0; pos + kBlock <= total; pos += kBlock)
    {
        noise.fill (buffer, sample);
        for (int i = 0; i < kBlock; ++i)
            in[(size_t) (pos + i)] = buffer.getSample (0, i);
        proc.processBlock (buffer, midi);
        for (int i = 0; i < kBlock; ++i)
            out[(size_t) (pos + i)] = buffer.getSample (0, i);
    }

    double peak = 0.0;
    for (int i = total / 2; i + kBlock < total; ++i)
        peak = juce::jmax (peak, (double) std::abs (out[(size_t) i] - in[(size_t) (i - latency)]));
    check (peak < 1.0e-5, tag + ": Bypass passes the input, latency aligned", "peak difference " + juce::String (peak, 7));

    {
        // The gain faders act on the audio: -6 dB on the output, left channel only, against the right one.
        PureSheenProcessor gained (product);
        gained.setParameterValue ("out.gainL", -6.0f);
        gained.prepareToPlay (kRate, kBlock);
        Noise same;
        juce::int64 at = 0;
        double left = 0.0, right = 0.0;
        for (int block = 0; block < (int) (3.0 * kRate / kBlock); ++block)
        {
            same.fill (buffer, at);
            buffer.copyFrom (1, 0, buffer, 0, 0, kBlock);   // the same signal on both channels
            gained.processBlock (buffer, midi);
            if (block > (int) (kRate / kBlock))
            {
                left += (double) buffer.getRMSLevel (0, 0, kBlock);
                right += (double) buffer.getRMSLevel (1, 0, kBlock);
            }
        }
        const double db = 20.0 * std::log10 ((left + 1.0e-12) / (right + 1.0e-12));
        check (std::abs (db + 6.0) < 0.5, tag + ": the output gain fader changes the level", juce::String (db, 2) + " dB");
    }

    if (product.isDither())
    {
        proc.setParameterValue ("io.bypass", 0.0f);
        proc.flushPendingChanges (true);
        double changed = 0.0;
        for (int block = 0; block < 100; ++block)
        {
            noise.fill (buffer, sample);
            juce::AudioBuffer<float> dry;
            dry.makeCopyOf (buffer);
            proc.processBlock (buffer, midi);
            if (block > 50)
                for (int i = 0; i < kBlock; ++i)
                    changed = juce::jmax (changed, (double) std::abs (buffer.getSample (0, i) - dry.getSample (0, i)));
        }
        check (changed > 1.0e-6 && changed < 1.0e-2, tag + ": with Bypass off it dithers", "largest change " + juce::String (changed, 7));
    }
}

template <typename T>
T* findChild (juce::Component& root)
{
    if (auto* t = dynamic_cast<T*> (&root))
        return t;
    for (auto* c : root.getChildren())
        if (auto* t = findChild<T> (*c))
            return t;
    return nullptr;
}

void testEditor (const psplug::Product& product)
{
    const juce::String tag (product.id);
    PureSheenProcessor proc (product);
    proc.prepareToPlay (kRate, kBlock);
    std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
    auto* editor = dynamic_cast<PureSheenEditor*> (holder.get());
    check (editor != nullptr, tag + ": the editor opens");
    if (editor == nullptr)
        return;

    check (findChild<ps::ui::ChainStrip> (*editor) == nullptr, tag + ": no chain strip");
    const int want = product.isDither() ? -1 : (int) product.module;
    check (editor->getSelectedModule() == want, tag + ": shows its own panel");
    editor->selectModule ((int) ps::ModuleType::Tape);
    check (editor->getSelectedModule() == want, tag + ": and no other");
    if (product.isDither())
    {
        auto* panel = findChild<ps::ui::DitherPanel> (*editor);
        check (panel != nullptr && panel->isVisible(), tag + ": the Dither panel is visible");
    }
    else
    {
        auto* panel = findChild<ps::ui::ModulePanel> (*editor);
        check (panel != nullptr && panel->getModuleType() == product.module, tag + ": the module panel is there");
    }

    check (editor->getHeight() == PureSheenEditor::defaultHeightFor (product), tag + ": the window starts at its default size");
    editor->setHelpVisible (true);
    editor->updateNow();
    editor->setHelpVisible (false);
    editor->setSize (PureSheenEditor::kMinWidth, PureSheenEditor::minHeightFor (product));
    editor->updateNow();

    // A saved window size and panel come back, and a host's stale selection cannot change the panel.
    juce::MemoryBlock saved;
    proc.getStateInformation (saved);
    holder.reset();
    proc.setStateInformation (saved.getData(), (int) saved.getSize());
    holder.reset (proc.createEditor());
    auto* again = dynamic_cast<PureSheenEditor*> (holder.get());
    check (again != nullptr && again->getSelectedModule() == want, tag + ": reopening shows its panel");
    check (again != nullptr && again->getHeight() == PureSheenEditor::minHeightFor (product), tag + ": at the saved size");
}

void testSuite()
{
    PureSheenProcessor suite;
    check (suite.getName() == "PureSheen Dynamics", "suite: the host name is unchanged");
    check ((int) suite.getParameters().size() == (int) ps::paramSpecs().size(), "suite: every parameter is a host parameter");
    check (&suite.getProduct() == &psplug::suiteProduct() && psplug::suiteProduct().isSuite(), "suite: the default product");
    check (suite.getChainOrder() == std::vector<ps::ModuleType> (ps::kDefaultChain.begin(), ps::kDefaultChain.end()),
           "suite: the default chain is unchanged");

    std::set<int> modules;
    int singles = 0;
    for (const auto& p : psplug::allProducts())
        if (p.kind == psplug::ProductKind::module)
        {
            modules.insert ((int) p.module);
            ++singles;
        }
    check ((int) modules.size() == singles, "products: no module has two plugins");
    for (int t = 0; t < ps::kNumModuleTypes; ++t)
        check (t == (int) ps::ModuleType::Equalizer2 || modules.count (t) == 1,
               juce::String ("products: ") + ps::moduleInfo ((ps::ModuleType) t).name + " has a plugin");
}
} // namespace

int runProductTests (const juce::StringArray& args)
{
    psdev::installHeapHooks();
    numPassed = numFailed = 0;

    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("PureSheenProductTest-" + juce::Uuid().toDashedString());
    root.createDirectory();

    const auto start = juce::Time::getMillisecondCounterHiRes();
    if (args.isEmpty())
        testSuite();

    for (const auto& product : psplug::allProducts())
    {
        if (product.isSuite() || (! args.isEmpty() && ! args.contains (product.id)))
            continue;
        std::cout << "\n-- " << product.name << "\n";
        testParameters (product);
        testChainLock (product);
        testState (product);
        testPresets (product, root);
        testAudio (product);
        testBypass (product);
        testEditor (product);
    }

    root.deleteRecursively();
    std::cout << "\nproducttest: " << numPassed << " passed, " << numFailed << " failed ("
              << juce::String ((juce::Time::getMillisecondCounterHiRes() - start) / 1000.0, 1) << " s)\n";
    return numFailed;
}

//==============================================================================
int runProductSnapshots (const juce::File& outDir, const juce::StringArray& args)
{
    if (! outDir.createDirectory())
        return 1;

    for (const auto& product : psplug::allProducts())
    {
        if (product.isSuite() || (! args.isEmpty() && ! args.contains (product.id)))
            continue;

        PureSheenProcessor proc (product);
        proc.prepareToPlay (kRate, 1600);
        std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
        auto* editor = dynamic_cast<PureSheenEditor*> (holder.get());
        if (editor == nullptr)
            return 1;

        juce::AudioBuffer<float> buffer (2, 1600);
        juce::MidiBuffer midi;
        Noise noise;
        juce::int64 sample = 0;
        auto play = [&] (int ticks)
        {
            for (int i = 0; i < ticks; ++i)
            {
                noise.fill (buffer, sample);
                proc.processBlock (buffer, midi);
                editor->updateNow();
            }
        };
        auto shot = [&] (int w, int h, const juce::String& suffix)
        {
            editor->setSize (w, h);
            play (30);
            const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
            const auto file = outDir.getChildFile (juce::String (product.id) + "_" + suffix + ".png");
            file.deleteFile();
            juce::FileOutputStream out (file);
            juce::PNGImageFormat png;
            const bool ok = out.openedOk() && png.writeImageToStream (image, out);
            std::cout << (ok ? "wrote " : "FAILED ") << file.getFullPathName() << "\n";
        };

        play (90);
        shot (PureSheenEditor::kDefaultWidth, PureSheenEditor::defaultHeightFor (product), "default");
        shot (PureSheenEditor::kMinWidth, PureSheenEditor::minHeightFor (product), "min");
        shot (PureSheenEditor::kMaxWidth, PureSheenEditor::kMaxHeight, "max");
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::defaultHeightFor (product));
        editor->setHelpVisible (true);
        shot (PureSheenEditor::kDefaultWidth, PureSheenEditor::defaultHeightFor (product), "help");
    }
    return 0;
}
