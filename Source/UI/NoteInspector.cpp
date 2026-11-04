#include "NoteInspector.h"
#include <algorithm>
#include "MoonLookAndFeel.h"
#include "DSP/NoteEqFilter.h"

#include <cmath>

namespace moontune
{

struct NoteInspector::Control
{
    juce::String name;
    juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label label;
    std::function<double (const Note&)> get;
    std::function<void (Note&, double)> set;
    bool relative = false;
    int eqBand = -1;              // >= 0: belongs to an EQ band (auto-enables it)
    double gestureStartValue = 0.0;
    std::vector<Note> gestureBase;
    bool dragging = false;
};

// Combined response of the first selected note's EQ.
class NoteInspector::EqCurve final : public juce::Component
{
public:
    void setEq (const NoteEq* e) { eq = e != nullptr ? *e : NoteEq{}; has = e != nullptr; repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::background);
        g.fillRoundedRectangle (r, 3.0f);
        if (! has) return;

        constexpr double fs = 48000.0;
        const NoteEqChain chain (eq, fs);
        auto xOf = [&] (double f) { return r.getX() + r.getWidth() * (float) (std::log10 (f / 20.0) / 3.0); };
        auto yOf = [&] (double db) { return r.getCentreY() - (float) (db / 20.0) * r.getHeight() * 0.5f; };

        g.setColour (Colours::gridLine);
        for (double f : { 100.0, 1000.0, 10000.0 })
            g.fillRect (juce::Rectangle<float> (xOf (f), r.getY(), 1.0f, r.getHeight()));
        g.fillRect (juce::Rectangle<float> (r.getX(), r.getCentreY(), r.getWidth(), 1.0f));

        juce::Path p;
        for (int i = 0; i <= 120; ++i)
        {
            const double f = 20.0 * std::pow (1000.0, i / 120.0);
            const auto pt = juce::Point<float> (xOf (f), yOf (juce::jlimit (-20.0, 20.0, chain.magnitudeDb (f, fs))));
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        g.setColour (eq.isActive() ? Colours::accent : Colours::textDim);
        g.strokePath (p, juce::PathStrokeType (1.5f));
    }

private:
    NoteEq eq;
    bool has = false;
};

namespace
{
    juce::String signedValue (double v, int decimals, const juce::String& unit)
    {
        return (v > 0.0 ? "+" : "") + juce::String (v, decimals) + unit;
    }
}

NoteInspector::NoteInspector (NoteModel& m, EditHistory& h) : model (m), history (h)
{
    title.setText ("Selected notes", juce::dontSendNotification);
    title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, Colours::text);
    addAndMakeVisible (title);

    infoLabel.setColour (juce::Label::textColourId, Colours::textDim);
    infoLabel.setFont (juce::FontOptions (12.0f));
    infoLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (infoLabel);

    // --- Note parameters ---------------------------------------------------------------
    addControl ("pitch", "Pitch", -1200.0, 1200.0, 1.0, 0.0,
                [] (const Note& n) { return (double) n.pitchShiftCents; },
                [] (Note& n, double v) { n.pitchShiftCents = (float) v; n.correctionCents = 0.0f; },
                true, [] (double v) { return signedValue (v, 0, " c"); });
    addControl ("modulation", "Vibrato", 0.0, 200.0, 1.0, 100.0,
                [] (const Note& n) { return n.pitchModulation * 100.0; },
                [] (Note& n, double v) { n.pitchModulation = (float) (v / 100.0); },
                false, [] (double v) { return juce::String (v, 0) + " %"; });
    addControl ("drift", "Drift", 0.0, 200.0, 1.0, 100.0,
                [] (const Note& n) { return n.pitchDrift * 100.0; },
                [] (Note& n, double v) { n.pitchDrift = (float) (v / 100.0); },
                false, [] (double v) { return juce::String (v, 0) + " %"; });
    addControl ("formant", "Formant", -1200.0, 1200.0, 1.0, 0.0,
                [] (const Note& n) { return (double) n.formantCents; },
                [] (Note& n, double v) { n.formantCents = (float) v; },
                false, [] (double v) { return signedValue (v / 100.0, 1, " st"); });
    addControl ("gain", "Gain", (double) Note::silentDb, 12.0, 0.1, 0.0,
                [] (const Note& n) { return (double) n.gainDb; },
                [] (Note& n, double v) { n.gainDb = (float) v; },
                false, [] (double v) { return v <= Note::silentDb + 0.05 ? juce::String ("-inf") : signedValue (v, 1, " dB"); });
    controls.back()->slider.setSkewFactorFromMidPoint (-12.0);
    controls.back()->slider.setTooltip ("Level of the selected notes, breaths or consonants. All the way down = silent.");
    addControl ("tilt", "Tilt", -600.0, 600.0, 1.0, 0.0,
                [] (const Note& n) { return (double) n.pitchTilt; },
                [] (Note& n, double v) { n.pitchTilt = (float) v; },
                false, [] (double v) { return signedValue (v, 0, " c"); });
    controls.back()->slider.setTooltip ("Pitch slope across the note: the start goes down and the end up by half of it (or the reverse).");
    addControl ("transition", "Transition", 0.0, 500.0, 1.0, 45.0,
                [] (const Note& n) { return n.transitionMs >= 0.0f ? (double) n.transitionMs : 45.0; },
                [] (Note& n, double v) { n.transitionMs = (float) v; },
                false, [] (double v) { return juce::String (v, 0) + " ms"; });
    controls.back()->slider.setTooltip ("Glide from the previous note into this one: short = a clean step, long = a slow slide.");

    // --- EQ ------------------------------------------------------------------------------
    eqTitle.setText ("Note EQ", juce::dontSendNotification);
    eqTitle.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    eqTitle.setColour (juce::Label::textColourId, Colours::text);
    addAndMakeVisible (eqTitle);

    eqEnable.setTooltip ("Switch the EQ of the selected notes on/off. Off by default; touching a band switches it on.");
    eqEnable.onClick = [this]
    {
        if (updating) return;
        auto all = model.getSnapshot();
        for (auto& n : all)
            if (selection.count (n.id) != 0)
                n.eq.enabled = eqEnable.getToggleState();
        history.perform ("Note EQ on/off", std::move (all));
        refresh();
    };
    addAndMakeVisible (eqEnable);

    curve = std::make_unique<EqCurve>();
    addAndMakeVisible (*curve);

    const juce::StringArray typeNames { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut" };
    for (int b = 0; b < 4; ++b)
    {
        auto& t = bandEnable[(size_t) b];
        t.setButtonText (juce::String (b + 1));
        t.onClick = [this, b]
        {
            if (updating) return;
            auto all = model.getSnapshot();
            for (auto& n : all)
                if (selection.count (n.id) != 0)
                {
                    n.eq.bands[(size_t) b].enabled = bandEnable[(size_t) b].getToggleState();
                    if (n.eq.bands[(size_t) b].enabled) n.eq.enabled = true;
                }
            history.perform ("EQ band on/off", std::move (all));
            refresh();
        };
        addAndMakeVisible (t);

        auto& c = bandType[(size_t) b];
        c.addItemList (typeNames, 1);
        c.onChange = [this, b]
        {
            if (updating) return;
            auto all = model.getSnapshot();
            for (auto& n : all)
                if (selection.count (n.id) != 0)
                {
                    n.eq.bands[(size_t) b].type = bandType[(size_t) b].getSelectedItemIndex();
                    n.eq.bands[(size_t) b].enabled = true;
                    n.eq.enabled = true;
                }
            history.perform ("EQ band type", std::move (all));
            refresh();
        };
        addAndMakeVisible (c);

        const auto band = (size_t) b;
        addControl ("eq" + juce::String (b) + "freq", "Freq", 20.0, 20000.0, 1.0, NoteEq::defaultBand (b).frequency,
                    [band] (const Note& n) { return (double) n.eq.bands[band].frequency; },
                    [band] (Note& n, double v) { n.eq.bands[band].frequency = (float) v; },
                    false, [] (double v) { return v >= 1000.0 ? juce::String (v / 1000.0, 1) + " k" : juce::String (v, 0); }, b);
        addControl ("eq" + juce::String (b) + "gain", "Gain", -18.0, 18.0, 0.1, 0.0,
                    [band] (const Note& n) { return (double) n.eq.bands[band].gainDb; },
                    [band] (Note& n, double v) { n.eq.bands[band].gainDb = (float) v; },
                    false, [] (double v) { return signedValue (v, 1, ""); }, b);
        addControl ("eq" + juce::String (b) + "q", "Q", 0.2, 10.0, 0.01, NoteEq::defaultBand (b).q,
                    [band] (const Note& n) { return (double) n.eq.bands[band].q; },
                    [band] (Note& n, double v) { n.eq.bands[band].q = (float) v; },
                    false, [] (double v) { return juce::String (v, 2); }, b);
    }

    for (auto& c : controls)
        if (c->name.endsWith ("freq"))
            c->slider.setSkewFactorFromMidPoint (1000.0);

    refresh();
}

NoteInspector::~NoteInspector() = default;

void NoteInspector::addControl (const juce::String& name, const juce::String& label, double lo, double hi, double step,
                                double defaultValue, std::function<double (const Note&)> get,
                                std::function<void (Note&, double)> set, bool relative,
                                std::function<juce::String (double)> format, int eqBand)
{
    auto c = std::make_unique<Control>();
    c->name = name;
    c->get = std::move (get);
    c->set = std::move (set);
    c->relative = relative;
    c->eqBand = eqBand;

    auto& s = c->slider;
    s.setRange (lo, hi, step);
    s.setValue (defaultValue, juce::dontSendNotification);
    s.setDoubleClickReturnValue (true, defaultValue);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 62, 16);
    s.textFromValueFunction = format;
    s.setColour (juce::Slider::rotarySliderFillColourId, Colours::accent);
    s.setColour (juce::Slider::rotarySliderOutlineColourId, Colours::panelLight);
    s.setColour (juce::Slider::textBoxTextColourId, Colours::text);
    s.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    auto* raw = c.get();
    s.onDragStart = [this, raw]
    {
        raw->dragging = true;
        raw->gestureBase = model.getSnapshot();
        raw->gestureStartValue = raw->slider.getValue();
        history.beginGesture();
    };
    s.onValueChange = [this, raw] { valueChanged (*raw); };
    s.onDragEnd = [this, raw]
    {
        raw->dragging = false;
        history.endGesture ("Change " + raw->label.getText().toLowerCase());
        refresh();
    };
    addAndMakeVisible (s);

    c->label.setText (label, juce::dontSendNotification);
    c->label.setJustificationType (juce::Justification::centred);
    c->label.setFont (juce::FontOptions (11.0f));
    c->label.setColour (juce::Label::textColourId, Colours::textDim);
    addAndMakeVisible (c->label);

    controls.push_back (std::move (c));
}

void NoteInspector::valueChanged (Control& c)
{
    if (updating || selection.empty())
        return;
    apply (c, c.slider.getValue(), c.dragging);
}

void NoteInspector::apply (Control& c, double value, bool asGesture)
{
    // Base: the state at the start of the drag, or the current state for single changes.
    std::vector<Note> base = asGesture && ! c.gestureBase.empty() ? c.gestureBase : model.getSnapshot();
    double startValue = c.gestureStartValue;
    if (! asGesture)
    {
        const auto* first = firstSelected();
        startValue = first != nullptr ? c.get (*first) : value;
    }

    for (auto& n : base)
    {
        if (selection.count (n.id) == 0)
            continue;
        if (! n.isNote() && c.name != "gain")
            continue;   // breaths / consonants only have a level
        if (c.relative)
            c.set (n, c.get (n) + (value - startValue));
        else
            c.set (n, value);
        if (c.eqBand >= 0)
        {
            n.eq.bands[(size_t) c.eqBand].enabled = true;   // touching a band switches it on
            n.eq.enabled = true;
        }
    }

    if (asGesture)
        history.updateGesture (std::move (base));
    else
        history.perform ("Change " + c.label.getText().toLowerCase(), std::move (base));

    // Reflect without feeding back into the sliders being dragged.
    cache = model.getSnapshot();
    if (c.eqBand >= 0 || ! asGesture)
    {
        const juce::ScopedValueSetter<bool> svs (updating, true);
        if (const auto* first = firstSelected())
        {
            eqEnable.setToggleState (first->eq.enabled, juce::dontSendNotification);
            for (size_t b = 0; b < 4; ++b)
                bandEnable[b].setToggleState (first->eq.bands[b].enabled, juce::dontSendNotification);
            curve->setEq (&first->eq);
        }
    }
}

void NoteInspector::setValueForTesting (const juce::String& controlName, double value)
{
    for (auto& c : controls)
        if (c->name == controlName)
        {
            c->slider.setValue (value, juce::sendNotificationSync);
            return;
        }
    jassertfalse;
}

std::vector<Note> NoteInspector::selectedNotes() const
{
    std::vector<Note> out;
    for (const auto& n : cache)
        if (selection.count (n.id) != 0)
            out.push_back (n);
    return out;
}

const Note* NoteInspector::firstSelected() const
{
    for (const auto& n : cache)
        if (selection.count (n.id) != 0)
            return &n;
    return nullptr;
}

void NoteInspector::setSelection (const std::set<juce::int64>& ids)
{
    selection = ids;
    refresh();
}

void NoteInspector::refresh()
{
    cache = model.getSnapshot();
    const juce::ScopedValueSetter<bool> svs (updating, true);
    const auto sel = selectedNotes();
    const auto* first = firstSelected();

    if (first == nullptr)
    {
        title.setText ("No note selected", juce::dontSendNotification);
        infoLabel.setText ("Click a note to edit it, drag on empty space to select several.\n"
                           "Drag up/down: pitch (Ctrl: free).  Drag left/right: time.  Drag an edge: length.\n"
                           "Double-click: onto the nearest note.  Ctrl+Z / Ctrl+Y: undo / redo.",
                           juce::dontSendNotification);
        curve->setEq (nullptr);
    }
    else
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const float p = first->editedPitch (true);
        const int nearest = juce::roundToInt (p);
        const int cents = juce::roundToInt ((p - (float) nearest) * 100.0f);
        const bool allNoise = std::all_of (sel.begin(), sel.end(), [] (const Note& x) { return ! x.isNote(); });
        juce::String t = allNoise ? (sel.size() > 1 ? juce::String ((int) sel.size()) + " breaths / consonants"
                                                    : juce::String (first->kind == Note::breath ? "Breath" : first->kind == Note::sibilant ? "Consonant s / ts" : "Consonant k / t / p"))
                       : sel.size() > 1 ? juce::String ((int) sel.size()) + " notes selected"
                                        : juce::String (names[((nearest % 12) + 12) % 12]) + juce::String (nearest / 12 - 1)
                                              + (cents != 0 ? (cents > 0 ? " +" : " ") + juce::String (cents) + " c" : juce::String());
        title.setText (t, juce::dontSendNotification);

        juce::String info;
        info << "Length " << juce::String ((first->outEnd() - first->outStart()) * 1000.0, 0) << " ms";
        if (first->hasTimeEdit())
            info << "  (moved " << juce::String (first->timeShiftSeconds * 1000.0, 0) << " ms)";
        if (first->hasCorrection())
            info << "\nScale correction " << juce::roundToInt (first->correctionCents) << " c";
        infoLabel.setText (info, juce::dontSendNotification);

        for (auto& c : controls)
            c->slider.setValue (c->get (*first), juce::dontSendNotification);
        eqEnable.setToggleState (first->eq.enabled, juce::dontSendNotification);
        for (size_t b = 0; b < 4; ++b)
        {
            bandEnable[b].setToggleState (first->eq.bands[b].enabled, juce::dontSendNotification);
            bandType[b].setSelectedItemIndex (first->eq.bands[b].type, juce::dontSendNotification);
        }
        curve->setEq (&first->eq);
    }
    updateEnablement();
    repaint();
}

void NoteInspector::updateEnablement()
{
    const bool any = firstSelected() != nullptr;
    for (auto& c : controls)
    {
        c->slider.setEnabled (any);
        c->slider.setAlpha (any ? 1.0f : 0.35f);
        c->label.setAlpha (any ? 1.0f : 0.35f);
    }
    eqEnable.setEnabled (any);
    for (size_t b = 0; b < 4; ++b)
    {
        bandEnable[b].setEnabled (any);
        bandType[b].setEnabled (any);
    }
}

void NoteInspector::paint (juce::Graphics& g)
{
    g.fillAll (Colours::panel);
    g.setColour (Colours::gridLineC);
    g.fillRect (getLocalBounds().removeFromTop (1));
    // Divider between note parameters and EQ.
    const int split = juce::jmin (getWidth() / 2, 600);
    g.fillRect (split, 8, 1, getHeight() - 16);
}

void NoteInspector::resized()
{
    auto r = getLocalBounds().reduced (12, 8);
    const int split = juce::jmin (getWidth() / 2, 600) - 12;

    auto left = r.removeFromLeft (split);
    auto info = left.removeFromLeft (150);
    title.setBounds (info.removeFromTop (20));
    infoLabel.setBounds (info);

    // 7 note knobs
    const int knobW = juce::jmin (74, left.getWidth() / 7);
    for (int i = 0; i < 7; ++i)
    {
        auto col = left.removeFromLeft (knobW);
        controls[(size_t) i]->label.setBounds (col.removeFromTop (16));
        controls[(size_t) i]->slider.setBounds (col.reduced (2, 0));
    }

    r.removeFromLeft (14);
    auto eqHeader = r.removeFromLeft (110);
    eqTitle.setBounds (eqHeader.removeFromTop (20));
    eqEnable.setBounds (eqHeader.removeFromTop (24).withWidth (60));
    eqHeader.removeFromTop (6);
    curve->setBounds (eqHeader.removeFromTop (juce::jmin (80, eqHeader.getHeight())).reduced (0, 2));

    // 4 band columns: [on][type] / freq gain q
    const int bandW = r.getWidth() / 4;
    for (int b = 0; b < 4; ++b)
    {
        auto col = r.removeFromLeft (bandW).reduced (4, 0);
        auto top = col.removeFromTop (22);
        bandEnable[(size_t) b].setBounds (top.removeFromLeft (34));
        bandType[(size_t) b].setBounds (top.reduced (0, 1));
        col.removeFromTop (2);
        const int w = col.getWidth() / 3;
        for (int k = 0; k < 3; ++k)
        {
            auto& c = *controls[(size_t) (7 + b * 3 + k)];
            auto cell = col.removeFromLeft (w);
            c.label.setBounds (cell.removeFromTop (14));
            c.slider.setBounds (cell);
        }
    }
}

} // namespace moontune
