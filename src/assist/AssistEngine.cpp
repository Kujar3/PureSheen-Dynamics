#include "assist/AssistEngine.h"

#include "dsp/Equalizer.h"
#include "dsp/Params.h"
#include "dsp/ToneBalance.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>

// The rules of the default suggestion (fitted black-box to the reference assistant's choices on seventeen test
// tracks, features from the first 8 s it listens to; numbers and method in the private notes, out/assistant):
//   - EQ 1: desired curve G = S tanh (k D / S), D = target - track (relative to pink, level-matched over 100 Hz ..
//     5.6 kHz, Gaussian-smoothed over 0.5 octave), k = 0.25, S = 1.5 dB at Balanced. Fitted greedily with a low shelf
//     (band 1), a high shelf (band 8) and up to four broad bells (bands 2-5, Q 0.3..1.1), gains within +-1.8 dB.
//     (Reference EQ curves: 0.76 dB RMS from this rule, 1.15 dB RMS from no EQ; the rule was fitted on the first seven
//     tracks and held on the other ten, so refitting it did not pay.)
//   - Imager band b: width = clamp (g_b (T_b - W_b), -100, 100), W_b = the band's side / mid energy ratio in dB, T / g =
//     (-21 dB, 4) narrowing only, (-5.5, 12), (-3.75, 9.5), (-1.5, 10.5); a reference track sets T_b to its own widths.
//   - Punch band b: amount = clamp (g_b (T_b - C_b), -80, 80), C_b = the band's frame crest (p99 - p50 of 46 ms
//     frames, dB), T / g = (7, 4.5), (3.75, 7.5), (8.5, 6.5), (11, 5).
//   - Dynamic EQ: the reference's template (a -4 dB bell in the low-mids, two narrow -4 dB bells at the strongest
//     resonances in 350 Hz .. 950 Hz and 1.3 .. 5.5 kHz, a -2 dB high shelf); thresholds = the input's sub-band level
//     there + 10 / 7.5 / 10.5 / 10 dB.
//   - Intensity scales k, S, the gain limits, the Punch / Imager slopes and the Definition / Tone Balance amounts by
//     0.6 (Gentle), 1 (Balanced) or 1.5 (Strong).
namespace ps::assist
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kDesignRate = 48000.0;

const std::vector<ModuleType> kDefaultOrder { ModuleType::Equalizer1, ModuleType::Punch, ModuleType::Imager,
                                              ModuleType::Definition, ModuleType::ToneBalance, ModuleType::DynamicEq,
                                              ModuleType::Maximizer };

float intensityScale (Intensity i) noexcept
{
    return i == Intensity::Gentle ? 0.6f : (i == Intensity::Strong ? 1.5f : 1.0f);
}

void set (std::vector<float>& v, const char* id, float value)
{
    const int i = pid (id);
    if (i < 0)
        return;
    const auto& s = paramSpecs()[(size_t) i];
    value = std::clamp (value, s.minValue, s.maxValue);
    if (s.kind != ParamKind::Float)
        value = std::round (value);
    v[(size_t) i] = value;
}

void set (std::vector<float>& v, const std::string& id, float value) { set (v, id.c_str(), value); }

float get (const std::vector<float>& v, const std::string& id)
{
    const int i = pid (id.c_str());
    return i >= 0 ? v[(size_t) i] : 0.0f;
}

std::vector<float> defaultValues()
{
    std::vector<float> v;
    for (const auto& s : paramSpecs())
        v.push_back (s.defaultValue);
    return v;
}

std::string band (const char* prefix, int b, const char* name)
{
    return std::string (prefix) + ".b" + std::to_string (b) + "." + name;
}

float widthDb (float pct) noexcept { return (float) (10.0 * std::log10 (std::max (pct, 0.05f) / 100.0)); }

//==============================================================================
// EQ fitting on the curve points between 25 Hz and 18 kHz.
struct EqFit
{
    std::vector<double> hz;
    std::vector<double> want;     // desired curve (dB)

    double error (const std::vector<double>& have) const
    {
        double e = 0.0;
        for (size_t i = 0; i < hz.size(); ++i)
            e += (want[i] - have[i]) * (want[i] - have[i]);
        return std::sqrt (e / (double) hz.size());
    }
};

std::vector<double> bandCurve (const EqBandParams& b, const std::vector<double>& hz)
{
    std::vector<double> r (hz.size());
    for (size_t i = 0; i < hz.size(); ++i)
        r[i] = 20.0 * std::log10 (std::max (1.0e-9, std::abs (eqBandResponse (b, EqFilterMode::Analog, hz[i],
                                                                                kDesignRate, 100.0f))));
    return r;
}

// Best gain for one band shape at a frequency: least squares on the unit (+1 dB) response, then refined once with the
// real response at that gain (shelves / bells are close to linear in dB at these small gains).
EqBandParams bestBand (const EqFit& fit, const std::vector<double>& have, EqShape shape, double freq, double q,
                       double maxGain, double& newError)
{
    EqBandParams b;
    b.on = true;
    b.shape = shape;
    b.freqHz = (float) freq;
    b.q = (float) q;
    b.gainDb = 1.0f;
    const auto unit = bandCurve (b, fit.hz);

    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < fit.hz.size(); ++i)
    {
        num += unit[i] * (fit.want[i] - have[i]);
        den += unit[i] * unit[i];
    }

    const double g = den > 1.0e-12 ? std::clamp (num / den, -maxGain, maxGain) : 0.0;
    b.gainDb = (float) g;
    const auto real = bandCurve (b, fit.hz);
    std::vector<double> next (have);
    for (size_t i = 0; i < next.size(); ++i)
        next[i] += real[i];
    newError = fit.error (next);
    return b;
}

// The reference's EQ moves carry a bias the target difference does not explain (its mean over the seven tracks minus
// our rule, sd 0.4..1 dB): a little less bass and air, a little more around 1 kHz. Interpolated in log f.
double eqBiasDb (double hz)
{
    static const double pts[][2] { { 25.0, -0.25 }, { 40.0, -0.35 }, { 60.0, -0.32 }, { 95.0, -0.23 }, { 150.0, -0.11 },
                                   { 233.0, 0.03 }, { 363.0, 0.21 }, { 567.0, 0.36 }, { 887.0, 0.53 },
                                   { 1385.0, 0.48 }, { 2163.0, 0.15 }, { 3379.0, 0.08 }, { 5279.0, 0.14 },
                                   { 8246.0, -0.32 }, { 12882.0, -0.81 }, { 20000.0, -0.9 } };
    constexpr int n = (int) (sizeof (pts) / sizeof (pts[0]));
    if (hz <= pts[0][0])
        return pts[0][1];
    for (int i = 1; i < n; ++i)
        if (hz <= pts[i][0])
        {
            const double u = std::log (hz / pts[i - 1][0]) / std::log (pts[i][0] / pts[i - 1][0]);
            return pts[i - 1][1] + u * (pts[i][1] - pts[i - 1][1]);
        }
    return pts[n - 1][1];
}

void fitEq (std::vector<float>& v, const Analysis& a, const Curve& target, float scale)
{
    const double k = 0.25 * scale, S = 1.5 * scale, maxGain = 1.8 * scale;

    // D = target - track, level-matched over 100 Hz .. 5.6 kHz
    std::vector<double> hz, d;
    double sum = 0.0;
    int count = 0;
    for (int i = 0; i < kCurvePoints; ++i)
    {
        const double f = curveFrequency (i);
        if (a.octaveDb[(size_t) i] <= kNoValue || target[(size_t) i] <= kNoValue || f < 25.0 || f > 18000.0)
            continue;
        hz.push_back (f);
        d.push_back (target[(size_t) i] - a.octaveDb[(size_t) i]);
        if (f >= 100.0 && f <= 5600.0)
        {
            sum += d.back();
            ++count;
        }
    }

    if (hz.size() < 8 || count == 0)
        return;

    for (auto& x : d)
        x -= sum / count;

    EqFit fit;
    fit.hz = hz;
    fit.want.resize (hz.size());
    for (size_t i = 0; i < hz.size(); ++i)
    {
        double ws = 0.0, acc = 0.0;
        for (size_t j = 0; j < hz.size(); ++j)
        {
            const double u = std::log2 (hz[j] / hz[i]) / 0.5;
            const double w = std::exp (-0.5 * u * u);
            ws += w;
            acc += w * d[j];
        }
        fit.want[i] = S * std::tanh (k * (acc / ws) / S) + scale * eqBiasDb (hz[i]);
    }

    std::vector<double> have (hz.size(), 0.0);
    auto commit = [&] (const EqBandParams& b, int index) {
        const auto r = bandCurve (b, hz);
        for (size_t i = 0; i < have.size(); ++i)
            have[i] += r[i];
        set (v, band ("eq1.a", index, "on"), 1.0f);
        set (v, band ("eq1.a", index, "shape"), (float) (int) b.shape);
        set (v, band ("eq1.a", index, "freq"), b.freqHz);
        set (v, band ("eq1.a", index, "gain"), b.gainDb);
        set (v, band ("eq1.a", index, "q"), b.q);
    };

    // Shelves: low shelf 20..250 Hz, high shelf 5..20 kHz (1/6-octave grid), the reference's shelf Q.
    constexpr double kShelfQ = 8.72222;
    auto bestShelf = [&] (EqShape shape, double lo, double hi) {
        EqBandParams best;
        double bestErr = fit.error (have);
        for (double f = lo; f <= hi * 1.0001; f *= std::pow (2.0, 1.0 / 6.0))
        {
            double e = 0.0;
            const auto b = bestBand (fit, have, shape, f, kShelfQ, maxGain, e);
            if (e < bestErr - 1.0e-6)
            {
                bestErr = e;
                best = b;
            }
        }
        return best;
    };

    const auto low = bestShelf (EqShape::LowShelfAnalog, 20.0, 250.0);
    commit (low.on ? low : EqBandParams { true, EqShape::LowShelfAnalog, 60.0f, 0.0f, (float) kShelfQ }, 1);
    const auto high = bestShelf (EqShape::HighShelfAnalog, 5000.0, 20000.0);
    commit (high.on ? high : EqBandParams { true, EqShape::HighShelfAnalog, 12000.0f, 0.0f, (float) kShelfQ }, 8);

    // Up to four broad bells, greedily, while each removes at least 0.03 dB of RMS error and moves >= 0.3 dB.
    constexpr double kQs[] { 0.3, 0.45, 0.6, 0.8, 1.1 };
    for (int index = 2; index <= 5; ++index)
    {
        EqBandParams best;
        const double before = fit.error (have);
        double bestErr = before;
        for (double f = 40.0; f <= 16000.0; f *= std::pow (2.0, 1.0 / 6.0))
            for (double q : kQs)
            {
                double e = 0.0;
                const auto b = bestBand (fit, have, EqShape::Bell, f, q, maxGain, e);
                if (e < bestErr - 1.0e-6)
                {
                    bestErr = e;
                    best = b;
                }
            }
        if (! best.on || before - bestErr < 0.03 || std::abs (best.gainDb) < 0.3f)
            break;
        commit (best, index);
    }
}

//==============================================================================
float subBandLevel (const Analysis& a, double hz)
{
    const double lo = hz * std::pow (2.0, -1.0 / 12.0), hi = hz * std::pow (2.0, 1.0 / 12.0);
    return bandFeatures (a, lo, hi).levelDb;
}

// Strongest resonance (curve over its 1-octave Gaussian smoothing) in [lo, hi].
double resonance (const Analysis& a, double lo, double hi)
{
    const auto& c = a.profile.curveDb;
    double best = -1.0e9, bestHz = std::sqrt (lo * hi);
    for (int i = 0; i < kCurvePoints; ++i)
    {
        const double f = curveFrequency (i);
        if (f < lo || f > hi || c[(size_t) i] <= kNoValue)
            continue;
        double ws = 0.0, acc = 0.0;
        for (int j = 0; j < kCurvePoints; ++j)
        {
            if (c[(size_t) j] <= kNoValue)
                continue;
            const double u = std::log2 (curveFrequency (j) / f) / 0.5;
            const double w = std::exp (-0.5 * u * u);
            ws += w;
            acc += w * c[(size_t) j];
        }
        const double excess = c[(size_t) i] - acc / ws;
        if (excess > best)
        {
            best = excess;
            bestHz = f;
        }
    }
    return bestHz;
}

void dynamicEq (std::vector<float>& v, const Analysis& a, float scale)
{
    struct Band
    {
        int index;
        EqShape shapeUnused;
        float gain, q, offset;
        double freq;
    };

    const double lowMid = subBandLevel (a, 294.5) >= subBandLevel (a, 258.3) ? 294.5 : 258.3;
    const Band bands[] {
        { 1, EqShape::Bell, -4.0f, 0.7f, 10.0f, lowMid },
        { 3, EqShape::Bell, -4.0f, 4.5f, 7.5f, resonance (a, 350.0, 950.0) },
        { 4, EqShape::Bell, -4.0f, 4.5f, 10.5f, resonance (a, 1300.0, 5500.0) },
    };

    for (const auto& b : bands)
    {
        set (v, band ("deq.a", b.index, "on"), 1.0f);
        set (v, band ("deq.a", b.index, "shape"), 0.0f);   // bell
        set (v, band ("deq.a", b.index, "freq"), (float) b.freq);
        set (v, band ("deq.a", b.index, "gain"), b.gain * scale);
        set (v, band ("deq.a", b.index, "q"), b.q);
        set (v, band ("deq.a", b.index, "thr"), subBandLevel (a, b.freq) + b.offset);
        set (v, band ("deq.a", b.index, "ratio"), 1.15385f);
        set (v, band ("deq.a", b.index, "knee"), 0.0f);
    }

    // High shelf (Dynamic EQ shape 4), -2 dB
    set (v, "deq.a.b6.on", 1.0f);
    set (v, "deq.a.b6.shape", 4.0f);
    set (v, "deq.a.b6.freq", 14000.0f);
    set (v, "deq.a.b6.gain", -2.0f * scale);
    set (v, "deq.a.b6.q", 0.7f);
    set (v, "deq.a.b6.thr", subBandLevel (a, 14000.0) + 10.0f);
    set (v, "deq.a.b6.ratio", 1.07143f);
    set (v, "deq.a.b6.knee", 0.0f);
}

void imager (std::vector<float>& v, const Analysis& a, const ReferenceTarget* ref, float scale)
{
    constexpr float T[kProcBands] { -21.0f, -5.5f, -3.75f, -1.5f };
    constexpr float G[kProcBands] { 4.0f, 12.0f, 9.5f, 10.5f };

    set (v, "img.bands", 4.0f);
    set (v, "img.xoverType", 1.0f);
    // The bands the widths were measured in (the reference's files do not store its Imager crossovers 2 / 3).
    set (v, "img.xover1", (float) kProcCrossovers[0]);
    set (v, "img.xover2", (float) kProcCrossovers[1]);
    set (v, "img.xover3", (float) kProcCrossovers[2]);
    set (v, "img.amount", 50.0f);
    set (v, "img.stz.amount", 30.0f);
    set (v, "img.rec.on", 1.0f);
    set (v, "img.rec.gain", 3.0f);

    for (int b = 0; b < kProcBands; ++b)
    {
        const float target = ref != nullptr && ref->valid ? widthDb (ref->widthPct[(size_t) b]) : T[b];
        float w = std::clamp (G[b] * scale * (target - widthDb (a.procBands[(size_t) b].widthPct)), -100.0f, 100.0f);
        if (b == 0)
            w = std::min (w, 0.0f);   // the lowest band is only ever narrowed
        set (v, band ("img", b + 1, "width"), w);
    }
}

void punch (std::vector<float>& v, const Analysis& a, float scale)
{
    constexpr float T[kProcBands] { 7.0f, 3.75f, 8.5f, 11.0f };
    constexpr float G[kProcBands] { 4.5f, 7.5f, 6.5f, 5.0f };

    set (v, "pnch.bands", 4.0f);
    set (v, "pnch.xover1", 120.0f);
    set (v, "pnch.xover2", 2000.0f);
    set (v, "pnch.xover3", 10000.0f);
    set (v, "pnch.xoverType", 1.0f);
    set (v, "pnch.amount", 50.0f);
    set (v, "pnch.autoGain", 1.0f);

    for (int b = 0; b < kProcBands; ++b)
        set (v, band ("pnch.a", b + 1, "amount"),
             std::clamp (G[b] * scale * (T[b] - a.procBands[(size_t) b].crestDb), -80.0f, 80.0f));
}

void maximizer (std::vector<float>& v, const Analysis& a, const Profile& p, const Settings& s, float lufs)
{
    set (v, "max.mode", (float) (int) p.mode);
    set (v, "max.character", p.character);
    set (v, "max.upward", p.upwardDb);
    set (v, "max.clip.on", p.clipOn ? 1.0f : 0.0f);
    set (v, "max.clip.mode", (float) p.clipMode);
    set (v, "max.clip.amount", p.clipAmount);
    set (v, "max.si.trans", p.stereoIndependence);
    set (v, "max.si.sus", p.stereoIndependenceSustain);
    set (v, "max.ceiling", s.ceilingDb);
    set (v, "max.truePeak", s.truePeak ? 1.0f : 0.0f);
    set (v, "max.learnTarget", lufs);

    // First estimate: the gain that would bring the input to the target, plus 2 dB for what the limiter takes back.
    // refineLoudness (OfflineChain) replaces it with a measured value.
    const float input = a.profile.integratedLufs > kNoValue ? a.profile.integratedLufs : -18.0f;
    set (v, "max.gain", lufs - input + 2.0f);
}

std::vector<ModuleType> filterOrder (std::vector<ModuleType> order, const Settings& s)
{
    order.erase (std::remove_if (order.begin(), order.end(), [&] (ModuleType t) {
                     if (t == ModuleType::Maximizer && ! s.useMaximizer)
                         return true;
                     return (s.allowedModules & (1u << (unsigned) t)) == 0;
                 }),
                 order.end());
    return order;
}

// Inserts `what` before `before`; when that module is not in the order, before the Maximizer (never after it), else
// at the end.
void insertBefore (std::vector<ModuleType>& order, ModuleType what, ModuleType before)
{
    if (std::find (order.begin(), order.end(), what) != order.end())
        return;
    auto it = std::find (order.begin(), order.end(), before);
    if (it == order.end())
        it = std::find (order.begin(), order.end(), ModuleType::Maximizer);
    order.insert (it, what);
}

MacroValues macrosOf (const Suggestion& s)
{
    MacroValues m {};
    m[(int) Macro::ToneMatch] = get (s.values, "eq1.amount");
    m[(int) Macro::Tilt] = 0.0f;
    m[(int) Macro::Dynamics] = get (s.values, "pnch.amount");
    m[(int) Macro::Width] = get (s.values, "img.amount");
    m[(int) Macro::Compress] = 0.0f;
    m[(int) Macro::Definition] = get (s.values, "defn.a.amount");
    m[(int) Macro::ToneBalance] = get (s.values, "tbal.a.amount");
    m[(int) Macro::Loudness] = s.loudnessLufs;
    return m;
}

Suggestion buildDefault (const Context& c, bool fromReference)
{
    const auto& a = c.analysis;
    const auto& s = c.settings;
    const float scale = intensityScale (s.intensity);
    const ReferenceTarget* ref = fromReference ? &c.reference : nullptr;

    Suggestion out;
    out.target = s.profile >= 0 && s.profile < (int) ToneTarget::Count ? (ToneTarget) s.profile : detectTarget (a);
    const auto& profile = profileFor (out.target);
    out.loudnessLufs = std::clamp (s.autoLoudness ? profile.loudnessLufs : s.loudnessLufs, kMinTargetLufs, kMaxTargetLufs);
    if (ref != nullptr && s.autoLoudness && ref->integratedLufs > kNoValue)
        out.loudnessLufs = std::clamp (ref->integratedLufs, -16.0f, -6.0f);

    out.values = defaultValues();
    auto& v = out.values;

    Curve target {};
    for (int i = 0; i < kCurvePoints; ++i)
        target[(size_t) i] = ref != nullptr ? ref->octaveDb[(size_t) i]
                                            : (float) toneTargetDb (out.target, curveFrequency (i));
    fitEq (v, a, target, scale);
    punch (v, a, scale);
    imager (v, a, ref, scale);
    set (v, "defn.a.amount", 25.0f * scale);
    set (v, "defn.a.att", 100.0f);
    set (v, "defn.a.rel", 100.0f);
    set (v, "defn.a.tilt", profile.definitionTilt);
    set (v, "tbal.target", (float) (int) out.target);
    set (v, "tbal.a.amount", 25.0f * scale);
    set (v, "tbal.a.cutSpeed", 100.0f);
    set (v, "tbal.a.sens", 30.0f);
    dynamicEq (v, a, scale);
    maximizer (v, a, profile, s, out.loudnessLufs);

    out.order = filterOrder (kDefaultOrder, s);
    out.macros = macrosOf (out);
    out.isDefault = true;

    if (ref != nullptr)
    {
        out.name = "Match reference";
        out.description = "Tone, width and loudness of \"" + ref->name + "\"";
    }
    else
    {
        out.name = "Suggested";
        out.description = std::string ("Balanced master, ") + targetName (out.target) + " target";
    }
    return out;
}
} // namespace

//==============================================================================
int pid (const char* id) noexcept
{
    const int i = paramIndex (id);
    assert (i >= 0);
    return i;
}

double eqCurveDb (const std::vector<float>& values, double hz, double sampleRate)
{
    EqParams p;
    readEqParams (values.data(), ModuleType::Equalizer1, p);
    double db = 0.0;
    for (const auto& b : p.bands[0])
        if (b.on)
            db += 20.0 * std::log10 (std::max (1.0e-9, std::abs (eqBandResponse (b, EqFilterMode::Analog, hz, sampleRate,
                                                                                  p.amountPercent))));
    return db;
}

ReferenceTarget referenceFromAnalysis (const Analysis& a, const std::string& name)
{
    ReferenceTarget r;
    r.valid = a.valid;
    r.name = name;
    r.octaveDb = a.octaveDb;
    for (int b = 0; b < kProcBands; ++b)
        r.widthPct[(size_t) b] = a.procBands[(size_t) b].widthPct;
    r.integratedLufs = a.profile.integratedLufs;
    r.plrDb = a.profile.plrDb;
    return r;
}

const MacroInfo& macroInfo (Macro m) noexcept
{
    static const MacroInfo infos[kNumMacros] {
        { "toneMatch", "Tone Match", 0.0f, 200.0f, "%", true },
        { "tilt", "Tilt", -4.0f, 4.0f, "dB", true },
        { "dynamics", "Dynamics Match", 0.0f, 100.0f, "%", true },
        { "width", "Width Match", 0.0f, 100.0f, "%", true },
        { "widthLow", "Low", -100.0f, 100.0f, "", false },
        { "widthLowMid", "Low-mid", -100.0f, 100.0f, "", false },
        { "widthHighMid", "High-mid", -100.0f, 100.0f, "", false },
        { "widthHigh", "High", -100.0f, 100.0f, "", false },
        { "compress", "Compress", 0.0f, 100.0f, "%", true },
        { "definition", "Definition", 0.0f, 100.0f, "%", true },
        { "toneBalance", "Tone Balance", 0.0f, 100.0f, "%", true },
        { "loudness", "Loudness", kMinTargetLufs, kMaxTargetLufs, "LUFS", false },
    };
    const int i = std::clamp ((int) m, 0, kNumMacros - 1);
    return infos[i];
}

std::vector<Suggestion> buildSuggestions (const Context& c)
{
    std::vector<Suggestion> out;
    const bool hasRef = c.reference.valid && c.settings.useReference;
    const auto base = buildDefault (c, false);

    if (hasRef)
    {
        out.push_back (buildDefault (c, true));
        auto b = base;
        b.isDefault = false;
        out.push_back (b);
    }
    else
    {
        out.push_back (base);
    }

    auto variant = [&] (const char* name, const char* description) {
        auto s = base;
        s.isDefault = false;
        s.name = name;
        s.description = description;
        return s;
    };

    // Gentle: quieter and more open; no clipping, half the processing amounts.
    {
        auto s = variant ("Gentle", "More dynamic, lighter touch");
        s.loudnessLufs = std::max (kMinTargetLufs, std::min (base.loudnessLufs - 2.0f, -12.0f));
        set (s.values, "max.mode", (float) (int) MaximizerMode::Transparent);
        set (s.values, "max.clip.on", 0.0f);
        set (s.values, "max.upward", 0.0f);
        set (s.values, "max.character", get (base.values, "max.character") + 1.0f);
        set (s.values, "max.learnTarget", s.loudnessLufs);
        set (s.values, "max.gain", get (base.values, "max.gain") + s.loudnessLufs - base.loudnessLufs);
        set (s.values, "eq1.amount", 70.0f);
        set (s.values, "pnch.amount", 30.0f);
        set (s.values, "img.amount", 30.0f);
        s.macros = macrosOf (s);
        out.push_back (s);
    }

    // Loud: +1.5 LU, moderate soft clipping, a faster character.
    {
        auto s = variant ("Loud", "Louder, denser, more clipping");
        s.loudnessLufs = std::clamp (base.loudnessLufs + 1.5f, kMinTargetLufs, -6.0f);
        set (s.values, "max.clip.on", 1.0f);
        set (s.values, "max.clip.mode", 1.0f);
        set (s.values, "max.clip.amount", 15.0f);
        set (s.values, "max.character", std::max (1.0f, get (base.values, "max.character") - 0.5f));
        set (s.values, "max.learnTarget", s.loudnessLufs);
        set (s.values, "max.gain", get (base.values, "max.gain") + 1.4f * (s.loudnessLufs - base.loudnessLufs));
        s.macros = macrosOf (s);
        out.push_back (s);
    }

    // Warm: darker tilt, gentle tape saturation after the Imager.
    if ((c.settings.allowedModules & (1u << (unsigned) ModuleType::Exciter)) != 0)
    {
        auto s = variant ("Warm", "Darker, softer top, tape colour");
        insertBefore (s.order, ModuleType::Exciter, ModuleType::Definition);
        set (s.values, "exc.bands", 1.0f);
        set (s.values, "exc.a.b1.on", 1.0f);
        set (s.values, "exc.a.b1.mode", 2.0f);   // Tape
        set (s.values, "exc.a.b1.drive", 20.0f);
        set (s.values, "exc.a.b1.mix", 30.0f);
        s.macros = macrosOf (s);
        s.macros[(int) Macro::Tilt] = -1.5f;
        const auto applied = applyMacros (s, s.macros, 0.0f, s.order, c.analysis);
        for (const auto& [i, value] : applied.values)
            s.values[(size_t) i] = value;
        out.push_back (s);
    }

    // Bright & Wide: brighter tilt, a wider image, more definition.
    {
        auto s = variant ("Bright & Wide", "Brighter top, wider image");
        set (s.values, "img.amount", 75.0f);
        set (s.values, "defn.a.amount", std::min (100.0f, get (base.values, "defn.a.amount") + 15.0f));
        s.macros = macrosOf (s);
        s.macros[(int) Macro::Tilt] = 1.5f;
        const auto applied = applyMacros (s, s.macros, 0.0f, s.order, c.analysis);
        for (const auto& [i, value] : applied.values)
            s.values[(size_t) i] = value;
        out.push_back (s);
    }

    // Punchy: transients up in every band, Low Contrast punchy on the kick / bass.
    {
        auto s = variant ("Punchy", "Harder transients, tighter low end");
        set (s.values, "pnch.amount", 70.0f);
        for (int b = 1; b <= kProcBands; ++b)
            set (s.values, band ("pnch.a", b, "amount"), get (base.values, band ("pnch.a", b, "amount")) + 30.0f);
        if ((c.settings.allowedModules & (1u << (unsigned) ModuleType::LowContrast)) != 0)
        {
            insertBefore (s.order, ModuleType::LowContrast, ModuleType::Punch);
            set (s.values, "lowc.a.mode", 0.0f);
            set (s.values, "lowc.a.contrast", 30.0f);
        }
        s.macros = macrosOf (s);
        out.push_back (s);
    }

    for (auto& s : out)
        s.order = filterOrder (s.order, c.settings);
    return out;
}

MacroApplication applyMacros (const Suggestion& base, const MacroValues& m, float compensationDb,
                              const std::vector<ModuleType>& currentOrder, const Analysis& a)
{
    MacroApplication out;
    out.order = currentOrder;
    std::vector<float> v = base.values;
    auto put = [&] (const std::string& id, float value) {
        set (v, id, value);
        const int i = pid (id.c_str());
        if (i >= 0)
            out.values.emplace_back (i, v[(size_t) i]);
    };
    auto value = [&] (Macro x) {
        const auto& info = macroInfo (x);
        return std::clamp (m[(int) x], info.minValue, info.maxValue);
    };

    put ("eq1.amount", value (Macro::ToneMatch));

    // Tilt: Baxandall shelves on EQ 1 bands 6 (bass, 250 Hz) and 7 (treble, 4 kHz), half the tilt each way.
    const float tilt = value (Macro::Tilt);
    const bool tiltOn = std::abs (tilt) > 0.01f;
    put ("eq1.a.b6.on", tiltOn ? 1.0f : 0.0f);
    put ("eq1.a.b6.shape", (float) (int) EqShape::LowShelfBaxandall);
    put ("eq1.a.b6.freq", 250.0f);
    put ("eq1.a.b6.gain", -0.5f * tilt);
    put ("eq1.a.b7.on", tiltOn ? 1.0f : 0.0f);
    put ("eq1.a.b7.shape", (float) (int) EqShape::HighShelfBaxandall);
    put ("eq1.a.b7.freq", 4000.0f);
    put ("eq1.a.b7.gain", 0.5f * tilt);

    put ("pnch.amount", value (Macro::Dynamics));
    put ("img.amount", value (Macro::Width));
    for (int b = 0; b < kProcBands; ++b)
    {
        const auto id = band ("img", b + 1, "width");
        put (id, get (base.values, id) + value ((Macro) ((int) Macro::WidthLow + b)));
    }

    put ("defn.a.amount", value (Macro::Definition));
    put ("tbal.a.amount", value (Macro::ToneBalance));

    // Compress: one-band glue compressor before Tone Balance (inserted when the macro leaves 0; at 0 an existing
    // module is set to a transparent 1:1).
    const float c = value (Macro::Compress) / 100.0f;
    const bool hasDyn = std::find (out.order.begin(), out.order.end(), ModuleType::Dynamics) != out.order.end();
    if (c > 0.0f || hasDyn)
    {
        if (c > 0.0f && ! hasDyn)
        {
            auto it = std::find (out.order.begin(), out.order.end(), ModuleType::ToneBalance);
            if (it == out.order.end())
                it = std::find (out.order.begin(), out.order.end(), ModuleType::Maximizer);
            out.order.insert (it, ModuleType::Dynamics);
        }
        const float rms = a.profile.rmsDb > kNoValue ? a.profile.rmsDb : -18.0f;
        put ("dyn.bands", 1.0f);
        put ("dyn.a.b1.on", 1.0f);
        put ("dyn.a.b1.comp.on", 1.0f);
        put ("dyn.a.b1.comp.thr", rms + 4.0f - 10.0f * c);
        put ("dyn.a.b1.comp.ratio", c > 0.0f ? 1.2f + 2.3f * c : 1.0f);
        put ("dyn.a.b1.comp.att", 30.0f);
        put ("dyn.a.b1.comp.rel", 150.0f);
        put ("dyn.a.b1.comp.knee", 8.0f);
        put ("dyn.autoGain", 1.0f);
    }

    // Loudness: the target moves the gain along the measured slope; loudness matching adds its correction. Gain and
    // Trim are one line: above 0 dB the gain, below it the trim.
    const float target = value (Macro::Loudness);
    put ("max.learnTarget", target);
    const float gain = get (base.values, "max.gain") + get (base.values, "max.trim")
                       + base.gainPerLufs * (target - base.loudnessLufs) + compensationDb;
    put ("max.gain", std::max (gain, 0.0f));
    put ("max.trim", std::min (gain, 0.0f));
    return out;
}
} // namespace ps::assist
