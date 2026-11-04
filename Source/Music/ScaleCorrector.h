#pragma once

#include <set>
#include <vector>
#include "KeyScale.h"
#include "Model/NoteModel.h"

namespace moontune
{

// "Correct Pitch": automatic correction of notes toward the selected scale.
//
// It is a macro, not a hidden process: it writes the correction layer of each note
// (correctionCents / correctionDrift / correctionSpeedMs), which the user can see,
// edit, undo, and switch off as a whole with the Scale Correction toggle.
// Notes are never changed just because they are outside the scale unless this is run.
struct ScaleCorrector
{
    struct Settings
    {
        float strength = 1.0f;        // 0..1 pull of the note centre toward the target note
        float driftStrength = 0.0f;   // 0..1 straightening of slow drift (vibrato is never touched)
        float speedMs = 0.0f;         // 0 = instant; larger = correction fades in at the note start
        float tolerance = 0.0f;       // cents: notes already this close are left alone
        double minNoteSeconds = 0.0;  // shorter notes (passing tones, ornaments) are left alone
    };

    // Applies to the notes whose ids are in `ids` (all notes if empty). Returns the new list.
    static std::vector<Note> apply (std::vector<Note> notes, const std::set<juce::int64>& ids,
                                    const KeyScale& scale, const Settings& settings);

    // Removes the correction layer from the given notes (all if empty).
    static std::vector<Note> remove (std::vector<Note> notes, const std::set<juce::int64>& ids);
};

} // namespace moontune
