#include "plugin/import/ImportCommon.h"

#include <algorithm>
#include <cmath>

// Preset importer: ModuleImport (the per-section mapping state the tables, transforms and hooks work with), the
// ParamID template split and the shared transforms. Pure C++17 (also compiled into CoreTests).
namespace ps::imp
{
namespace
{
    constexpr std::string_view kSetBPrefix = "Aux: ";   // the format's generic marker of the second channel set
}

//==============================================================================
ParamName splitParamName (std::string_view id, const Aliases* aliases)
{
    ParamName p;
    if (id.size() > 256)
    {
        p.number = -2;
        return p;
    }

    if (id.substr (0, kSetBPrefix.size()) == kSetBPrefix)
    {
        p.set = 1;
        id.remove_prefix (kSetBPrefix.size());
    }

    size_t start = std::string_view::npos;
    for (size_t i = 0; i < id.size(); ++i)
        if (id[i] >= '0' && id[i] <= '9')
        {
            start = i;
            break;
        }

    if (start == std::string_view::npos)
    {
        p.templ.assign (id.data(), id.size());
    }
    else
    {
        size_t end = start;
        while (end < id.size() && id[end] >= '0' && id[end] <= '9')
            ++end;

        if (end - start <= 6)
        {
            int n = 0;
            for (size_t i = start; i < end; ++i)
                n = n * 10 + (id[i] - '0');
            p.number = n;
        }
        else
        {
            p.number = -2;
        }

        p.templ.assign (id.data(), start);
        p.templ += '#';
        p.templ.append (id.data() + end, id.size() - end);
    }

    if (aliases != nullptr)
        if (const auto it = aliases->find (p.templ); it != aliases->end())
        {
            p.key = it->second;
            return p;
        }

    p.key = importKey (p.templ);
    return p;
}

Key sectionKey (std::string_view name, const Aliases* aliases)
{
    if (aliases != nullptr)
        if (const auto it = aliases->find (name); it != aliases->end())
            return it->second;
    return importKey (name);
}

void Report::note (const std::string& text)
{
    if (std::find (notes.begin(), notes.end(), text) == notes.end())
        notes.push_back (text);
}

//==============================================================================
ModuleImport::ModuleImport (const ModuleSpec& s, std::vector<StoredParam>& st, int pluginVer, std::vector<float>& v,
                            Report& r, std::string sectionName)
    : spec (s), stored (st), version (pluginVer), values (v), report (r), section (std::move (sectionName))
{
    if ((int) spec.type >= 0 && (int) spec.type < kNumModuleTypes)
        idPrefix = moduleInfo (spec.type).idPrefix;
}

const Rule* ModuleImport::findRule (Key key) const noexcept
{
    if (spec.rules == nullptr)
        return nullptr;
    for (const auto& r : *spec.rules)
        if (r.key == key)
            return &r;
    return nullptr;
}

StoredParam* ModuleImport::find (Key key, int number, int set)
{
    for (auto& p : stored)
        if (p.name.key == key && p.name.number == number && p.name.set == set)
            return &p;
    return nullptr;
}

const StoredParam* ModuleImport::find (Key key, int number, int set) const
{
    for (const auto& p : stored)
        if (p.name.key == key && p.name.number == number && p.name.set == set)
            return &p;
    return nullptr;
}

bool ModuleImport::has (Key key, int number, int set) const
{
    const auto* p = find (key, number, set);
    return p != nullptr && ! std::isnan (p->value);
}

double ModuleImport::defaultOf (Key key, int number, int set) const
{
    const auto* r = findRule (key);
    if (r == nullptr)
        return kNoValue;
    return r->def.fn != nullptr ? r->def.fn (*this, number, set) : r->def.value;
}

double ModuleImport::get (Key key, int number, int set)
{
    if (auto* p = find (key, number, set))
    {
        if (! std::isnan (p->value))
        {
            if (p->use == StoredParam::Use::None)
                p->use = StoredParam::Use::Mapped;
            return p->value;
        }
        p->use = StoredParam::Use::Rejected;
    }
    return defaultOf (key, number, set);
}

double ModuleImport::peek (Key key, int number, int set) const
{
    if (const auto* p = find (key, number, set); p != nullptr && ! std::isnan (p->value))
        return p->value;
    return defaultOf (key, number, set);
}

const std::string* ModuleImport::extraBytes (Key elementKey) const noexcept
{
    for (const auto& [k, data] : extra)
        if (k == elementKey)
            return data;
    return nullptr;
}

void ModuleImport::attach (int slot, std::vector<unsigned char> bytes, int mappedControls)
{
    Report::Attachment a;
    a.type = spec.type;
    a.slot = slot;
    a.bytes = std::move (bytes);
    a.mappedControls = mappedControls;
    report.attachments.push_back (std::move (a));
}

double ModuleImport::getOr (Key key, int number, int set, double fallback)
{
    const double v = get (key, number, set);
    return std::isnan (v) ? fallback : v;
}

void ModuleImport::reject (Key key, int number, int set, const char* reason)
{
    if (auto* p = find (key, number, set))
    {
        p->use = StoredParam::Use::Rejected;
        report.unmappedList.push_back ({ section, p->rawName, p->rawValue, reason });
        p->rawName.clear();   // reported here, not again in the leftover pass
    }
}

void ModuleImport::ignore (Key key, int number, int set)
{
    if (auto* p = find (key, number, set))
        if (p->use == StoredParam::Use::None)
            p->use = StoredParam::Use::Ignored;
}

std::string ModuleImport::resolve (std::string_view pattern, int band) const
{
    std::string id;
    id.reserve (pattern.size() + 8);
    for (const char c : pattern)
    {
        if (c == '~')
            id += idPrefix;
        else if (c == '#')
            id += std::to_string (band);
        else
            id += c;
    }
    return id;
}

bool ModuleImport::set (std::string_view pattern, double v, int band)
{
    if (std::isnan (v) || std::isinf (v))
        return false;

    const auto id = resolve (pattern, band);
    const int index = paramIndex (id);
    if (index < 0 || index >= (int) values.size())
    {
        report.unmappedList.push_back ({ section, "(table target)", id, "target id" });
        report.note ("internal: unknown target id " + id);
        return false;
    }

    const auto& s = paramSpecs()[(size_t) index];
    double out = v;

    switch (s.kind)
    {
        case ParamKind::Bool:
            out = v >= 0.5 ? 1.0 : 0.0;
            break;

        case ParamKind::Choice:
        {
            const double hi = (double) std::max<size_t> (1, s.choices.size()) - 1.0;
            out = std::round (v);
            if (out < 0.0 || out > hi)
            {
                ++report.clamped;
                out = std::clamp (out, 0.0, hi);
            }
            break;
        }

        case ParamKind::Float:
        {
            const double lo = s.minValue, hi = s.maxValue;
            const double tolerance = (hi - lo) * 1.0e-5;
            if (v < lo - tolerance || v > hi + tolerance)
                ++report.clamped;
            out = std::clamp (v, lo, hi);
            if (s.interval > 0.0f)
                out = std::clamp (lo + std::round ((out - lo) / s.interval) * s.interval, lo, hi);
            break;
        }
    }

    values[(size_t) index] = (float) out;
    return true;
}

bool ModuleImport::setFromDefault (std::string_view pattern, double v, int band)
{
    if (! set (pattern, v, band))
        return false;
    ++report.defaulted;
    return true;
}

void ModuleImport::setChannelMode (int foreignIndex)
{
    const auto modes = supportedChannelModes (spec.type);
    if (modes.size() < 2)
        return;   // Stereo-only module: nothing to write

    if (foreignIndex < 0 || foreignIndex >= (int) modes.size())
    {
        ++report.clamped;
        foreignIndex = 0;
    }

    for (const auto& [id, value] : channelModeParamValues (spec.type, modes[(size_t) foreignIndex]))
        set (id, value);
}

void ModuleImport::note (const std::string& text) { report.note (text); }

//==============================================================================
const BandLayout& ModuleImport::readLayout (Key activeKey, Key numBandsKey, int activeFlagsFrom)
{
    bool active[kMaxBands] {};

    if (version == 0 || version >= activeFlagsFrom)
    {
        for (int slot = 0; slot < kMaxBands; ++slot)
        {
            const double a = get (activeKey, slot, 0);
            active[slot] = std::isnan (a) ? slot == 0 : a >= 0.5;
        }
        ignore (numBandsKey);   // the older band count is stored alongside; the per-slot flags win
    }
    else
    {
        const double n = get (numBandsKey);
        const int count = std::isnan (n) ? 1 : (int) std::lround (std::clamp (n, 1.0, (double) kMaxBands));
        for (int slot = 0; slot < kMaxBands; ++slot)
        {
            active[slot] = slot < count;
            ignore (activeKey, slot, 0);
        }
    }

    if (std::none_of (std::begin (active), std::end (active), [] (bool b) { return b; }))
        active[0] = true;

    BandLayout l;
    l.numBands = 0;
    for (int slot = 0; slot < kMaxBands; ++slot)
    {
        l.bandOfSlot[slot] = 0;
        if (active[slot])
        {
            l.slotOfBand[l.numBands] = slot;
            l.bandOfSlot[slot] = l.numBands + 1;
            ++l.numBands;
        }
    }
    for (int i = l.numBands; i < kMaxBands; ++i)
        l.slotOfBand[i] = -1;
    for (int i = 0; i < kMaxCrossovers; ++i)
        l.cutoffOfXover[i] = i + 1 < l.numBands ? l.slotOfBand[i + 1] : 0;

    bands = l;
    if (! idPrefix.empty())
        set ("~.bands", l.numBands);
    return bands;
}

void ModuleImport::writeCrossovers (Key cutoffKey)
{
    bool used[kMaxBands] {};
    for (int i = 0; i < kMaxCrossovers; ++i)
    {
        const int c = bands.cutoffOfXover[i];
        if (c <= 0 || c > kMaxCrossovers)
            continue;
        used[c] = true;
        const double v = get (cutoffKey, c, 0);
        if (! std::isnan (v))
        {
            if (! has (cutoffKey, c, 0))
                ++report.defaulted;
            set ("~.xover" + std::to_string (i + 1), v);
        }
    }
    for (int c = 1; c <= kMaxCrossovers; ++c)
        if (! used[c])
            ignore (cutoffKey, c, 0);   // the edge of a removed band: stale
}

//==============================================================================
void ModuleImport::applyRule (const Rule& r)
{
    switch (r.act)
    {
        case Act::Hook:
        case Act::Unsupported:
            return;

        case Act::Ignore:
            for (auto& p : stored)
                if (p.name.key == r.key && p.use == StoredParam::Use::None)
                    p.use = StoredParam::Use::Ignored;
            return;

        case Act::ChannelMode:
        {
            const bool stored0 = has (r.key, -1, 0);
            const double v = get (r.key, -1, 0);
            if (! std::isnan (v))
            {
                if (! stored0)
                    ++report.defaulted;
                setChannelMode ((int) std::lround (std::clamp (v, -1.0, 100.0)));
            }
            return;
        }

        case Act::Map:
            break;
    }

    const int numbers = r.count > 0 ? r.count : 1;
    for (int chSet = 0; chSet < 2; ++chSet)
    {
        const char* target = chSet == 0 ? r.idA : r.idB;
        if (target == nullptr)
            continue;

        for (int i = 0; i < numbers; ++i)
        {
            const int number = r.count > 0 ? r.first + i : -1;
            int band = r.count > 0 ? i + 1 : 0;
            if (r.band == Band::Layout)
            {
                band = (i >= 0 && i < kMaxBands) ? bands.bandOfSlot[i] : 0;
                if (band <= 0)
                {
                    ignore (r.key, number, chSet);   // a removed band's leftover
                    continue;
                }
            }

            auto* p = find (r.key, number, chSet);
            const bool isStored = p != nullptr && ! std::isnan (p->value);
            double v = kNoValue;
            if (isStored)
                v = p->value;
            else
            {
                if (p != nullptr)
                    p->use = StoredParam::Use::Rejected;   // unreadable number: fall back to the default
                v = r.def.fn != nullptr ? r.def.fn (*this, number, chSet) : r.def.value;
            }
            if (std::isnan (v))
                continue;

            const double out = r.xf != nullptr ? r.xf (v, *this, number, chSet) : v;
            if (std::isnan (out))
            {
                if (isStored)
                    p->use = StoredParam::Use::Rejected;
                continue;
            }

            if (set (target, out, band))
            {
                if (isStored)
                {
                    if (p->use == StoredParam::Use::None)
                        p->use = StoredParam::Use::Mapped;
                }
                else
                {
                    ++report.defaulted;
                }
            }
        }
    }
}

void ModuleImport::run()
{
    if (spec.prepare != nullptr)
        spec.prepare (*this);

    if (spec.rules != nullptr)
        for (const auto& r : *spec.rules)
            applyRule (r);

    if (spec.finish != nullptr)
        spec.finish (*this);

    for (auto& p : stored)
    {
        switch (p.use)
        {
            case StoredParam::Use::Mapped:  ++report.mapped; break;
            case StoredParam::Use::Ignored: ++report.ignored; break;
            case StoredParam::Use::Rejected:
                ++report.unmapped;
                if (! p.rawName.empty())
                    report.unmappedList.push_back ({ section, p.rawName, p.rawValue, "value" });
                break;
            case StoredParam::Use::None:
            {
                ++report.unmapped;
                const auto* r = findRule (p.name.key);
                const char* reason = "unknown";
                if (r != nullptr)
                {
                    if (r->act == Act::Unsupported)
                        reason = "unsupported";
                    else if (r->act == Act::Map && p.name.set == 1 && r->idB == nullptr)
                        reason = "set B";
                    else if (r->act == Act::Map || r->act == Act::ChannelMode)
                        reason = "number";
                    else
                        reason = "unused";
                }
                report.unmappedList.push_back ({ section, p.rawName, p.rawValue, reason });
                break;
            }
        }
    }
}

//==============================================================================
namespace xf
{
double boolean (double v, ModuleImport&, int, int) { return v >= 0.5 ? 1.0 : 0.0; }
double invertBool (double v, ModuleImport&, int, int) { return v >= 0.5 ? 0.0 : 1.0; }
double roundIndex (double v, ModuleImport&, int, int) { return std::round (v); }
} // namespace xf
} // namespace ps::imp
