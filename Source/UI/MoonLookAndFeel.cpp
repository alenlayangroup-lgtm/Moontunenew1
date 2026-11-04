#include "MoonLookAndFeel.h"

namespace moontune
{

MoonLookAndFeel::MoonLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, Colours::background);
    setColour (juce::Label::textColourId, Colours::text);
    setColour (juce::Slider::textBoxTextColourId, Colours::text);
    setColour (juce::Slider::textBoxBackgroundColourId, Colours::panelLight);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::thumbColourId, Colours::accent);
    setColour (juce::Slider::trackColourId, Colours::accent);
    setColour (juce::Slider::backgroundColourId, Colours::panelLight);
    setColour (juce::ToggleButton::textColourId, Colours::text);
    setColour (juce::TooltipWindow::backgroundColourId, Colours::panelLight);
    setColour (juce::TooltipWindow::textColourId, Colours::text);
    setColour (juce::ComboBox::backgroundColourId, Colours::panelLight);
    setColour (juce::ComboBox::textColourId, Colours::text);
    setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ComboBox::arrowColourId, Colours::textDim);
    setColour (juce::PopupMenu::backgroundColourId, Colours::panel);
    setColour (juce::PopupMenu::textColourId, Colours::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Colours::accent.withAlpha (0.35f));
    setColour (juce::PopupMenu::highlightedTextColourId, Colours::text);
    setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TextEditor::focusedOutlineColourId, Colours::accent.withAlpha (0.6f));
}

void MoonLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                        float sliderPos, float, float,
                                        juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, 0, 0, style, slider);
        return;
    }

    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto track  = bounds.withSizeKeepingCentre (bounds.getWidth(), 4.0f);

    g.setColour (Colours::panelLight);
    g.fillRoundedRectangle (track, 2.0f);

    // Fill from the 0 dB point (or the range start) to the thumb.
    const double zeroValue = juce::jlimit (slider.getMinimum(), slider.getMaximum(), 0.0);
    const float zeroX = (float) slider.getPositionOfValue (zeroValue);
    const float left = juce::jmin (zeroX, sliderPos), right = juce::jmax (zeroX, sliderPos);

    g.setColour (slider.isEnabled() ? Colours::accent : Colours::textDim);
    g.fillRect (juce::Rectangle<float> (left, track.getY(), right - left, track.getHeight()));

    g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ sliderPos, bounds.getCentreY() }));
}

void MoonLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                        bool highlighted, bool)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = button.getToggleState();

    g.setColour (on ? Colours::accent.withAlpha (0.9f) : Colours::panelLight.brighter (highlighted ? 0.15f : 0.0f));
    g.fillRoundedRectangle (bounds, 4.0f);

    g.setColour (on ? Colours::background : Colours::text);
    g.setFont (juce::FontOptions (juce::jmin (13.0f, bounds.getHeight() * 0.55f), juce::Font::bold));
    g.drawFittedText (button.getButtonText(), bounds.reduced (3.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.6f);
}

void MoonLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                                        float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    if (radius < 4.0f)
        return;
    const auto centre = bounds.getCentre();
    const float track = juce::jmax (2.0f, radius * 0.14f);
    const float arcRadius = radius - track * 0.5f;

    // Track
    juce::Path bg;
    bg.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (Colours::panelLight.brighter (0.05f));
    g.strokePath (bg, juce::PathStrokeType (track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Value arc from the default (centre for bipolar ranges, start otherwise)
    const double lo = slider.getMinimum(), hi = slider.getMaximum();
    const bool bipolar = lo < 0.0 && hi > 0.0;
    const float zeroPos = bipolar ? (float) ((0.0 - lo) / (hi - lo)) : 0.0f;
    const float a0 = startAngle + zeroPos * (endAngle - startAngle);
    const float a1 = startAngle + pos * (endAngle - startAngle);
    if (std::abs (a1 - a0) > 0.001f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, juce::jmin (a0, a1), juce::jmax (a0, a1), true);
        g.setColour (slider.isEnabled() ? Colours::accent : Colours::textDim);
        g.strokePath (value, juce::PathStrokeType (track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Knob body + pointer
    const float bodyR = arcRadius - track * 1.2f;
    g.setColour (Colours::panelLight);
    g.fillEllipse (juce::Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (centre));
    const juce::Point<float> tip (centre.x + bodyR * 0.85f * std::sin (a1), centre.y - bodyR * 0.85f * std::cos (a1));
    const juce::Point<float> base (centre.x + bodyR * 0.25f * std::sin (a1), centre.y - bodyR * 0.25f * std::cos (a1));
    g.setColour (slider.isEnabled() ? Colours::text : Colours::textDim);
    g.drawLine ({ base, tip }, juce::jmax (1.5f, track * 0.6f));
}

} // namespace moontune
