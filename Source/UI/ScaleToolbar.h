#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include "Music/KeyScale.h"

namespace moontune
{

class MoonTuneAudioProcessor;

// Second toolbar row: Key, Scale, Snap to Scale (+ strength), Highlight amount.
// Only edits parameters and reports display changes; contains no music logic itself.
class ScaleToolbar final : public juce::Component
{
public:
    explicit ScaleToolbar (MoonTuneAudioProcessor&);
    ~ScaleToolbar() override;

    // Called on the message thread whenever key, scale or highlight amount changes.
    std::function<void (const KeyScale&, float highlight)> onScaleDisplayChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int preferredHeight = 38;

private:
    void parameterChanged();
    void updateEnablement();
    float plainValue (const juce::ParameterID&) const;
    KeyScale currentScale() const;

    MoonTuneAudioProcessor& processor;

    juce::Label keyLabel, scaleLabel, strengthLabel, highlightLabel;
    juce::ComboBox keyBox, scaleBox;
    juce::ToggleButton snapButton { "SNAP TO SCALE" };
    juce::Slider strengthSlider, highlightSlider;

    using APVTS = juce::AudioProcessorValueTreeState;
    std::unique_ptr<APVTS::ComboBoxAttachment> keyAttachment, scaleAttachment;
    std::unique_ptr<APVTS::ButtonAttachment> snapAttachment;
    std::unique_ptr<APVTS::SliderAttachment> strengthAttachment, highlightAttachment;

    // Message-thread notifications for display updates (no polling).
    std::unique_ptr<juce::ParameterAttachment> keyWatch, scaleWatch, highlightWatch, snapWatch, editsWatch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScaleToolbar)
};

} // namespace moontune
