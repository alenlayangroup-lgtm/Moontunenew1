#include "ScaleToolbar.h"
#include "MoonLookAndFeel.h"
#include "PluginProcessor.h"

namespace moontune
{

namespace
{
    void setupLabel (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, Colours::textDim);
        l.setJustificationType (juce::Justification::centredRight);
        l.setFont (juce::FontOptions (12.0f));
    }

    void setupPercentSlider (juce::Slider& s, double defaultValue)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 20);
        s.setTextValueSuffix (" %");
        s.setDoubleClickReturnValue (true, defaultValue);
    }
}

ScaleToolbar::ScaleToolbar (MoonTuneAudioProcessor& p) : processor (p)
{
    auto& state = p.getParameterState();

    setupLabel (keyLabel, "Key");
    setupLabel (scaleLabel, "Scale");
    setupLabel (strengthLabel, "Strength");
    setupLabel (highlightLabel, "Highlight");

    // Item ids must be index + 1 for ComboBoxAttachment.
    keyBox.addItemList (KeyScale::allPitchClassNames(), 1);
    scaleBox.addItemList (KeyScale::allScaleTypeNames(), 1);
    keyBox.setTooltip ("Key root");
    scaleBox.setTooltip ("Scale / mode. Chromatic = no restriction.");

    snapButton.setTooltip ("When dragging a note's pitch, pull it toward the nearest note of the scale. "
                           "Off = no correction at all.");
    setupPercentSlider (strengthSlider, 100.0);
    strengthSlider.setTooltip ("Snap strength: 100 % lands exactly on the scale note, lower values only pull "
                               "toward it (keeps intentional bends).");
    setupPercentSlider (highlightSlider, 50.0);
    highlightSlider.setTooltip ("How strongly the scale is shown on the grid. 0 % = plain piano roll.");

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &keyLabel, &keyBox, &scaleLabel, &scaleBox, &snapButton,
             &strengthLabel, &strengthSlider, &highlightLabel, &highlightSlider })
        addAndMakeVisible (c);

    keyAttachment       = std::make_unique<APVTS::ComboBoxAttachment> (state, ParamIDs::keyRoot.getParamID(), keyBox);
    scaleAttachment     = std::make_unique<APVTS::ComboBoxAttachment> (state, ParamIDs::scaleType.getParamID(), scaleBox);
    snapAttachment      = std::make_unique<APVTS::ButtonAttachment> (state, ParamIDs::snapToScale.getParamID(), snapButton);
    strengthAttachment  = std::make_unique<APVTS::SliderAttachment> (state, ParamIDs::snapStrength.getParamID(), strengthSlider);
    highlightAttachment = std::make_unique<APVTS::SliderAttachment> (state, ParamIDs::scaleHighlight.getParamID(), highlightSlider);

    auto watch = [&] (const juce::ParameterID& id)
    {
        auto w = std::make_unique<juce::ParameterAttachment> (*state.getParameter (id.getParamID()),
                                                              [this] (float) { parameterChanged(); });
        return w;
    };
    keyWatch       = watch (ParamIDs::keyRoot);
    scaleWatch     = watch (ParamIDs::scaleType);
    highlightWatch = watch (ParamIDs::scaleHighlight);
    snapWatch      = watch (ParamIDs::snapToScale);
    editsWatch     = watch (ParamIDs::scaleEdits);
    scaleBox.setTooltip ("Scale / mode. Chromatic = no restriction. Right-click a note on the pitch ruler "
                         "to add it to / remove it from the scale.");

    updateEnablement();
}

ScaleToolbar::~ScaleToolbar() = default;

// Read the parameter objects themselves: inside a parameter callback the APVTS raw
// atomics may not have been updated yet (listener order is not guaranteed).
float ScaleToolbar::plainValue (const juce::ParameterID& id) const
{
    auto* param = processor.getParameterState().getParameter (id.getParamID());
    return param->convertFrom0to1 (param->getValue());
}

KeyScale ScaleToolbar::currentScale() const
{
    return KeyScale (juce::roundToInt (plainValue (ParamIDs::keyRoot)), (ScaleType) juce::roundToInt (plainValue (ParamIDs::scaleType)))
               .withPitchClassesToggled ((juce::uint16) juce::roundToInt (plainValue (ParamIDs::scaleEdits)));
}

void ScaleToolbar::parameterChanged()
{
    updateEnablement();

    if (onScaleDisplayChanged)
        onScaleDisplayChanged (currentScale(), plainValue (ParamIDs::scaleHighlight) / 100.0f);
}

void ScaleToolbar::updateEnablement()
{
    const auto sc = currentScale();
    const bool chromatic = sc.isChromatic() || sc.getType() == ScaleType::Chromatic;
    keyBox.setEnabled (! chromatic);          // root is meaningless without a scale
    keyLabel.setEnabled (! chromatic);
    const bool snapOn = plainValue (ParamIDs::snapToScale) >= 0.5f;
    strengthSlider.setEnabled (snapOn);
    strengthLabel.setEnabled (snapOn);
}

void ScaleToolbar::paint (juce::Graphics& g)
{
    g.fillAll (Colours::panel.darker (0.15f));
    g.setColour (Colours::gridLineC);
    g.fillRect (getLocalBounds().removeFromBottom (1));
}

void ScaleToolbar::resized()
{
    auto r = getLocalBounds().reduced (16, 7);

    keyLabel.setBounds (r.removeFromLeft (30));
    r.removeFromLeft (6);
    keyBox.setBounds (r.removeFromLeft (64));
    r.removeFromLeft (12);
    scaleLabel.setBounds (r.removeFromLeft (40));
    r.removeFromLeft (6);
    scaleBox.setBounds (r.removeFromLeft (124));
    r.removeFromLeft (16);
    snapButton.setBounds (r.removeFromLeft (112));
    r.removeFromLeft (6);
    strengthLabel.setBounds (r.removeFromLeft (56));
    strengthSlider.setBounds (r.removeFromLeft (110));
    r.removeFromLeft (8);

    highlightSlider.setBounds (r.removeFromRight (juce::jmin (130, juce::jmax (90, r.getWidth() - 64))));
    highlightLabel.setBounds (r.removeFromRight (64));
}

} // namespace moontune
