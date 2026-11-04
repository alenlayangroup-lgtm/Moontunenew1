#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace moontune
{

namespace Colours
{
    inline const juce::Colour background   { 0xff15171c };
    inline const juce::Colour panel        { 0xff1d2027 };
    inline const juce::Colour panelLight   { 0xff262a33 };
    inline const juce::Colour gridWhiteKey { 0xff1b1e24 };
    inline const juce::Colour gridBlackKey { 0xff16181d };
    inline const juce::Colour gridLine     { 0xff2a2e37 };
    inline const juce::Colour gridLineC    { 0xff3a404c };
    inline const juce::Colour text         { 0xffd9dce3 };
    inline const juce::Colour textDim      { 0xff8a909c };
    inline const juce::Colour accent       { 0xffd8b45a };   // moon gold
    inline const juce::Colour note         { 0xff6fa8dc };
    inline const juce::Colour noteSelected { 0xffd8b45a };
    inline const juce::Colour noteOutOfScale { 0xffc9835a };  // muted orange: marked, not alarming
    inline const juce::Colour gridOutOfScale { 0xff111317 };
    inline const juce::Colour meterGreen   { 0xff5fbf7f };
    inline const juce::Colour meterRed     { 0xffe0564f };
}

class MoonLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    MoonLookAndFeel();

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPosProportional,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
};

} // namespace moontune
