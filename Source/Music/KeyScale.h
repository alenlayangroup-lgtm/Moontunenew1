#pragma once

#include <juce_core/juce_core.h>
#include <array>

namespace moontune
{

// Musical key / scale logic (the ScaleManager of the spec).
//
// Pure, allocation-free, thread-safe value type. It only answers music-theory
// questions; it does NOT detect keys, draw anything, or touch audio. The four
// spec functions stay separate:
//   1. key detection           -> KeyDetector (later, needs analysed notes, Stage 2)
//   2. highlighting            -> NoteGridView, asks KeyScale::containsMidi()
//   3. snap while dragging     -> KeyScale::snap(), called by the editing tool (Stage 3)
//   4. automatic audio correction -> separate DSP module (Stage 4), uses KeyScale only
//      as a source of target pitches
enum class ScaleType
{
    Major = 0,
    NaturalMinor,
    HarmonicMinor,
    MelodicMinor,   // ascending ("jazz") form: 1 2 b3 4 5 6 7
    Dorian,
    Phrygian,
    Lydian,
    Mixolydian,
    Locrian,
    Chromatic,      // no restriction: every semitone is "in scale"
    numTypes
};

class KeyScale
{
public:
    constexpr KeyScale() noexcept = default;
    KeyScale (int rootPitchClass, ScaleType type) noexcept;

    int getRoot() const noexcept { return root; }            // 0 = C ... 11 = B
    ScaleType getType() const noexcept { return type; }
    bool isChromatic() const noexcept { return type == ScaleType::Chromatic && ! customised; }

    // Bit n set = pitch class n (absolute, C = 0) belongs to the scale.
    juce::uint16 getPitchClassMask() const noexcept { return mask; }

    bool containsPitchClass (int pitchClass) const noexcept;
    bool containsMidi (int midiNote) const noexcept;
    bool isRoot (int midiNote) const noexcept;

    // Nearest in-scale MIDI note to a (fractional) pitch.
    // On an exact tie (e.g. C# in C major: C and D are equally far) the
    // direction hint decides: > 0 picks the upper note, < 0 the lower,
    // 0 picks the lower one (deterministic).
    int nearestScaleNote (float midiPitch, int directionHint = 0) const noexcept;

    struct SnapResult
    {
        float pitch = 0.0f;         // resulting pitch (MIDI, fractional)
        int   targetNote = 0;       // in-scale note that attracted it
        float correctionCents = 0;  // resulting pitch - input pitch, in cents
    };

    // Pulls `midiPitch` toward the nearest scale note.
    // strength 0 = unchanged, 1 = exactly on the scale note; values in between are
    // a proportional pull, so intentional bends/chromatic notes can be kept by
    // simply lowering the strength (or switching snap off).
    SnapResult snap (float midiPitch, float strength, int directionHint = 0) const noexcept;

    // Names
    static juce::String pitchClassName (int pitchClass);               // "C", "C#", ...
    static juce::String scaleTypeName (ScaleType type);                // "Major", "Dorian", ...
    static juce::StringArray allPitchClassNames();
    static juce::StringArray allScaleTypeNames();
    juce::String getName() const;                                      // "D Dorian"

    // Scale steps in semitones from the root (Chromatic: 0..11). Returns the count.
    static int getIntervals (ScaleType type, std::array<int, 12>& out) noexcept;

    // Custom scale: the given pitch classes (bit n = pitch class n) are added to / removed from
    // the scale (e.g. A-flat allowed in A minor). The root always stays in.
    KeyScale withPitchClassesToggled (juce::uint16 toggles) const noexcept;
    bool isCustomised() const noexcept { return customised; }
    juce::uint16 getToggles() const noexcept { return toggles; }

    bool operator== (const KeyScale& o) const noexcept { return root == o.root && type == o.type && mask == o.mask; }
    bool operator!= (const KeyScale& o) const noexcept { return ! (*this == o); }

private:
    int root = 0;
    ScaleType type = ScaleType::Chromatic;
    juce::uint16 mask = 0x0fff;
    juce::uint16 toggles = 0;
    bool customised = false;
};

} // namespace moontune
