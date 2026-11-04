#pragma once

#include <vector>
#include "KeyScale.h"
#include "Model/NoteModel.h"

namespace moontune
{

// Suggests the most likely key from the sung notes (Analyze Key).
//
// Method: a pitch-class histogram weighted by note duration (long, stable notes count
// more; notes that end a phrase get extra weight because phrases tend to resolve to the
// tonic), correlated with the Krumhansl-Kessler major and minor key profiles for all
// 24 keys. Scores are turned into relative confidences.
//
// The result is a suggestion, never a certainty: with little or ambiguous material the
// detector says so, and relative major/minor (e.g. C major / A minor share all notes)
// is reported as ambiguous when the vocal alone cannot separate them.
// Only major / natural minor are suggested; modes can be chosen manually.
class KeyDetector
{
public:
    struct Candidate
    {
        KeyScale key;
        float correlation = 0.0f;   // -1..1
        float confidence = 0.0f;    // 0..1, relative among the 24 keys
    };

    struct Result
    {
        std::vector<Candidate> candidates;   // best first; up to 4
        bool enoughData = false;
        bool ambiguous = true;
        bool relativeAmbiguity = false;      // best two are relative major/minor
        double sungSeconds = 0.0;
        int noteCount = 0;
        juce::String summary;                // one-line explanation for the user

        bool hasSuggestion() const noexcept { return ! candidates.empty(); }
    };

    static Result detect (const std::vector<Note>& notes);

    static constexpr double minSecondsForConfidence = 15.0;
    static constexpr int minNotesForConfidence = 16;
};

} // namespace moontune
