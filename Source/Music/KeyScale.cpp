#include "KeyScale.h"

#include <cmath>
#include <iterator>

namespace moontune
{

namespace
{
    int wrap12 (int n) noexcept { return ((n % 12) + 12) % 12; }

    struct ScaleDef { const char* name; int count; int steps[12]; };

    constexpr ScaleDef defs[] =
    {
        { "Major",          7, { 0, 2, 4, 5, 7, 9, 11 } },
        { "Natural Minor",  7, { 0, 2, 3, 5, 7, 8, 10 } },
        { "Harmonic Minor", 7, { 0, 2, 3, 5, 7, 8, 11 } },
        { "Melodic Minor",  7, { 0, 2, 3, 5, 7, 9, 11 } },
        { "Dorian",         7, { 0, 2, 3, 5, 7, 9, 10 } },
        { "Phrygian",       7, { 0, 1, 3, 5, 7, 8, 10 } },
        { "Lydian",         7, { 0, 2, 4, 6, 7, 9, 11 } },
        { "Mixolydian",     7, { 0, 2, 4, 5, 7, 9, 10 } },
        { "Locrian",        7, { 0, 1, 3, 5, 6, 8, 10 } },
        { "Chromatic",     12, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } },
    };
    static_assert (std::size (defs) == (size_t) ScaleType::numTypes);

    const ScaleDef& defFor (ScaleType t) noexcept
    {
        const int i = juce::jlimit (0, (int) ScaleType::numTypes - 1, (int) t);
        return defs[i];
    }

    const char* const pitchNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
}

KeyScale::KeyScale (int rootPitchClass, ScaleType t) noexcept
    : root (wrap12 (rootPitchClass)),
      type ((int) t >= 0 && t < ScaleType::numTypes ? t : ScaleType::Chromatic)
{
    const auto& d = defFor (type);
    mask = 0;
    for (int i = 0; i < d.count; ++i)
        mask = (juce::uint16) (mask | (1u << wrap12 (root + d.steps[i])));
}

KeyScale KeyScale::withPitchClassesToggled (juce::uint16 t) const noexcept
{
    KeyScale k = *this;
    t = (juce::uint16) (t & 0x0fff & ~(1u << root));   // the root cannot be removed
    k.mask = (juce::uint16) (mask ^ t);
    k.toggles = t;
    k.customised = t != 0;
    return k;
}

bool KeyScale::containsPitchClass (int pitchClass) const noexcept
{
    return (mask >> wrap12 (pitchClass)) & 1u;
}

bool KeyScale::containsMidi (int midiNote) const noexcept
{
    return containsPitchClass (midiNote);
}

bool KeyScale::isRoot (int midiNote) const noexcept
{
    return wrap12 (midiNote) == root;
}

int KeyScale::nearestScaleNote (float midiPitch, int directionHint) const noexcept
{
    if (! std::isfinite (midiPitch))
        return 60;

    const float p = juce::jlimit (-12.0f, 140.0f, midiPitch);
    const int base = (int) std::floor (p);

    // Every scale has a note within 2 semitones in each direction, so a +-6 search is ample.
    int best = base;
    float bestDist = 1.0e9f;

    for (int offset = -6; offset <= 7; ++offset)
    {
        const int candidate = base + offset;
        if (! containsMidi (candidate))
            continue;

        const float dist = std::abs ((float) candidate - p);
        constexpr float eps = 1.0e-4f;

        if (dist < bestDist - eps)
        {
            best = candidate;
            bestDist = dist;
        }
        else if (std::abs (dist - bestDist) <= eps && directionHint > 0 && candidate > best)
        {
            best = candidate;   // tie: prefer upper when moving up; otherwise the lower one (found first) stays
        }
    }

    return best;
}

KeyScale::SnapResult KeyScale::snap (float midiPitch, float strength, int directionHint) const noexcept
{
    SnapResult r;
    if (! std::isfinite (midiPitch))
    {
        r.pitch = 60.0f;
        r.targetNote = 60;
        return r;
    }

    const float s = std::isfinite (strength) ? juce::jlimit (0.0f, 1.0f, strength) : 0.0f;
    r.targetNote = nearestScaleNote (midiPitch, directionHint);
    r.pitch = midiPitch + ((float) r.targetNote - midiPitch) * s;
    if (s >= 1.0f)
        r.pitch = (float) r.targetNote;   // exact, no rounding residue
    r.correctionCents = (r.pitch - midiPitch) * 100.0f;
    return r;
}

juce::String KeyScale::pitchClassName (int pitchClass)
{
    return pitchNames[wrap12 (pitchClass)];
}

juce::String KeyScale::scaleTypeName (ScaleType t)
{
    return defFor (t).name;
}

juce::StringArray KeyScale::allPitchClassNames()
{
    juce::StringArray a;
    for (auto* n : pitchNames)
        a.add (n);
    return a;
}

juce::StringArray KeyScale::allScaleTypeNames()
{
    juce::StringArray a;
    for (const auto& d : defs)
        a.add (d.name);
    return a;
}

juce::String KeyScale::getName() const
{
    if (isChromatic())
        return "Chromatic";
    juce::String name = type == ScaleType::Chromatic ? juce::String ("Custom") : pitchClassName (root) + " " + scaleTypeName (type);
    if (customised)
    {
        juce::StringArray added, removed;
        for (int pc = 0; pc < 12; ++pc)
            if ((toggles >> pc) & 1u)
                (((mask >> pc) & 1u) ? added : removed).add (pitchClassName (pc));
        if (type != ScaleType::Chromatic)
        {
            if (! added.isEmpty()) name << " +" << added.joinIntoString (" +");
            if (! removed.isEmpty()) name << " -" << removed.joinIntoString (" -");
        }
    }
    return name;
}

int KeyScale::getIntervals (ScaleType t, std::array<int, 12>& out) noexcept
{
    const auto& d = defFor (t);
    for (int i = 0; i < d.count; ++i)
        out[(size_t) i] = d.steps[i];
    return d.count;
}

} // namespace moontune
