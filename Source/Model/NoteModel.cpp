#include "NoteModel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <locale>

namespace moontune
{

namespace ids
{
    static const juce::Identifier note        { "Note" };
    static const juce::Identifier id          { "id" };
    static const juce::Identifier start       { "start" };
    static const juce::Identifier end         { "end" };
    static const juce::Identifier pitch       { "pitch" };
    static const juce::Identifier pitchShift  { "pitchShiftCents" };
    static const juce::Identifier modulation  { "modulation" };
    static const juce::Identifier drift       { "drift" };
    static const juce::Identifier timeShift   { "timeShift" };
    static const juce::Identifier startEdit   { "startEdit" };
    static const juce::Identifier endEdit     { "endEdit" };
    static const juce::Identifier formant     { "formant" };
    static const juce::Identifier gain        { "gain" };
    static const juce::Identifier eq          { "eq" };
    static const juce::Identifier corr        { "corr" };
    static const juce::Identifier corrDrift   { "corrDrift" };
    static const juce::Identifier corrSpeed   { "corrSpeed" };
    static const juce::Identifier drawn       { "drawn" };
    static const juce::Identifier kind        { "kind" };
    static const juce::Identifier tilt        { "tilt" };
    static const juce::Identifier transition  { "transition" };
}

const juce::Identifier NoteModel::treeType { "Notes" };

namespace
{
    // Lossless text for doubles/floats (JUCE's default double->string keeps fewer digits).
    juce::String exact (double v, int digits)
    {
        std::ostringstream os;
        os.imbue (std::locale::classic());
        os.precision (digits);
        os << v;
        return os.str();
    }

    double num (const juce::var& v, double fallback)
    {
        if (v.isVoid())
            return fallback;
        const double d = v.isString() ? v.toString().getDoubleValue() : (double) v;
        return std::isfinite (d) ? d : fallback;
    }

    bool same (float a, float b) noexcept { return juce::exactlyEqual (a, b); }
    bool same (double a, double b) noexcept { return juce::exactlyEqual (a, b); }

    // EQ as compact text: "1|t,f,g,q,on|t,f,g,q,on|..."
    juce::String eqToString (const NoteEq& e)
    {
        juce::String s (e.enabled ? "1" : "0");
        for (const auto& b : e.bands)
            s << "|" << b.type << "," << exact (b.frequency, 9) << "," << exact (b.gainDb, 9) << ","
              << exact (b.q, 9) << "," << (b.enabled ? 1 : 0);
        return s;
    }

    NoteEq eqFromString (const juce::String& text)
    {
        NoteEq e;
        const auto parts = juce::StringArray::fromTokens (text, "|", "");
        if (parts.isEmpty())
            return e;
        e.enabled = parts[0].getIntValue() != 0;
        for (int i = 0; i < 4 && i + 1 < parts.size(); ++i)
        {
            const auto f = juce::StringArray::fromTokens (parts[i + 1], ",", "");
            if (f.size() < 5)
                continue;
            auto& b = e.bands[(size_t) i];
            b.type = juce::jlimit (0, (int) EqBand::numTypes - 1, f[0].getIntValue());
            b.frequency = (float) f[1].getDoubleValue();
            b.gainDb = (float) f[2].getDoubleValue();
            b.q = (float) f[3].getDoubleValue();
            b.enabled = f[4].getIntValue() != 0;
        }
        return e;
    }

    float clampFinite (float v, float lo, float hi, float fallback) noexcept
    {
        return std::isfinite (v) ? juce::jlimit (lo, hi, v) : fallback;
    }
}

// --- EQ ---------------------------------------------------------------------------

bool EqBand::operator== (const EqBand& o) const noexcept
{
    return enabled == o.enabled && type == o.type && same (frequency, o.frequency)
        && same (gainDb, o.gainDb) && same (q, o.q);
}

bool EqBand::isActive() const noexcept
{
    if (! enabled)
        return false;
    if (type == lowCut || type == highCut)
        return true;
    return std::abs (gainDb) > 0.01f;
}

EqBand NoteEq::defaultBand (int index) noexcept
{
    EqBand b;
    switch (index)
    {
        case 0:  b.type = EqBand::lowShelf;  b.frequency = 150.0f;  b.q = 0.7f; break;
        case 1:  b.type = EqBand::peak;      b.frequency = 500.0f;  b.q = 1.0f; break;
        case 2:  b.type = EqBand::peak;      b.frequency = 2500.0f; b.q = 1.0f; break;
        default: b.type = EqBand::highShelf; b.frequency = 8000.0f; b.q = 0.7f; break;
    }
    return b;
}

bool NoteEq::isActive() const noexcept
{
    if (! enabled)
        return false;
    for (const auto& b : bands)
        if (b.isActive())
            return true;
    return false;
}

bool NoteEq::operator== (const NoteEq& o) const noexcept
{
    return enabled == o.enabled && bands == o.bands;
}

// --- Note -------------------------------------------------------------------------

static bool sameDrawing (const std::vector<float>& a, const std::vector<float>& b) noexcept
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::isfinite (a[i]) != std::isfinite (b[i]) || (std::isfinite (a[i]) && std::abs (a[i] - b[i]) > 1.0e-3f))
            return false;
    return true;
}

bool Note::hasTimeEdit() const noexcept
{
    return std::abs (timeShiftSeconds) > 1.0e-6 || std::abs (startEditSeconds) > 1.0e-6
        || std::abs (endEditSeconds) > 1.0e-6;
}

bool Note::hasDrawing() const noexcept
{
    for (float v : drawnCents)
        if (std::isfinite (v)) return true;
    return false;
}

float Note::drawnAt (double sourceSeconds) const noexcept
{
    const double pos = (sourceSeconds - startSeconds) / drawHop;
    const long i = (long) std::floor (pos);
    if (i < 0 || i >= (long) drawnCents.size()) return std::numeric_limits<float>::quiet_NaN();
    const float a = drawnCents[(size_t) i];
    if (! std::isfinite (a)) return a;
    if (i + 1 >= (long) drawnCents.size() || ! std::isfinite (drawnCents[(size_t) i + 1])) return a;
    return a + (drawnCents[(size_t) i + 1] - a) * (float) (pos - (double) i);
}

float Note::drawnWeight (double sourceSeconds) const noexcept
{
    const long i = (long) std::lround ((sourceSeconds - startSeconds) / drawHop);
    if (i < 0 || i >= (long) drawnCents.size() || ! std::isfinite (drawnCents[(size_t) i])) return 0.0f;
    constexpr int edge = 4;   // 4 x 5 ms
    int dist = edge;
    for (int d = 1; d <= edge; ++d)
    {
        const long l = i - d, r = i + d;
        // An undrawn neighbour inside the note is an edge; the note's own ends are not (the
        // drawn curve holds right up to them).
        const long frames = (long) std::ceil ((endSeconds - startSeconds) / drawHop) + 1;
        const bool lEdge = l >= 0 && ! std::isfinite (drawnCents[(size_t) l]);
        const bool rEdge = r < frames && (r >= (long) drawnCents.size() || ! std::isfinite (drawnCents[(size_t) r]));
        if (lEdge || rEdge) { dist = d - 1; break; }
    }
    const float x = (float) (dist + 1) / (float) (edge + 1);
    return 0.5f - 0.5f * std::cos (3.14159265f * std::min (1.0f, x));
}

bool Note::hasCorrection() const noexcept
{
    return std::abs (correctionCents) > 0.01f || correctionDrift > 0.001f;
}

bool Note::hasSoundEdit() const noexcept
{
    return std::abs (pitchShiftCents) > 0.01f || std::abs (pitchModulation - 1.0f) > 0.001f
        || std::abs (pitchDrift - 1.0f) > 0.001f || std::abs (formantCents) > 0.01f
        || std::abs (pitchTilt) > 0.01f || transitionMs >= 0.0f
        || std::abs (gainDb) > 0.001f || eq.isActive() || hasTimeEdit() || hasCorrection() || hasDrawing();
}

bool Note::operator== (const Note& o) const noexcept
{
    return id == o.id && kind == o.kind
        && same (pitchTilt, o.pitchTilt) && same (transitionMs, o.transitionMs)
        && same (startSeconds, o.startSeconds) && same (endSeconds, o.endSeconds)
        && same (detectedPitch, o.detectedPitch)
        && same (pitchShiftCents, o.pitchShiftCents) && same (pitchModulation, o.pitchModulation)
        && same (pitchDrift, o.pitchDrift)
        && same (timeShiftSeconds, o.timeShiftSeconds) && same (startEditSeconds, o.startEditSeconds)
        && same (endEditSeconds, o.endEditSeconds)
        && same (formantCents, o.formantCents) && same (gainDb, o.gainDb) && eq == o.eq
        && same (correctionCents, o.correctionCents) && same (correctionDrift, o.correctionDrift)
        && same (correctionSpeedMs, o.correctionSpeedMs) && sameDrawing (drawnCents, o.drawnCents);
}

// --- Model ------------------------------------------------------------------------

bool NoteModel::isValid (const Note& n) noexcept
{
    return n.id > 0
        && std::isfinite (n.startSeconds) && std::isfinite (n.endSeconds)
        && n.startSeconds >= 0.0 && n.endSeconds > n.startSeconds
        && std::isfinite (n.detectedPitch) && n.detectedPitch >= 0.0f && n.detectedPitch <= 127.0f
        && std::isfinite (n.pitchShiftCents) && std::abs (n.pitchShiftCents) <= maxPitchShiftCents
        && std::isfinite (n.timeShiftSeconds) && std::abs (n.timeShiftSeconds) <= maxTimeShiftSeconds;
}

void NoteModel::clampEdits (Note& n) noexcept
{
    n.pitchShiftCents  = clampFinite (n.pitchShiftCents, -maxPitchShiftCents, maxPitchShiftCents, 0.0f);
    n.pitchModulation  = clampFinite (n.pitchModulation, 0.0f, 2.0f, 1.0f);
    n.pitchDrift       = clampFinite (n.pitchDrift, 0.0f, 2.0f, 1.0f);
    n.formantCents     = clampFinite (n.formantCents, -1200.0f, 1200.0f, 0.0f);
    n.gainDb           = clampFinite (n.gainDb, Note::silentDb, 12.0f, 0.0f);
    n.pitchTilt        = clampFinite (n.pitchTilt, -1200.0f, 1200.0f, 0.0f);
    n.transitionMs     = n.transitionMs < 0.0f ? -1.0f : clampFinite (n.transitionMs, 0.0f, 500.0f, -1.0f);
    n.kind             = juce::jlimit ((int) Note::note, (int) Note::plosive, n.kind);
    n.correctionCents  = clampFinite (n.correctionCents, -1200.0f, 1200.0f, 0.0f);
    n.correctionDrift  = clampFinite (n.correctionDrift, 0.0f, 1.0f, 0.0f);
    n.correctionSpeedMs = clampFinite (n.correctionSpeedMs, 0.0f, 1000.0f, 0.0f);
    {
        const auto frames = (size_t) std::ceil (std::max (0.0, n.endSeconds - n.startSeconds) / Note::drawHop) + 1;
        if (n.drawnCents.size() > frames) n.drawnCents.resize (frames);
        for (auto& v : n.drawnCents)
            if (std::isfinite (v)) v = juce::jlimit (-maxDrawnCents, maxDrawnCents, v);
            else v = std::numeric_limits<float>::quiet_NaN();
        if (! n.hasDrawing()) n.drawnCents.clear();
    }

    if (! std::isfinite (n.timeShiftSeconds)) n.timeShiftSeconds = 0.0;
    if (! std::isfinite (n.startEditSeconds)) n.startEditSeconds = 0.0;
    if (! std::isfinite (n.endEditSeconds))   n.endEditSeconds = 0.0;
    n.timeShiftSeconds = juce::jlimit (-maxTimeShiftSeconds, maxTimeShiftSeconds, n.timeShiftSeconds);

    // Keep the edited note at least minNoteSeconds long.
    if (n.outEnd() - n.outStart() < minNoteSeconds)
        n.endEditSeconds += minNoteSeconds - (n.outEnd() - n.outStart());

    for (auto& b : n.eq.bands)
    {
        b.type      = juce::jlimit (0, (int) EqBand::numTypes - 1, b.type);
        b.frequency = clampFinite (b.frequency, 20.0f, 20000.0f, 1000.0f);
        b.gainDb    = clampFinite (b.gainDb, -18.0f, 18.0f, 0.0f);
        b.q         = clampFinite (b.q, 0.2f, 10.0f, 1.0f);
    }
}

std::vector<Note> NoteModel::getSnapshot() const
{
    const juce::ScopedLock sl (lock);
    return notes;
}

void NoteModel::replaceAll (std::vector<Note> newNotes)
{
    std::sort (newNotes.begin(), newNotes.end(),
               [] (const Note& a, const Note& b) { return a.startSeconds < b.startSeconds; });

    const juce::ScopedLock sl (lock);
    notes = std::move (newNotes);
    ++version;
}

void NoteModel::clear()
{
    const juce::ScopedLock sl (lock);
    notes.clear();
    ++version;
}

juce::int64 NoteModel::nextFreeId() const
{
    const juce::ScopedLock sl (lock);
    juce::int64 maxId = 0;
    for (const auto& n : notes)
        maxId = std::max (maxId, n.id);
    return maxId + 1;
}

int NoteModel::size() const
{
    const juce::ScopedLock sl (lock);
    return (int) std::count_if (notes.begin(), notes.end(), [] (const Note& n) { return n.isNote(); });
}

int NoteModel::sizeAll() const
{
    const juce::ScopedLock sl (lock);
    return (int) notes.size();
}

juce::ValueTree NoteModel::toValueTree() const
{
    juce::ValueTree tree (treeType);
    const Note defaults;

    for (const auto& n : getSnapshot())
    {
        juce::ValueTree child (ids::note);
        child.setProperty (ids::id,         juce::String (n.id), nullptr);  // int64 as string: lossless
        child.setProperty (ids::start,      exact (n.startSeconds, 17),     nullptr);
        child.setProperty (ids::end,        exact (n.endSeconds, 17),       nullptr);
        child.setProperty (ids::pitch,      exact (n.detectedPitch, 9),     nullptr);
        child.setProperty (ids::pitchShift, exact (n.pitchShiftCents, 9),   nullptr);
        child.setProperty (ids::timeShift,  exact (n.timeShiftSeconds, 17), nullptr);

        // Newer fields are written only when they differ from the default (keeps projects small).
        auto opt = [&child] (const juce::Identifier& key, double value, double def, int digits)
        {
            if (! juce::exactlyEqual (value, def))
                child.setProperty (key, exact (value, digits), nullptr);
        };
        opt (ids::modulation, n.pitchModulation, defaults.pitchModulation, 9);
        opt (ids::drift,      n.pitchDrift, defaults.pitchDrift, 9);
        opt (ids::startEdit,  n.startEditSeconds, 0.0, 17);
        opt (ids::endEdit,    n.endEditSeconds, 0.0, 17);
        opt (ids::formant,    n.formantCents, 0.0, 9);
        opt (ids::gain,       n.gainDb, 0.0, 9);
        opt (ids::corr,       n.correctionCents, 0.0, 9);
        opt (ids::corrDrift,  n.correctionDrift, 0.0, 9);
        opt (ids::corrSpeed,  n.correctionSpeedMs, 0.0, 9);
        opt (ids::tilt,       n.pitchTilt, 0.0, 9);
        opt (ids::transition, n.transitionMs, -1.0, 9);
        if (n.kind != Note::note)
            child.setProperty (ids::kind, n.kind, nullptr);
        if (n.eq != defaults.eq)
            child.setProperty (ids::eq, eqToString (n.eq), nullptr);
        if (n.hasDrawing())
        {
            // "cents cents n ..." (n = not drawn), 0.001-cent resolution.
            juce::String d;
            d.preallocateBytes (n.drawnCents.size() * 6);
            for (size_t i = 0; i < n.drawnCents.size(); ++i)
            {
                if (i > 0) d << ' ';
                const float v = n.drawnCents[i];
                d << (std::isfinite (v) ? juce::String (v, 3) : juce::String ("n"));
            }
            child.setProperty (ids::drawn, d, nullptr);
        }

        tree.appendChild (child, nullptr);
    }

    return tree;
}

int NoteModel::fromValueTree (const juce::ValueTree& tree)
{
    std::vector<Note> loaded;
    std::set<juce::int64> seenIds;
    int rejected = 0;

    if (tree.hasType (treeType))
    {
        for (const auto& child : tree)
        {
            if (! child.hasType (ids::note))
            {
                ++rejected;
                continue;
            }

            Note n;
            n.id               = child.getProperty (ids::id).toString().getLargeIntValue();
            n.startSeconds     = num (child.getProperty (ids::start), -1.0);
            n.endSeconds       = num (child.getProperty (ids::end), -1.0);
            n.detectedPitch    = (float) num (child.getProperty (ids::pitch), -1.0);
            n.pitchShiftCents  = (float) num (child.getProperty (ids::pitchShift), 0.0);
            n.timeShiftSeconds = num (child.getProperty (ids::timeShift), 0.0);

            if (! isValid (n) || ! seenIds.insert (n.id).second)
            {
                ++rejected;
                continue;
            }

            n.pitchModulation   = (float) num (child.getProperty (ids::modulation), 1.0);
            n.pitchDrift        = (float) num (child.getProperty (ids::drift), 1.0);
            n.startEditSeconds  = num (child.getProperty (ids::startEdit), 0.0);
            n.endEditSeconds    = num (child.getProperty (ids::endEdit), 0.0);
            n.formantCents      = (float) num (child.getProperty (ids::formant), 0.0);
            n.gainDb            = (float) num (child.getProperty (ids::gain), 0.0);
            n.correctionCents   = (float) num (child.getProperty (ids::corr), 0.0);
            n.correctionDrift   = (float) num (child.getProperty (ids::corrDrift), 0.0);
            n.correctionSpeedMs = (float) num (child.getProperty (ids::corrSpeed), 0.0);
            n.pitchTilt         = (float) num (child.getProperty (ids::tilt), 0.0);
            n.transitionMs      = (float) num (child.getProperty (ids::transition), -1.0);
            n.kind              = (int) child.getProperty (ids::kind, 0);
            if (child.hasProperty (ids::eq))
                n.eq = eqFromString (child.getProperty (ids::eq).toString());
            if (child.hasProperty (ids::drawn))
            {
                juce::StringArray parts;
                parts.addTokens (child.getProperty (ids::drawn).toString(), " ", "");
                for (const auto& p : parts)
                    n.drawnCents.push_back (p == "n" ? std::numeric_limits<float>::quiet_NaN() : p.getFloatValue());
            }
            clampEdits (n);

            loaded.push_back (n);
        }
    }

    replaceAll (std::move (loaded));
    return rejected;
}

} // namespace moontune
