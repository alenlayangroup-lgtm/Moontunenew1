#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <mutex>
#include <vector>

namespace moontune
{

// Captured mono vocal, addressed by host timeline position (in samples at the take's rate).
//
// Storage is chunked and allocated lazily, so capturing 30 s at minute 3 of a song
// only allocates those 30 s. Parts of the timeline that were never captured read as
// silence and are not listed in getRanges().
//
// Thread use: one writer (the capture thread) and readers (analysis, saving). All
// public methods lock an internal mutex; none of them is called on the audio thread.
class TakeAudio
{
public:
    struct Range
    {
        juce::int64 start = 0, end = 0;   // [start, end) in samples
        juce::int64 length() const noexcept { return end - start; }
        bool operator== (const Range& o) const noexcept { return start == o.start && end == o.end; }
    };

    static constexpr int chunkSize = 1 << 16;
    static constexpr double maxSeconds = 30.0 * 60.0;   // timeline positions beyond 30 min are ignored

    TakeAudio (double sampleRate, juce::String id = {});

    double getSampleRate() const noexcept { return sampleRate; }
    const juce::String& getId() const noexcept { return id; }

    // Writes samples at a timeline position. Returns the number of samples actually stored
    // (fewer if the write runs past maxSeconds or starts before 0).
    int write (juce::int64 startSample, const float* data, int numSamples);

    // Copies samples into dest; uncaptured positions become 0.
    void read (juce::int64 startSample, float* dest, int numSamples) const;
    std::vector<float> readRange (Range) const;

    std::vector<Range> getRanges() const;            // merged, sorted
    juce::int64 getEndSample() const;                // end of the last range (0 if empty)
    double getCapturedSeconds() const;               // total captured duration
    bool isEmpty() const;

    void setRanges (std::vector<Range>);             // used when loading from disk

    // ARA: the original channels of the events (timeline-indexed, all the same length), kept only
    // when they differ from each other. The mono take is what is analysed and edited.
    using Channels = std::vector<std::vector<float>>;
    void setOriginalChannels (std::shared_ptr<const Channels> c) { const std::lock_guard<std::mutex> l (mutex); originalChannels = std::move (c); }
    std::shared_ptr<const Channels> getOriginalChannels() const { const std::lock_guard<std::mutex> l (mutex); return originalChannels; }

    // 64-bit content hash over the captured ranges.
    juce::uint64 computeHash() const;

    // Disk persistence: FLAC (24-bit) of [0, end); ranges are stored by the caller.
    bool saveFlac (const juce::File& file) const;
    static std::shared_ptr<TakeAudio> loadFlac (const juce::File& file, const juce::String& id,
                                                std::vector<Range> ranges);

private:
    float* chunkForWrite (juce::int64 chunkIndex);
    static void addRange (std::vector<Range>& ranges, Range r);

    const double sampleRate;
    const juce::String id;
    const juce::int64 maxSamples;

    mutable std::mutex mutex;
    std::vector<std::unique_ptr<float[]>> chunks;
    std::vector<Range> ranges;
    std::shared_ptr<const Channels> originalChannels;
};

} // namespace moontune
