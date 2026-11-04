#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include <memory>
#include <set>
#include <vector>

#include "Model/NoteModel.h"
#include "Model/EditHistory.h"

namespace moontune
{

// Bottom panel: parameters of the selected notes and their 4-band EQ.
//
// Every control works on all selected notes. Pitch moves are relative (each note
// keeps its own offset); the other values are set on every selected note.
// Dragging a knob is one undo step. Touching an EQ band switches that band and the
// note's EQ on (the EQ is off by default and only used where needed).
class NoteInspector final : public juce::Component
{
public:
    NoteInspector (NoteModel&, EditHistory&);
    ~NoteInspector() override;

    void setSelection (const std::set<juce::int64>& ids);
    void refresh();   // re-read values from the model (after any edit)

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int preferredHeight = 156;

    // Tests: set a control value as if the user turned it.
    void setValueForTesting (const juce::String& controlName, double value);

private:
    struct Control;
    class EqCurve;

    void addControl (const juce::String& name, const juce::String& label, double lo, double hi, double step,
                     double defaultValue, std::function<double (const Note&)> get,
                     std::function<void (Note&, double)> set, bool relative,
                     std::function<juce::String (double)> format, int eqBand = -1);
    void valueChanged (Control&);
    void apply (Control&, double value, bool asGesture);
    std::vector<Note> selectedNotes() const;
    const Note* firstSelected() const;
    void updateEnablement();

    NoteModel& model;
    EditHistory& history;
    std::set<juce::int64> selection;
    std::vector<Note> cache;
    bool updating = false;

    std::vector<std::unique_ptr<Control>> controls;
    juce::Label title, infoLabel, eqTitle;
    juce::ToggleButton eqEnable { "EQ" };
    std::array<juce::ToggleButton, 4> bandEnable;
    std::array<juce::ComboBox, 4> bandType;
    std::unique_ptr<EqCurve> curve;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteInspector)
};

} // namespace moontune
