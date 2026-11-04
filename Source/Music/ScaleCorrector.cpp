#include "ScaleCorrector.h"

#include <cmath>

namespace moontune
{

std::vector<Note> ScaleCorrector::apply (std::vector<Note> notes, const std::set<juce::int64>& ids,
                                         const KeyScale& scale, const Settings& s)
{
    const float strength = juce::jlimit (0.0f, 1.0f, s.strength);
    for (auto& n : notes)
    {
        if ((! ids.empty() && ids.count (n.id) == 0) || ! n.isNote())
            continue;

        // Target from the pitch the user has set (detected + manual shift), not the correction.
        const float current = n.detectedPitch + n.pitchShiftCents / 100.0f;
        const auto snapped = scale.snap (current, 1.0f);
        const float distanceCents = (snapped.pitch - current) * 100.0f;

        const bool skip = n.duration() < s.minNoteSeconds || std::abs (distanceCents) <= s.tolerance;
        n.correctionCents = skip ? 0.0f : distanceCents * strength;
        n.correctionDrift = skip ? 0.0f : juce::jlimit (0.0f, 1.0f, s.driftStrength);
        n.correctionSpeedMs = juce::jlimit (0.0f, 1000.0f, s.speedMs);
        NoteModel::clampEdits (n);
    }
    return notes;
}

std::vector<Note> ScaleCorrector::remove (std::vector<Note> notes, const std::set<juce::int64>& ids)
{
    for (auto& n : notes)
        if (ids.empty() || ids.count (n.id) > 0)
        {
            n.correctionCents = 0.0f;
            n.correctionDrift = 0.0f;
            n.correctionSpeedMs = 0.0f;
        }
    return notes;
}

} // namespace moontune
