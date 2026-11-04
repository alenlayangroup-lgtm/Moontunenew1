#include "PassthroughEngine.h"

namespace moontune
{

void PassthroughEngine::prepare (double sampleRate, int maximumBlockSize, int numChannels)
{
    jassert (sampleRate > 0.0);

    maxBlock    = juce::jmax (1, maximumBlockSize);
    maxChannels = juce::jmax (1, numChannels);
    dryScratch.setSize (maxChannels, maxBlock, false, true, false);

    outputGain.reset (sampleRate, rampSeconds);
    bypassAmount.reset (sampleRate, rampSeconds);

    prepared = true;
}

void PassthroughEngine::reset() noexcept
{
    outputGain.setCurrentAndTargetValue (outputGain.getTargetValue());
    bypassAmount.setCurrentAndTargetValue (bypassAmount.getTargetValue());
}

void PassthroughEngine::setOutputGainDecibels (float gainDb) noexcept
{
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (gainDb, -120.0f));
}

void PassthroughEngine::setBypassed (bool shouldBypass) noexcept
{
    bypassAmount.setTargetValue (shouldBypass ? 1.0f : 0.0f);
}

void PassthroughEngine::process (juce::AudioBuffer<float>& buffer, const juce::AudioBuffer<float>* dry) noexcept
{
    if (! prepared)
        return;   // pass audio through untouched rather than touching unallocated memory

    const int numChannels = juce::jmin (buffer.getNumChannels(), maxChannels);
    const int numSamples  = buffer.getNumSamples();

    for (int start = 0; start < numSamples; start += maxBlock)
        processChunk (buffer, dry, start, juce::jmin (maxBlock, numSamples - start), numChannels);
}

void PassthroughEngine::processChunk (juce::AudioBuffer<float>& buffer, const juce::AudioBuffer<float>* externalDry,
                                      int startSample, int numSamples, int numChannels) noexcept
{
    const bool gainIsUnity = ! outputGain.isSmoothing() && juce::exactlyEqual (outputGain.getTargetValue(), 1.0f);
    const bool fullyActive = ! bypassAmount.isSmoothing() && juce::exactlyEqual (bypassAmount.getTargetValue(), 0.0f);
    const bool fullyBypassed = ! bypassAmount.isSmoothing() && juce::exactlyEqual (bypassAmount.getTargetValue(), 1.0f);

    // Fast paths keep the default state bit-exact (no rounding at all).
    if (fullyBypassed)
    {
        outputGain.skip (numSamples);   // keep the gain ramp moving while bypassed
        if (externalDry != nullptr)     // bypass = the original recording (A/B comparison)
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.copyFrom (ch, startSample, *externalDry, ch, startSample, numSamples);
        return;
    }

    if (fullyActive && gainIsUnity)
        return;

    if (fullyActive && ! outputGain.isSmoothing())
    {
        const float g = outputGain.getTargetValue();
        for (int ch = 0; ch < numChannels; ++ch)
            buffer.applyGain (ch, startSample, numSamples, g);
        return;
    }

    // General path: per-sample gain ramp and dry/processed crossfade.
    // processed = buffer (the edited vocal if any) * gain; dry = original input.
    for (int ch = 0; ch < numChannels; ++ch)
        dryScratch.copyFrom (ch, 0, buffer, ch, startSample, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        const float g   = outputGain.getNextValue();
        const float mix = bypassAmount.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float source    = dryScratch.getSample (ch, i);
            const float dry       = externalDry != nullptr ? externalDry->getSample (ch, startSample + i) : source;
            const float processed = source * g;
            buffer.setSample (ch, startSample + i, processed + (dry - processed) * mix);
        }
    }
}

} // namespace moontune
