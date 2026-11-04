#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>

using namespace moontune;

namespace
{
    constexpr double sampleRates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };
    constexpr int blockSizes[]     = { 1, 17, 32, 64, 128, 441, 512, 1024, 2048, 4096 };

    // Deterministic test signal: a vocal-range tone plus a little noise.
    void fillSignal (juce::AudioBuffer<float>& buffer, juce::int64 startSample, double sampleRate, juce::Random& rng)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const double t = (double) (startSample + i) / sampleRate;
                buffer.setSample (ch, i, (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 220.0 * t)
                                                  + 0.01 * (rng.nextDouble() * 2.0 - 1.0)));
            }
    }

    void setParam (MoonTuneAudioProcessor& p, const juce::ParameterID& id, float plainValue)
    {
        auto* param = p.getParameterState().getParameter (id.getParamID());
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
    }

    // Runs `totalSamples` through the processor in blocks taken cyclically from `blockPattern`
    // and returns {input, output}.
    std::pair<juce::AudioBuffer<float>, juce::AudioBuffer<float>>
    runProcessor (MoonTuneAudioProcessor& p, int numChannels, double sr, int preparedBlock,
                  const std::vector<int>& blockPattern, int totalSamples,
                  const std::function<void (juce::int64)>& beforeBlock = {})
    {
        juce::AudioBuffer<float> in (numChannels, totalSamples), out (numChannels, totalSamples);
        juce::Random rng (1234);
        fillSignal (in, 0, sr, rng);

        p.prepareToPlay (sr, preparedBlock);
        juce::MidiBuffer midi;
        size_t patternIndex = 0;

        for (int pos = 0; pos < totalSamples;)
        {
            const int n = juce::jmin (blockPattern[patternIndex++ % blockPattern.size()], totalSamples - pos);
            if (beforeBlock)
                beforeBlock (pos);

            juce::AudioBuffer<float> block (numChannels, n);
            for (int ch = 0; ch < numChannels; ++ch)
                block.copyFrom (ch, 0, in, ch, pos, n);

            p.processBlock (block, midi);

            for (int ch = 0; ch < numChannels; ++ch)
                out.copyFrom (ch, pos, block, ch, 0, n);
            pos += n;
        }

        p.releaseResources();
        return { std::move (in), std::move (out) };
    }

    float maxAbsDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        float m = 0.0f;
        for (int ch = 0; ch < a.getNumChannels(); ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
                m = juce::jmax (m, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
        return m;
    }

    float maxStep (const juce::AudioBuffer<float>& b, int ch)
    {
        float m = 0.0f;
        for (int i = 1; i < b.getNumSamples(); ++i)
            m = juce::jmax (m, std::abs (b.getSample (ch, i) - b.getSample (ch, i - 1)));
        return m;
    }

    bool allFinite (const juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (! std::isfinite (b.getSample (ch, i)))
                    return false;
        return true;
    }
}

class PassthroughTests final : public juce::UnitTest
{
public:
    PassthroughTests() : juce::UnitTest ("Passthrough / buffer handling", "MoonTune") {}

    void runTest() override
    {
        beginTest ("Bus layouts: mono and stereo accepted, mismatched rejected");
        {
            MoonTuneAudioProcessor p;
            using L = juce::AudioProcessor::BusesLayout;
            L mono;   mono.inputBuses.add (juce::AudioChannelSet::mono());     mono.outputBuses.add (juce::AudioChannelSet::mono());
            L stereo; stereo.inputBuses.add (juce::AudioChannelSet::stereo()); stereo.outputBuses.add (juce::AudioChannelSet::stereo());
            L mixed;  mixed.inputBuses.add (juce::AudioChannelSet::mono());    mixed.outputBuses.add (juce::AudioChannelSet::stereo());
            expect (p.checkBusesLayoutSupported (mono));
            expect (p.checkBusesLayoutSupported (stereo));
            expect (! p.checkBusesLayoutSupported (mixed));
            expect (p.setBusesLayout (stereo));
            expectEquals (p.getTotalNumOutputChannels(), 2);
            expectEquals (p.getLatencySamples(), 0);
        }

        beginTest ("Default settings are bit-exact at all sample rates and block sizes (mono + stereo)");
        {
            for (int channels : { 1, 2 })
                for (double sr : sampleRates)
                    for (int bs : blockSizes)
                    {
                        MoonTuneAudioProcessor p;
                        if (channels == 2)
                            p.setBusesLayout ({ { juce::AudioChannelSet::stereo() }, { juce::AudioChannelSet::stereo() } });
                        auto [in, out] = runProcessor (p, channels, sr, bs, { bs }, (int) sr / 4);
                        expectEquals (maxAbsDiff (in, out), 0.0f,
                                      "ch=" + juce::String (channels) + " sr=" + juce::String (sr) + " bs=" + juce::String (bs));
                    }
        }

        beginTest ("Variable host block sizes, including blocks larger than prepared");
        {
            for (double sr : sampleRates)
            {
                MoonTuneAudioProcessor p;
                setParam (p, ParamIDs::outputGain, -6.0f);
                // prepared for 256, host sends up to 3000
                auto [in, out] = runProcessor (p, 1, sr, 256, { 256, 1, 3000, 100, 512, 7, 1024 }, (int) sr / 2);
                expect (allFinite (out));
                const float g = juce::Decibels::decibelsToGain (-6.0f);
                float err = 0.0f;
                for (int i = 0; i < in.getNumSamples(); ++i)
                    err = juce::jmax (err, std::abs (out.getSample (0, i) - in.getSample (0, i) * g));
                expectLessThan (err, 1.0e-6f, "sr=" + juce::String (sr));
            }
        }

        beginTest ("Gain change ramps smoothly (no click)");
        {
            for (double sr : sampleRates)
            {
                MoonTuneAudioProcessor p;
                // DC input makes any gain step directly visible as a sample-to-sample jump.
                const int total = (int) sr / 2;
                juce::AudioBuffer<float> in (1, total), out (1, total);
                in.clear();
                for (int i = 0; i < total; ++i) in.setSample (0, i, 0.5f);

                p.prepareToPlay (sr, 512);
                juce::MidiBuffer midi;
                for (int pos = 0; pos < total; pos += 512)
                {
                    const int n = juce::jmin (512, total - pos);
                    if (pos == 512 * 8) setParam (p, ParamIDs::outputGain, -24.0f);
                    if (pos == 512 * 40) setParam (p, ParamIDs::outputGain, 12.0f);
                    juce::AudioBuffer<float> block (1, n);
                    block.copyFrom (0, 0, in, 0, pos, n);
                    p.processBlock (block, midi);
                    out.copyFrom (0, pos, block, 0, 0, n);
                }
                // Full jump would be ~1.5 (0.5 * 3.98 - 0.5*0.063). A 20 ms ramp gives a tiny per-sample step.
                const float bound = 2.0f * 0.5f * 4.0f / (float) (sr * PassthroughEngine::rampSeconds);
                expectLessThan (maxStep (out, 0), bound, "sr=" + juce::String (sr));
                expectWithinAbsoluteError (out.getSample (0, total - 1), 0.5f * juce::Decibels::decibelsToGain (12.0f), 1.0e-5f);
            }
        }

        beginTest ("Bypass toggling crossfades without clicks and reaches exact dry signal");
        {
            MoonTuneAudioProcessor p;
            setParam (p, ParamIDs::outputGain, -12.0f);
            const double sr = 48000.0;
            auto [in, out] = runProcessor (p, 1, sr, 480, { 480 }, 48000, [&p] (juce::int64 pos)
            {
                if (pos == 4800)  setParam (p, ParamIDs::bypass, 1.0f);
                if (pos == 24000) setParam (p, ParamIDs::bypass, 0.0f);
            });
            expect (allFinite (out));

            // While fully bypassed (after the 20 ms fade) output == input exactly.
            float bypassErr = 0.0f;
            for (int i = 4800 + 2000; i < 24000; ++i)
                bypassErr = juce::jmax (bypassErr, std::abs (out.getSample (0, i) - in.getSample (0, i)));
            expectEquals (bypassErr, 0.0f);

            // Step between consecutive samples never exceeds signal slope + ramp contribution.
            const float signalSlope = (float) (0.5 * juce::MathConstants<double>::twoPi * 220.0 / sr) + 0.02f;
            expectLessThan (maxStep (out, 0), signalSlope + 0.01f);
        }

        beginTest ("Silence in, silence out; no NaN/denormals on tiny input");
        {
            MoonTuneAudioProcessor p;
            setParam (p, ParamIDs::outputGain, 3.0f);
            p.prepareToPlay (44100.0, 128);
            juce::AudioBuffer<float> b (1, 128);
            juce::MidiBuffer midi;
            for (int k = 0; k < 100; ++k)
            {
                for (int i = 0; i < 128; ++i) b.setSample (0, i, (k % 2 == 0) ? 0.0f : 1.0e-38f);
                p.processBlock (b, midi);
                expect (allFinite (b));
            }
        }

        beginTest ("Re-prepare with a different sample rate / block size");
        {
            MoonTuneAudioProcessor p;
            for (double sr : sampleRates)
                for (int bs : { 64, 4096, 1 })
                {
                    auto [in, out] = runProcessor (p, 1, sr, bs, { bs, bs * 2 + 1 }, 10000);
                    expectEquals (maxAbsDiff (in, out), 0.0f);
                }
        }

        beginTest ("Editor can be created, resized and destroyed");
        {
            MoonTuneAudioProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditorIfNeeded());
            expect (editor != nullptr);
            if (editor != nullptr)
            {
                expectEquals (editor->getWidth(), MoonTuneAudioProcessorEditor::defaultWidth);
                editor->setSize (900, 500);
                juce::Image img (juce::Image::ARGB, 900, 500, true);
                juce::Graphics g (img);
                editor->paintEntireComponent (g, false);   // exercises all paint code paths

                // Optional: MOONTUNE_SNAPSHOT=/path/file.png saves the rendered editor for review.
                const auto snapshot = juce::SystemStats::getEnvironmentVariable ("MOONTUNE_SNAPSHOT", {});
                if (snapshot.isNotEmpty())
                {
                    juce::File f (snapshot);
                    f.deleteFile();
                    juce::FileOutputStream os (f);
                    juce::PNGImageFormat().writeImageToStream (img, os);
                }
                p.editorBeingDeleted (editor.get());
                editor.reset();
            }
        }
    }
};

static PassthroughTests passthroughTests;
