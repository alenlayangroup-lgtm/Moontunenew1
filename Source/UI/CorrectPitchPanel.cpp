#include "CorrectPitchPanel.h"
#include "MoonLookAndFeel.h"

namespace moontune
{

CorrectPitchPanel::CorrectPitchPanel (const juce::String& scaleName, bool hasSelection)
{
    header.setText ("Correct Pitch  -  " + scaleName, juce::dontSendNotification);
    header.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    header.setColour (juce::Label::textColourId, Colours::accent);
    addAndMakeVisible (header);

    hint.setText ("Pulls notes toward the scale; you see and hear it while moving the controls. "
                  "Vibrato and transitions are kept. Compare with the CORR switch.", juce::dontSendNotification);
    hint.setFont (juce::FontOptions (11.5f));
    hint.setColour (juce::Label::textColourId, Colours::textDim);
    hint.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (hint);

    addRow (strength, "Pitch center", 0, 100, 100, " %");
    addRow (drift, "Drift (not vibrato)", 0, 100, 30, " %");
    addRow (speed, "Speed (attack)", 0, 400, 40, " ms");
    addRow (tolerance, "Leave if within", 0, 50, 0, " c");
    addRow (minLength, "Skip notes shorter than", 0, 300, 0, " ms");
    strength.slider.setTooltip ("100 %: note centers land on the scale notes. Lower keeps part of the singer's intonation.");
    speed.slider.setTooltip ("0 ms corrects the whole note. Larger values keep the natural approach at the note start.");
    tolerance.slider.setTooltip ("Notes already this close to the scale are left untouched.");
    minLength.slider.setTooltip ("Short passing notes and ornaments are left untouched.");

    selectedOnly.setToggleState (hasSelection, juce::dontSendNotification);
    selectedOnly.setEnabled (hasSelection);
    addAndMakeVisible (selectedOnly);

    auto preview = [this] { if (! finished && onPreview) onPreview (getSettings(), selectedOnly.getToggleState()); };
    for (Row* row : { &strength, &drift, &speed, &tolerance, &minLength })
        row->slider.onValueChange = preview;
    selectedOnly.onClick = preview;

    applyButton.onClick = [this] { finish (true); if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); };
    cancelButton.onClick = [this] { finish (false); if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); };
    removeButton.onClick = [this]
    {
        if (finished) return;
        finished = true;
        if (onRemove) onRemove (selectedOnly.getToggleState());
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
    };
    addAndMakeVisible (applyButton);
    addAndMakeVisible (cancelButton);
    addAndMakeVisible (removeButton);

    scaleLabel.setText ("Notes of the scale (click to leave one out / add it):", juce::dontSendNotification);
    scaleLabel.setFont (juce::FontOptions (11.5f));
    scaleLabel.setColour (juce::Label::textColourId, Colours::textDim);
    addAndMakeVisible (scaleLabel);
    for (int pc = 0; pc < 12; ++pc)
    {
        auto& b = noteButtons[(size_t) pc];
        b.setButtonText (KeyScale::pitchClassName (pc));
        b.setColour (juce::TextButton::buttonColourId, Colours::panelLight);
        b.setColour (juce::TextButton::buttonOnColourId, Colours::accent);
        b.setColour (juce::TextButton::textColourOffId, Colours::textDim);
        b.setColour (juce::TextButton::textColourOnId, Colours::background);
        b.onClick = [this, pc]
        {
            if (onTogglePitchClass) onTogglePitchClass (pc);
            refreshScale();
            if (! finished && onPreview) onPreview (getSettings(), selectedOnly.getToggleState());
        };
        addAndMakeVisible (b);
    }
    resetScaleButton.setTooltip ("Back to the standard notes of the chosen scale");
    resetScaleButton.onClick = [this]
    {
        if (onResetScale) onResetScale();
        refreshScale();
        if (! finished && onPreview) onPreview (getSettings(), selectedOnly.getToggleState());
    };
    addAndMakeVisible (resetScaleButton);

    setSize (380, 360);
}

CorrectPitchPanel::~CorrectPitchPanel()
{
    finish (true);   // closed by clicking outside: keep what is shown
}

void CorrectPitchPanel::refreshScale()
{
    if (! scaleProvider) return;
    const auto sc = scaleProvider();
    for (int pc = 0; pc < 12; ++pc)
    {
        auto& b = noteButtons[(size_t) pc];
        b.setToggleState (sc.containsPitchClass (pc), juce::dontSendNotification);
        b.setEnabled (! (sc.getType() != ScaleType::Chromatic && sc.isRoot (pc)));   // the root stays
    }
    header.setText ("Correct Pitch  -  " + (sc.isChromatic() ? juce::String ("Chromatic (nearest semitone)") : sc.getName()),
                    juce::dontSendNotification);
}

void CorrectPitchPanel::startPreview()
{
    refreshScale();
    if (onPreview) onPreview (getSettings(), selectedOnly.getToggleState());
}

void CorrectPitchPanel::finish (bool apply)
{
    if (finished) return;
    finished = true;
    if (apply) { if (onApply) onApply(); }
    else if (onCancel) onCancel();
}

void CorrectPitchPanel::addRow (Row& r, const juce::String& text, double lo, double hi, double value, const juce::String& suffix)
{
    r.label.setText (text, juce::dontSendNotification);
    r.label.setFont (juce::FontOptions (12.0f));
    r.label.setColour (juce::Label::textColourId, Colours::text);
    addAndMakeVisible (r.label);

    r.slider.setSliderStyle (juce::Slider::LinearHorizontal);
    r.slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
    r.slider.setRange (lo, hi, 1.0);
    r.slider.setValue (value, juce::dontSendNotification);
    r.slider.setTextValueSuffix (suffix);
    addAndMakeVisible (r.slider);
}

ScaleCorrector::Settings CorrectPitchPanel::getSettings() const
{
    ScaleCorrector::Settings s;
    s.strength = (float) (strength.slider.getValue() / 100.0);
    s.driftStrength = (float) (drift.slider.getValue() / 100.0);
    s.speedMs = (float) speed.slider.getValue();
    s.tolerance = (float) tolerance.slider.getValue();
    s.minNoteSeconds = minLength.slider.getValue() / 1000.0;
    return s;
}

void CorrectPitchPanel::paint (juce::Graphics& g)
{
    g.fillAll (Colours::panel);
}

void CorrectPitchPanel::resized()
{
    auto r = getLocalBounds().reduced (12);
    header.setBounds (r.removeFromTop (22));
    hint.setBounds (r.removeFromTop (34));
    for (Row* row : { &strength, &drift, &speed, &tolerance, &minLength })
    {
        auto line = r.removeFromTop (30);
        row->label.setBounds (line.removeFromLeft (140));
        row->slider.setBounds (line);
    }
    r.removeFromTop (4);
    scaleLabel.setBounds (r.removeFromTop (16));
    {
        auto row = r.removeFromTop (24);
        resetScaleButton.setBounds (row.removeFromRight (50));
        row.removeFromRight (4);
        const int w = row.getWidth() / 12;
        for (auto& b : noteButtons)
            b.setBounds (row.removeFromLeft (w).reduced (1, 0));
    }
    r.removeFromTop (4);
    selectedOnly.setBounds (r.removeFromTop (24));
    r.removeFromTop (8);
    auto buttons = r.removeFromTop (28);
    applyButton.setBounds (buttons.removeFromLeft (80));
    buttons.removeFromLeft (8);
    cancelButton.setBounds (buttons.removeFromLeft (80));
    buttons.removeFromLeft (8);
    removeButton.setBounds (buttons);
}

} // namespace moontune
