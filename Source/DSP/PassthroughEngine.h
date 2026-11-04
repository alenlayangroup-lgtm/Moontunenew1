#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace moontune
{

// Stage 1 real-time signal path:
//
//     out = mix(processed, dry, bypassAmount)   with   processed = dry * outputGain
//
// "processed" is where the TimePitchEngine output will go from Stage 3 on.
// The bypass crossfade and the gain ramp are already sample-accurate, so
// toggling bypass or moving the gain never clicks.
//
// Real-time rules: no allocation, no locks, no I/O in process().
// All memory is allocated in prepare(); blocks larger than the prepared size
// are processed in chunks, so the engine is safe with any host buffer size.
class PassthroughEngine
{
public:
    static constexpr double rampSeconds = 0.02;   // 20 ms for gain and bypass

    void prepare (double sampleRate, int maximumBlockSize, int numChannels);
    void reset() noexcept;

    void setOutputGainDecibels (float gainDb) noexcept;
    void setBypassed (bool shouldBypass) noexcept;

    // `dry`: the original input when `buffer` already contains the edited vocal; bypass
    // then crossfades to it. nullptr: the buffer itself is the dry signal.
    void process (juce::AudioBuffer<float>& buffer, const juce::AudioBuffer<float>* dry = nullptr) noexcept;

    bool isPrepared() const noexcept { return prepared; }

private:
    void processChunk (juce::AudioBuffer<float>& buffer, const juce::AudioBuffer<float>* dry,
                       int startSample, int numSamples, int numChannels) noexcept;

    juce::AudioBuffer<float> dryScratch;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outputGain { 1.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bypassAmount { 0.0f };
    int maxBlock = 0;
    int maxChannels = 0;
    bool prepared = false;
};

} // namespace moontune
