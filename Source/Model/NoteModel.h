#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <atomic>
#include <vector>

namespace moontune
{

// One band of a note's 4-band EQ.
struct EqBand
{
    enum Type { peak = 0, lowShelf, highShelf, lowCut, highCut, numTypes };

    bool  enabled = false;
    int   type = peak;
    float frequency = 1000.0f;   // Hz, 20 .. 20000
    float gainDb = 0.0f;         // -18 .. +18 (ignored by cut filters)
    float q = 1.0f;              // 0.2 .. 10

    bool operator== (const EqBand& o) const noexcept;
    bool operator!= (const EqBand& o) const noexcept { return ! (*this == o); }
    bool isActive() const noexcept;   // enabled and actually changes the sound
};

// Per-note EQ. Off by default; switched on only when a note needs it.
struct NoteEq
{
    bool enabled = false;
    std::array<EqBand, 4> bands { defaultBand (0), defaultBand (1), defaultBand (2), defaultBand (3) };

    static EqBand defaultBand (int index) noexcept;
    bool isActive() const noexcept;
    bool operator== (const NoteEq& o) const noexcept;
    bool operator!= (const NoteEq& o) const noexcept { return ! (*this == o); }
};

// One detected / edited vocal note.
//
// Source fields (from analysis): id, startSeconds, endSeconds, detectedPitch.
// Everything else is an edit; all edits default to "no change", so an unedited note
// renders as the original recording, bit for bit.
struct Note
{
    // What the object is. Breaths and consonants are separate objects so they can be made
    // quieter / louder / silent; pitch tools ignore them (they are never re-pitched).
    enum Kind { note = 0, breath, sibilant, plosive };

    juce::int64 id = 0;              // unique within a NoteModel, > 0
    int    kind = note;
    double startSeconds = 0.0;       // position in the source audio
    double endSeconds   = 0.0;       // must be > startSeconds
    float  detectedPitch = 60.0f;    // perceived centre pitch, MIDI note number (fractional)

    // --- Pitch -----------------------------------------------------------------
    float  pitchShiftCents = 0.0f;   // move of the note centre (user edit)
    float  pitchModulation = 1.0f;   // scale of fast pitch movement: vibrato, wobble (1 = keep, 0 = flat)
    float  pitchDrift = 1.0f;        // scale of slow pitch drift inside the note (1 = keep, 0 = straight)
    float  pitchTilt = 0.0f;         // cents from start to end added as a straight slope (0 = none)
    float  transitionMs = -1.0f;     // glide from the previous note into this one (-1 = natural default)

    // Pitch drawn with the pencil (like Auto-Tune's graphical mode): one value per drawHop of the
    // note's source audio from startSeconds, in cents relative to the note's centre (so it moves
    // with the note); NaN where nothing is drawn. Where drawn it replaces the sung pitch curve.
    static constexpr double drawHop = 0.005;
    std::vector<float> drawnCents;
    bool  hasDrawing() const noexcept;
    float drawnAt (double sourceSeconds) const noexcept;       // NaN where not drawn
    float drawnWeight (double sourceSeconds) const noexcept;   // 0..1, soft 20 ms edges of a drawn stretch

    // --- Time ------------------------------------------------------------------
    double timeShiftSeconds = 0.0;   // move of the whole note
    double startEditSeconds = 0.0;   // extra move of the start edge (stretches the note)
    double endEditSeconds = 0.0;     // extra move of the end edge

    // --- Sound -----------------------------------------------------------------
    float  formantCents = 0.0f;      // formant shift (0 = natural timbre preserved)
    float  gainDb = 0.0f;            // note level (-60 = silent)
    NoteEq eq;

    // --- Scale correction layer (written by "Correct Pitch", switchable as a whole) ---
    float  correctionCents = 0.0f;   // pull of the centre toward the scale note
    float  correctionDrift = 0.0f;   // 0..1 reduction of drift
    float  correctionSpeedMs = 0.0f; // how fast the correction takes hold at the note start (0 = at once)

    static constexpr float silentDb = -60.0f;   // gain at or below this = muted

    // Derived
    bool isNote() const noexcept { return kind == note; }
    double outStart() const noexcept { return startSeconds + timeShiftSeconds + startEditSeconds; }
    double outEnd() const noexcept   { return endSeconds + timeShiftSeconds + endEditSeconds; }
    double duration() const noexcept { return endSeconds - startSeconds; }
    float  editedPitch (bool withCorrection = true) const noexcept
    {
        return detectedPitch + (pitchShiftCents + (withCorrection ? correctionCents : 0.0f)) / 100.0f;
    }
    bool hasTimeEdit() const noexcept;
    bool hasSoundEdit() const noexcept;   // anything that changes the rendered audio
    bool hasCorrection() const noexcept;

    bool operator== (const Note& other) const noexcept;
    bool operator!= (const Note& other) const noexcept { return ! (*this == other); }
};

// Thread-safe container for the note list.
// The audio thread never touches it; the renderer works on snapshots.
class NoteModel
{
public:
    static constexpr float  maxPitchShiftCents = 2400.0f;   // +-2 octaves
    static constexpr double maxTimeShiftSeconds = 60.0;
    static constexpr double minNoteSeconds = 0.02;

    static constexpr float maxDrawnCents = 2400.0f;

    std::vector<Note> getSnapshot() const;
    void replaceAll (std::vector<Note> notes);
    void clear();
    int size() const;      // musical notes only (breaths / consonants not counted)
    int sizeAll() const;   // every object

    // Incremented on every change, so views can cheaply check whether to refresh.
    juce::uint32 getVersion() const noexcept { return version.load(); }

    // An id larger than every id currently in the model.
    juce::int64 nextFreeId() const;

    // Serialisation. fromValueTree() skips invalid entries and returns how many it rejected.
    juce::ValueTree toValueTree() const;
    int fromValueTree (const juce::ValueTree& tree);

    static bool isValid (const Note& note) noexcept;
    static void clampEdits (Note& note) noexcept;   // bring edit fields into their legal ranges
    static const juce::Identifier treeType;

private:
    mutable juce::CriticalSection lock;
    std::vector<Note> notes;
    std::atomic<juce::uint32> version { 0 };
};

} // namespace moontune
