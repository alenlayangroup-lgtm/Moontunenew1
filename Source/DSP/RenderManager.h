#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

#include "VocalRenderer.h"
#include "Analysis/AnalysisManager.h"
#include "Capture/AudioCapture.h"
#include "Model/NoteModel.h"

namespace moontune
{

// Keeps the rendered (edited) vocal up to date and plays it back.
//
// Render thread: watches the notes, the analysis, the captured take and the
// processing options; when something changed it waits a moment for more edits
// (a drag produces many) and renders in the background.
//
// Audio thread: process() replaces the input with the rendered vocal where the
// take was captured, following the host timeline. It never allocates, locks, or
// frees memory. Buffers are retired by the render thread only after the audio
// thread has confirmed it no longer uses them (hazard counter). A new render is
// faded in over 20 ms so edits during playback do not click.
class RenderManager : private juce::Thread
{
public:
    RenderManager (NoteModel&, AnalysisManager&, AudioCapture&);
    ~RenderManager() override;

    void stop();

    // Read on the render thread to get the current processing options.
    std::function<RenderOptions()> optionsProvider;

    // --- Audio thread ------------------------------------------------------------
    // Returns true if any rendered audio was written into `buffer`.
    bool process (juce::AudioBuffer<float>& buffer, int numChannels, juce::int64 timelineSample,
                  double hostSampleRate) noexcept;
    void resetPlayback() noexcept;   // host stopped: next start begins without a fade

    // Audition / ARA playback: writes the vocal (edited, or the plain take where nothing is edited)
    // into `buffer` at timeline position `timelineSample`. Returns true if anything was written.
    bool processAudition (juce::AudioBuffer<float>& buffer, int numChannels, juce::int64 timelineSample,
                          double hostSampleRate) noexcept;
    // ARA A/B: the unedited take at the timeline position (no crossfade state).
    bool processOriginal (juce::AudioBuffer<float>& buffer, int numChannels, juce::int64 timelineSample,
                          double hostSampleRate) noexcept;

    // --- Status (any thread) --------------------------------------------------------
    bool isRendering() const noexcept { return rendering.load(); }
    bool hasRenderedAudio() const noexcept { return publishedIsEdit.load(); }   // edits (not the plain take)
    juce::uint64 getGeneration() const noexcept { return publishedGeneration.load(); }

    // Tests: block until everything requested so far is rendered and published.
    bool waitUntilRendered (int timeoutMs);
    std::shared_ptr<const RenderedAudio> getRenderedForTesting() const;

    static constexpr int debounceMs = 25;

private:
    struct Inputs
    {
        juce::uint32 notesVersion = 0, resultVersion = 0;
        const void* take = nullptr;
        RenderOptions options;
        bool operator== (const Inputs& o) const noexcept
        {
            return notesVersion == o.notesVersion && resultVersion == o.resultVersion
                && take == o.take && options == o.options;
        }
    };

    void run() override;
    bool processImpl (juce::AudioBuffer<float>&, int numChannels, juce::int64 timelineSample,
                      double hostSampleRate, bool includePlain) noexcept;
    std::shared_ptr<RenderedAudio> makePlain (const std::shared_ptr<TakeAudio>&);
    void ensurePlainSamples (const std::shared_ptr<TakeAudio>&);
    Inputs currentInputs() const;
    void publish (std::shared_ptr<RenderedAudio>);
    void retireUnused();

    NoteModel& notes;
    AnalysisManager& analysis;
    AudioCapture& capture;

    // Handoff to the audio thread
    std::atomic<RenderedAudio*> published { nullptr };
    std::atomic<juce::uint64> publishedGeneration { 0 };
    std::atomic<juce::uint64> audioInUse { (std::numeric_limits<juce::uint64>::max)() };

    mutable std::mutex ownedMutex;   // render thread / tests only
    std::vector<std::shared_ptr<RenderedAudio>> owned;

    std::atomic<bool> rendering { false };
    std::atomic<bool> publishedIsEdit { false };
    std::atomic<bool> cancel { false };
    std::atomic<bool> upToDate { false };
    Inputs renderedInputs;
    VoiceModelCache voiceCache;          // render thread only
    std::shared_ptr<const std::vector<float>> plainSamples;   // render thread only
    const void* plainTake = nullptr;
    juce::uint32 plainVersion = 0;
    const void* cachedTake = nullptr;
    juce::uint32 cachedResultVersion = 0;
    bool renderedOnce = false;
    int deferred = 0;

    // Audio-thread state
    const RenderedAudio* current = nullptr;
    const RenderedAudio* previous = nullptr;
    int fadeRemaining = 0;
    bool haveCurrentFlag = false;
};

} // namespace moontune
