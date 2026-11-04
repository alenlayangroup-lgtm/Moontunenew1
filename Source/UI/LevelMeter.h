#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>

namespace moontune
{

// Peak meter fed by a lock-free "pop peak" function from the processor.
class LevelMeter final : public juce::Component,
                         private juce::Timer
{
public:
    explicit LevelMeter (std::function<float()> peakSource);
    ~LevelMeter() override;

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    std::function<float()> source;
    float displayDb = -100.0f;
    float holdDb = -100.0f;
    int holdFrames = 0;
};

} // namespace moontune
