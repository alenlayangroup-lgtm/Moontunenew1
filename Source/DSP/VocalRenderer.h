#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "Analysis/AnalysisResult.h"
#include "Capture/TakeAudio.h"
#include "Model/NoteModel.h"

namespace moontune
{

// Sound engines (both share the same phase-locked analysis of the voice).
namespace Engine
{
    constexpr int harmonyClean = 0;  // harmonics re-synthesised at the new pitch, breath/noise layer kept apart
    constexpr int trueVoice = 1;     // pitch-synchronous overlap-add of the real waveform (phase-locked marks)
    constexpr int hybrid = 2;        // True Voice, then only its harmonics kept + the breath layer (clean and natural)
}

// Which processing stages are applied (each can be bypassed for A/B listening).
struct RenderOptions
{
    bool pitch = true;            // note pitch edits
    bool time = true;             // note timing / length edits
    bool preserveFormants = true; // off: formants follow the pitch (chipmunk / giant effect)
    bool eq = true;               // per-note EQ
    bool protectConsonants = true;// consonants/breaths are not pitch-shifted and keep their length
    bool scaleCorrection = true;  // the "Correct Pitch" layer
    int engine = 0;               // Engine::harmonyClean or Engine::trueVoice

    bool operator== (const RenderOptions& o) const noexcept
    {
        return pitch == o.pitch && time == o.time && preserveFormants == o.preserveFormants
            && eq == o.eq && protectConsonants == o.protectConsonants && scaleCorrection == o.scaleCorrection
            && engine == o.engine;
    }
    bool operator!= (const RenderOptions& o) const noexcept { return ! (*this == o); }
};

// The edited vocal, on the same timeline as the captured take.
// Immutable once published; the audio thread only reads it.
struct RenderedAudio
{
    double sampleRate = 44100.0;
    std::vector<float> samples;                // index = timeline sample
    std::vector<TakeAudio::Range> ranges;      // where `samples` replaces the input
    juce::uint64 generation = 0;

    // plain = an unedited copy of the take (shared buffer): used for audition and ARA playback,
    // never to replace the host's input.
    bool plain = false;
    std::shared_ptr<const std::vector<float>> sharedSamples;
    std::shared_ptr<const std::vector<float>> original;   // the unedited take (mono)
    std::shared_ptr<const std::vector<std::vector<float>>> channels;   // ARA: original channels when they differ

    // The original channel `ch` at `pos` (falls back to the mono take).
    float originalChannel (juce::int64 pos, int ch, float mono) const noexcept
    {
        if (channels == nullptr || channels->empty()) return mono;
        const auto& c = (*channels)[(size_t) std::min (ch, (int) channels->size() - 1)];
        return (size_t) pos < c.size() ? c[(size_t) pos] : mono;
    }

    bool originalAt (juce::int64 pos, float& out) const noexcept
    {
        if (original == nullptr) return false;
        for (const auto& r : ranges)
            if (pos >= r.start && pos < r.end)
            {
                out = (size_t) pos < original->size() ? (*original)[(size_t) pos] : 0.0f;
                return true;
            }
        return false;
    }

    // Returns false when `pos` is not covered (the input should be used there).
    bool sampleAt (juce::int64 pos, float& out) const noexcept
    {
        const auto& v = sharedSamples != nullptr ? *sharedSamples : samples;
        for (const auto& r : ranges)
            if (pos >= r.start && pos < r.end)
            {
                out = (size_t) pos < v.size() ? v[(size_t) pos] : 0.0f;
                return true;
            }
        return false;
    }
};

// Analysis of the voice that does not depend on edits (fundamental phase, harmonic frames).
// Keep one alive between renders of the same take + analysis so an edit only costs the
// re-synthesis; clear() it when the take or the analysis changes. Used by one thread at a time.
class VoiceModelCache
{
public:
    VoiceModelCache();
    ~VoiceModelCache();
    void clear();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Offline (background-thread) renderer of the edited vocal.
//
// Analysis (shared): the fundamental phase of the voice is integrated from the pitch track,
// and every 2 ms the voice is fitted with harmonics locked to that phase (least squares over
// three periods). This gives a precise, jitter-free position of every glottal period and a
// clean split of the voice into harmonics and a residual (breath, noise, consonants).
//
// Harmony Clean: the harmonics are re-synthesised at the new pitch; their amplitudes and
// phases are read from the spectral envelope at the new harmonic frequencies, so formants
// stay in place. The residual is moved with the voice by phase-locked PSOLA. No grain
// splicing of the voiced part -> no doubled voice, no clicks.
//
// True Voice: PSOLA of the real waveform on the phase-locked marks; the level follows the
// original.
//
// In both, only the offset of a note is changed (vibrato and transitions stay), time edits
// follow a TimeMap with consonant protection, unvoiced audio is never re-pitched, and audio
// outside edits is copied sample for sample. After an edit the extra phase of the voice is
// drained back to the original within the next voiced audio, so untouched notes return
// bit-exact.
class VocalRenderer
{
public:
    static bool needsRender (const std::vector<Note>& notes, const RenderOptions&);

    // Returns nullptr if nothing has to be rendered or if cancelled.
    static std::shared_ptr<RenderedAudio> render (const TakeAudio& take, const AnalysisResult& analysis,
                                                  const std::vector<Note>& notes, const RenderOptions& options,
                                                  const std::atomic<bool>* cancel = nullptr,
                                                  VoiceModelCache* cache = nullptr);

    // Output pitch curve of a note as it will sound (for drawing), in MIDI note numbers.
    // Samples one value per analysis frame of the note's source range; NaN where unvoiced.
    static std::vector<float> notePitchCurve (const Note&, const AnalysisResult&, const RenderOptions&);
};

} // namespace moontune
