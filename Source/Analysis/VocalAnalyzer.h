#pragma once

#include <atomic>
#include <functional>
#include "AnalysisResult.h"
#include "PitchDetector.h"
#include "Capture/TakeAudio.h"

namespace moontune
{

// Turns captured mono vocal into frames (pitch / level / voicing) and notes.
//
// Notes are segmented from the pitch track, not from spectral peaks:
//   - a note boundary is placed where the vibrato-smoothed pitch moves to a new level
//     and stays there (so vibrato and small drift never split a note);
//   - re-articulations of the same pitch are split at clear level dips;
//   - very short fragments are merged into the neighbour with the closest pitch.
// Each note gets a centre pitch (confidence-weighted median of its stable middle part),
// vibrato rate/extent if present, and a reliability flag.
//
// Runs on a background thread; never on the audio thread.
struct AnalyzerSettings
{
    PitchDetectorSettings pitch;
    double minNoteSeconds = 0.07;
    float noteChangeSemitones = 0.6f;   // sustained deviation that starts a new note
    double noteChangeHoldSeconds = 0.04;
    float rearticulationDipDb = 6.0f;
};

class VocalAnalyzer
{
public:
    using Settings = AnalyzerSettings;

    static AnalysisResult analyse (const TakeAudio& take, const Settings& settings = {},
                                   const std::atomic<bool>* cancel = nullptr,
                                   const std::function<void (float)>& progress = {});

    // Analyse a plain buffer whose first sample is at timeline position 0 (tests, offline).
    static AnalysisResult analyseBuffer (const float* samples, int numSamples, double sampleRate,
                                         const Settings& settings = {});

    // Re-derives the centre pitch of [start, end) from frames (used after manual split/merge).
    static float centrePitch (const AnalysisResult&, double startSeconds, double endSeconds, float fallback);

private:
    struct Span { juce::int64 start, end; };   // in samples
    static AnalysisResult run (const std::function<void (juce::int64, float*, int)>& reader,
                               const std::vector<Span>& spans, juce::int64 totalSamples, double sampleRate,
                               const Settings&, const std::atomic<bool>* cancel,
                               const std::function<void (float)>& progress);
    static void segmentNotes (AnalysisResult&, const Settings&);
};

} // namespace moontune
