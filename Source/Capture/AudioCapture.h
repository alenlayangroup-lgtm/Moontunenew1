#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "TakeAudio.h"

namespace moontune
{

// Records the plugin input while the host is playing and capture is armed
// (the non-ARA "transfer" workflow).
//
// Audio thread: pushBlock() downmixes to mono and copies into a pre-allocated
// lock-free FIFO together with the host timeline position. No allocation, no locks.
// Capture thread: drains the FIFO into the current TakeAudio. When data stops
// arriving (host stopped) or capture is disarmed, the session is finished and
// onSessionFinished is called (from the capture thread) with the captured range.
class AudioCapture : private juce::Thread
{
public:
    static constexpr double fifoSeconds = 4.0;
    static constexpr int idleMsToFinish = 300;

    AudioCapture();
    ~AudioCapture() override;

    // Call from prepareToPlay (not concurrently with pushBlock).
    void prepare (double sampleRate, int maximumBlockSize);
    void stop();   // stop the capture thread (owner calls this before tearing down)

    void setArmed (bool shouldBeArmed) noexcept;
    bool isArmed() const noexcept { return armed.load(); }
    bool isReceiving() const noexcept { return receiving.load(); }

    // Audio thread.
    void pushBlock (const juce::AudioBuffer<float>& buffer, int numInputChannels,
                    std::optional<juce::int64> timelineSample, bool hostIsPlaying) noexcept;

    // Current take (created on first capture). Shared with the analysis.
    std::shared_ptr<TakeAudio> getTake() const;
    void setTake (std::shared_ptr<TakeAudio>);   // restore from disk
    void clearTake();

    // Called on the capture thread when a session ends. Argument: timeline range covered.
    std::function<void (TakeAudio::Range)> onSessionFinished;

    double getSessionSeconds() const noexcept { return sessionSeconds.load(); }
    int getDroppedBlocks() const noexcept { return droppedBlocks.load(); }

    // Tests / offline use: drain everything synchronously and finish the session.
    void flushForTesting();

private:
    struct Header { juce::int64 timelineStart; int numSamples; };

    void run() override;
    bool drainOnce();          // returns true if any data was processed
    void finishSession();

    // FIFO (audio thread -> capture thread)
    juce::AbstractFifo sampleFifo { 1 }, headerFifo { 1 };
    std::vector<float> sampleRing;
    std::vector<Header> headerRing;
    std::vector<float> monoScratch;     // audio thread only
    std::vector<float> drainScratch;    // capture thread only

    std::atomic<bool> armed { false }, receiving { false };
    std::atomic<int> droppedBlocks { 0 };
    std::atomic<double> sessionSeconds { 0.0 };
    juce::int64 freeRunningPosition = 0;   // used when the host gives no timeline position
    double sampleRate = 44100.0;

    mutable std::mutex takeMutex;   // guards `take` pointer and session bookkeeping
    std::shared_ptr<TakeAudio> take;
    bool sessionActive = false;
    TakeAudio::Range sessionRange;
    juce::uint32 lastDataTime = 0;

    std::mutex drainMutex;          // serialises drainOnce between thread and flushForTesting
    juce::WaitableEvent dataEvent;
};

} // namespace moontune
