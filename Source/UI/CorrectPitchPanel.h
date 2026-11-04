#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "Music/ScaleCorrector.h"
#include "Music/KeyScale.h"
#include <array>

namespace moontune
{

// "Correct Pitch" pop-up: automatic correction toward the selected key/scale.
// Writes the correction layer of the notes (undoable, switchable with the CORR stage toggle).
// Live: every change of a control is previewed at once on the notes (and in the sound);
// Apply keeps it as one undo step, Cancel restores the notes as they were. Closing the pop-up
// keeps what is shown.
class CorrectPitchPanel final : public juce::Component
{
public:
    CorrectPitchPanel (const juce::String& scaleName, bool hasSelection);
    ~CorrectPitchPanel() override;

    std::function<void (const ScaleCorrector::Settings&, bool selectedOnly)> onPreview;
    std::function<void()> onApply, onCancel;     // exactly one of these (or onRemove) ends the session
    std::function<void (bool selectedOnly)> onRemove;
    void startPreview();                          // call once after the callbacks are set

    // The 12 notes of the scale: click one to leave it out of / add it to the scale.
    std::function<KeyScale()> scaleProvider;
    std::function<void (int pitchClass)> onTogglePitchClass;
    std::function<void()> onResetScale;
    void refreshScale();

    void paint (juce::Graphics&) override;
    void resized() override;

    ScaleCorrector::Settings getSettings() const;

private:
    struct Row { juce::Label label; juce::Slider slider; };
    void addRow (Row&, const juce::String& text, double lo, double hi, double value, const juce::String& suffix);

    juce::Label header, hint;
    Row strength, drift, speed, tolerance, minLength;
    juce::ToggleButton selectedOnly { "Selected notes only" };
    juce::TextButton applyButton { "Apply" }, cancelButton { "Cancel" }, removeButton { "Remove correction" };
    std::array<juce::TextButton, 12> noteButtons;
    juce::TextButton resetScaleButton { "Reset" };
    juce::Label scaleLabel;
    bool finished = false;
    void finish (bool apply);
};

} // namespace moontune
