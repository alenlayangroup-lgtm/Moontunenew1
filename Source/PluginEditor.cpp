#include "PluginEditor.h"
#include "Music/KeyDetector.h"
#include "Music/ScaleCorrector.h"
#include "UI/CorrectPitchPanel.h"

namespace moontune
{

namespace uiIds
{
    static const juce::Identifier width       { "editorWidth" };
    static const juce::Identifier height      { "editorHeight" };
    static const juce::Identifier viewStart   { "viewStart" };
    static const juce::Identifier viewPps     { "viewPixelsPerSecond" };
    static const juce::Identifier viewTop     { "viewTopPitch" };
    static const juce::Identifier viewPpst    { "viewPixelsPerSemitone" };
}

namespace
{
    void styleButton (juce::TextButton& b, juce::Colour on)
    {
        b.setColour (juce::TextButton::buttonColourId, Colours::panelLight);
        b.setColour (juce::TextButton::buttonOnColourId, on);
        b.setColour (juce::TextButton::textColourOffId, Colours::text);
        b.setColour (juce::TextButton::textColourOnId, Colours::background);
    }
}

MoonTuneAudioProcessorEditor::MoonTuneAudioProcessorEditor (MoonTuneAudioProcessor& p)
    : AudioProcessorEditor (p),
      processor (p),
      meter ([&p] { return p.popOutputPeak(); }),
      scaleToolbar (p),
      grid (p.getNoteModel()),
      inspector (p.getNoteModel(), p.getHistory()),
      bypassAttachment (p.getParameterState(), ParamIDs::bypass.getParamID(), bypassButton),
      outputGainAttachment (p.getParameterState(), ParamIDs::outputGain.getParamID(), outputGainSlider)
{
    setLookAndFeel (&lookAndFeel);

    styleButton (recButton, Colours::meterRed);
    recButton.setClickingTogglesState (true);
    recButton.setTooltip ("Record: while this is on, MoonTune listens to the track whenever the DAW plays "
                          "and draws the notes when playback stops.");
    recButton.onClick = [this] { processor.setCaptureArmed (recButton.getToggleState()); };
    addAndMakeVisible (recButton);

    styleButton (clearButton, Colours::accent);
    clearButton.setTooltip ("Remove the recorded vocal and all notes.");
    clearButton.onClick = [this]
    {
        juce::Component::SafePointer<MoonTuneAudioProcessorEditor> safe (this);
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Clear MoonTune",
                                            "Remove the recorded vocal, all notes and all edits?", "Clear", "Cancel", this,
                                            juce::ModalCallbackFunction::create ([safe] (int ok)
                                            {
                                                if (safe != nullptr && ok != 0)
                                                {
                                                    safe->processor.clearCapturedVocal();
                                                    safe->recButton.setToggleState (false, juce::dontSendNotification);
                                                    safe->grid.setAnalysis (nullptr);
                                                }
                                            }));
    };
    addAndMakeVisible (clearButton);

    for (auto* b : { &undoButton, &redoButton })
    {
        styleButton (*b, Colours::accent);
        addAndMakeVisible (*b);
    }
    undoButton.onClick = [this] { processor.getHistory().undo(); grid.refreshNotes(); inspector.refresh(); };
    redoButton.onClick = [this] { processor.getHistory().redo(); grid.refreshNotes(); inspector.refresh(); };

    styleButton (keyButton, Colours::accent);
    keyButton.setTooltip ("Suggest the key from the recorded notes. You can accept it or keep your own choice.");
    keyButton.onClick = [this] { analyseKey(); };
    addAndMakeVisible (keyButton);

    styleButton (correctButton, Colours::accent);
    correctButton.setTooltip ("Automatic correction toward the selected key and scale (strength, speed, drift).");
    correctButton.onClick = [this] { showCorrectPitch(); };
    addAndMakeVisible (correctButton);

    bypassButton.setTooltip ("A/B: hear the original recording (click-free crossfade)");
    addAndMakeVisible (bypassButton);

    outputGainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    outputGainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 22);
    outputGainSlider.setTextValueSuffix (" dB");
    outputGainSlider.setDoubleClickReturnValue (true, 0.0);
    addAndMakeVisible (outputGainSlider);

    outputGainLabel.setText ("Output", juce::dontSendNotification);
    outputGainLabel.setColour (juce::Label::textColourId, Colours::textDim);
    outputGainLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (outputGainLabel);

    addAndMakeVisible (meter);

    statusLabel.setColour (juce::Label::textColourId, Colours::textDim);
    statusLabel.setFont (juce::FontOptions (12.0f));
    statusLabel.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (statusLabel);

    // Processing stages
    stagesLabel.setText ("Processing", juce::dontSendNotification);
    stagesLabel.setFont (juce::FontOptions (11.0f));
    stagesLabel.setColour (juce::Label::textColourId, Colours::textDim);
    stagesLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (stagesLabel);
    const std::pair<const juce::ParameterID*, const char*> stageDefs[] = {
        { &ParamIDs::procPitch, "PITCH" }, { &ParamIDs::procTime, "TIME" }, { &ParamIDs::procFormant, "TIMBRE" },
        { &ParamIDs::procEq, "EQ" }, { &ParamIDs::procProtect, "PROTECT" }, { &ParamIDs::procCorrection, "CORR" } };
    const char* stageTips[] = {
        "Pitch edits on/off (compare)", "Timing edits on/off", "Keep the voice's timbre (formants) when the pitch changes. Off: the timbre follows the pitch (chipmunk / giant effect)",
        "Note EQ on/off", "Consonant & breath protection", "Scale correction layer (Correct Pitch) on/off" };
    for (size_t i = 0; i < 6; ++i)
    {
        StageButton s;
        s.button = std::make_unique<juce::ToggleButton> (stageDefs[i].second);
        s.button->setTooltip (stageTips[i]);
        s.attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            p.getParameterState(), stageDefs[i].first->getParamID(), *s.button);
        addAndMakeVisible (*s.button);
        stages.push_back (std::move (s));
    }

    // --- Navigation / engine bar ---------------------------------------------------------
    auto setupSmallLabel = [] (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (juce::FontOptions (11.0f));
        l.setColour (juce::Label::textColourId, Colours::textDim);
        l.setJustificationType (juce::Justification::centredRight);
    };
    setupSmallLabel (engineLabel, "Engine");
    setupSmallLabel (gridLabel, "Grid");
    addAndMakeVisible (engineLabel);
    addAndMakeVisible (gridLabel);
    for (auto* b : { &cleanButton, &trueVoiceButton, &hybridButton })
    {
        styleButton (*b, Colours::accent);
        b->setClickingTogglesState (false);
        addAndMakeVisible (*b);
    }
    cleanButton.setTooltip ("Harmony Clean: the voice is rebuilt from its harmonics at the new pitch, breath and "
                            "noise kept apart. The cleanest sound, no doubled voice.");
    trueVoiceButton.setTooltip ("True Voice: the real waveform is moved period by period (phase-locked). "
                                "Keeps the natural character of the voice.");
    hybridButton.setTooltip ("Hybrid: True Voice's natural waveform, cleaned by keeping only its harmonics at the new "
                             "pitch, plus the breath layer. Renders about twice as long.");
    {
        auto* engineParam = p.getParameterState().getParameter (ParamIDs::engine.getParamID());
        engineAttachment = std::make_unique<juce::ParameterAttachment> (*engineParam, [this] (float) { updateEngineButtons(); });
        engineAttachment->sendInitialUpdate();
        cleanButton.onClick = [this] { engineAttachment->setValueAsCompleteGesture (0.0f); };
        trueVoiceButton.onClick = [this] { engineAttachment->setValueAsCompleteGesture (1.0f); };
        hybridButton.onClick = [this] { engineAttachment->setValueAsCompleteGesture (2.0f); };
    }

    gridBox.addItemList ({ "Off", "1/4", "1/8", "1/16", "1/32", "1/64" }, 1);
    gridBox.setTooltip ("Musical grid from the DAW tempo. Moving notes and note edges snap to it "
                        "(hold Ctrl/Cmd for free movement).");
    gridAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        p.getParameterState(), ParamIDs::gridDivision.getParamID(), gridBox);
    addAndMakeVisible (gridBox);
    tripletButton.setTooltip ("Triplet grid");
    tripletAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        p.getParameterState(), ParamIDs::gridTriplet.getParamID(), tripletButton);
    addAndMakeVisible (tripletButton);
    followButton.setTooltip ("Follow the DAW playhead (pauses while you scroll, resumes on the next Play)");
    followAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        p.getParameterState(), ParamIDs::followPlayhead.getParamID(), followButton);
    addAndMakeVisible (followButton);
    auditionButton.setTooltip ("Hear a note while you hold and drag it (when the DAW is stopped)");
    auditionAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        p.getParameterState(), ParamIDs::audition.getParamID(), auditionButton);
    addAndMakeVisible (auditionButton);
    styleButton (showAllButton, Colours::accent);
    showAllButton.setTooltip ("Zoom out to show the whole vocal");
    showAllButton.onClick = [this]
    {
        auto v = grid.getView();
        const auto notesNow = processor.getNoteModel().getSnapshot();
        if (notesNow.empty()) return;
        double end = 0.0;
        for (const auto& n : notesNow) end = std::max (end, n.outEnd());
        const double start = std::max (0.0, notesNow.front().outStart() - 0.5);
        const int w = std::max (100, grid.getWidth() - NoteGridView::pitchRulerWidth);
        v.pixelsPerSecond = juce::jlimit (NoteGridView::minPixelsPerSecond, NoteGridView::maxPixelsPerSecond, w / std::max (1.0, end + 0.5 - start));
        grid.setView (v);
        grid.fitToNotes();
        followSuspended = true;
    };
    addAndMakeVisible (showAllButton);

    // Tools
    for (auto* b : { &editToolButton, &splitToolButton, &gainToolButton })
    {
        styleButton (*b, Colours::accent);
        b->setClickingTogglesState (false);
        addAndMakeVisible (*b);
    }
    editToolButton.setTooltip ("Edit tool (E): drag notes for pitch and time, edges for length, handles for vibrato / drift");
    splitToolButton.setTooltip ("Scissors (S): click a note where it should be cut in two");
    gainToolButton.setTooltip ("Gain tool (G): drag a note up / down to make it louder / quieter (Ctrl: fine)");
    editToolButton.onClick = [this] { setTool (NoteGridView::Tool::edit); };
    splitToolButton.onClick = [this] { setTool (NoteGridView::Tool::split); };
    gainToolButton.onClick = [this] { setTool (NoteGridView::Tool::gain); };
    editToolButton.setToggleState (true, juce::dontSendNotification);
    grid.onToolChanged = [this] (NoteGridView::Tool t) { setTool (t); };

    styleButton (playButton, Colours::accent);
    playButton.setTooltip ("Play / stop from the marker (Space). Click the time ruler: move the marker; double-click: "
                           "play from there; Alt+click a note: play from it. With ARA this drives the DAW; "
                           "without ARA MoonTune plays the edited vocal itself.");
    playButton.onClick = [this] { togglePlay(); };
    addAndMakeVisible (playButton);
    styleButton (breathButton, Colours::accent);
    breathButton.setClickingTogglesState (true);
    breathButton.setTooltip ("Breaths & consonants: selects every breath. Turn the Gain knob down to silence them all, "
                             "then click single breaths (or consonants s / ts / k) and bring them back up.");
    breathButton.onClick = [this] { grid.setBreathMode (breathButton.getToggleState()); inspector.setSelection (grid.getSelection()); };
    addAndMakeVisible (breathButton);

    waveButton.setTooltip ("Show the waveform of the vocal behind the notes");
    waveButton.setToggleState (true, juce::dontSendNotification);
    waveButton.onClick = [this] { grid.setWaveformVisible (waveButton.getToggleState()); };
    addAndMakeVisible (waveButton);

    overview.onNavigate = [this] (double seconds)
    {
        grid.showTime (seconds, 0.5);
        followSuspended = true;
    };
    addAndMakeVisible (overview);

    grid.setHistory (&p.getHistory());
    grid.onUserNavigated = [this] { followSuspended = true; };
    grid.onAuditionNote = [this] (double a, double b)
    {
        if (processor.isAuditionEnabled() && ! processor.isHostPlaying())
            processor.startAudition (a, b, true);
    };
    grid.onAuditionMove = [this] (double a, double b) { processor.updateAuditionRange (a, b); };
    grid.onAuditionStop = [this] { processor.stopAudition(); };
    grid.onTogglePitchClass = [this] (int pc)
    {
        auto* param = processor.getParameterState().getParameter (ParamIDs::scaleEdits.getParamID());
        const int edits = juce::roundToInt (param->convertFrom0to1 (param->getValue())) ^ (1 << pc);
        param->setValueNotifyingHost (param->convertTo0to1 ((float) edits));
    };
    grid.onResetScaleEdits = [this]
    {
        auto* param = processor.getParameterState().getParameter (ParamIDs::scaleEdits.getParamID());
        param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
    };
    grid.onLocate = [this] (double seconds) { locate (seconds); };
    grid.onPlayFrom = [this] (double seconds) { playFrom (seconds); };
    grid.onTogglePlay = [this] { togglePlay(); };
    grid.setEmptyMessage ("No vocal yet.\n"
                          "1. Press REC.   2. Play the vocal in your DAW.   3. Stop: MoonTune draws the notes.\n"
                          "4. Drag notes up/down (pitch) and left/right (time); the DAW plays the result.\n\n"
                          "Wheel: scroll pitch  |  Shift+wheel: scroll time  |  Ctrl+wheel: zoom time  |  Alt+wheel: zoom pitch");
    grid.onViewChanged = [this] (const NoteGridView::View& v) { storeView (v); };
    grid.onSelectionChanged = [this] { inspector.setSelection (grid.getSelection()); };
    addAndMakeVisible (grid);
    addAndMakeVisible (inspector);

    scaleToolbar.onScaleDisplayChanged = [this] (const KeyScale& scale, float highlight)
    {
        grid.setScaleDisplay (scale, highlight);
    };
    grid.setScaleDisplay (p.getKeyScale(),
                          p.getParameterState().getRawParameterValue (ParamIDs::scaleHighlight.getParamID())->load() / 100.0f);
    addAndMakeVisible (scaleToolbar);

    const int w = juce::jlimit (minWidth, maxWidth, (int) processor.getUiProperty (uiIds::width, defaultWidth));
    const int h = juce::jlimit (minHeight, maxHeight, (int) processor.getUiProperty (uiIds::height, defaultHeight));
    setResizable (true, true);
    setResizeLimits (minWidth, minHeight, maxWidth, maxHeight);
    setSize (w, h);

    restoreView();
    constructed = true;
    sendLookAndFeelChange();   // children created before being added pick up the MoonTune look

    hadNotes = processor.getNoteModel().size() > 0;
    timerCallback();
    startTimerHz (30);
}

MoonTuneAudioProcessorEditor::~MoonTuneAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void MoonTuneAudioProcessorEditor::updateUndoButtons()
{
    auto& h = processor.getHistory();
    undoButton.setEnabled (h.canUndo());
    redoButton.setEnabled (h.canRedo());
    undoButton.setTooltip (h.canUndo() ? "Undo: " + h.getUndoName() + " (Ctrl+Z)" : juce::String ("Nothing to undo"));
    redoButton.setTooltip (h.canRedo() ? "Redo: " + h.getRedoName() + " (Ctrl+Y)" : juce::String ("Nothing to redo"));
}

// --- Transport from inside MoonTune ------------------------------------------------------
// ARA: the DAW's own transport is driven. Without ARA a plug-in cannot move the DAW, so MoonTune
// plays the edited vocal itself (preview) while the DAW is stopped.
void MoonTuneAudioProcessorEditor::locate (double seconds)
{
    locator = seconds;
    locatorHostPos = seconds;
    grid.setLocator (seconds);
    if (processor.canControlHost())
        processor.requestHostLocate (seconds);
    else if (processor.isPreviewing())
        processor.startPreview (seconds);
}

void MoonTuneAudioProcessorEditor::playFrom (double seconds)
{
    locator = seconds;
    locatorHostPos = seconds;
    grid.setLocator (seconds);
    if (processor.canControlHost())
    {
        processor.requestHostLocate (seconds);
        processor.requestHostPlay();
    }
    else if (! processor.isHostPlaying())
        processor.startPreview (seconds);
    else
        statusLabel.setText ("The DAW is playing: stop it to preview inside MoonTune (or use MoonTune as ARA).",
                             juce::dontSendNotification);
}

void MoonTuneAudioProcessorEditor::togglePlay()
{
    if (processor.canControlHost())
    {
        // Space / PLAY: from the ruler marker if one is set, otherwise from where the DAW cursor
        // already is (the cursor is never moved to the start).
        if (processor.isHostPlaying()) processor.requestHostStop();
        else
        {
            if (locator >= 0.0) processor.requestHostLocate (locator);
            processor.requestHostPlay();
        }
        return;
    }
    if (processor.isPreviewing()) processor.stopAudition();
    else playFrom (locator >= 0.0 ? locator : (processor.hasSeenHostPosition() ? processor.getPlayheadSeconds() : 0.0));
}

void MoonTuneAudioProcessorEditor::setTool (NoteGridView::Tool t)
{
    grid.setTool (t);
    editToolButton.setToggleState (t == NoteGridView::Tool::edit, juce::dontSendNotification);
    splitToolButton.setToggleState (t == NoteGridView::Tool::split, juce::dontSendNotification);
    gainToolButton.setToggleState (t == NoteGridView::Tool::gain, juce::dontSendNotification);
}

// Peaks of the recorded vocal for the waveform behind the notes (rebuilt when the take changes,
// not while it is being recorded).
void MoonTuneAudioProcessorEditor::updateWaveform()
{
    auto take = processor.getCapture().getTake();
    if (take == nullptr || take->isEmpty())
    {
        if (waveTake != nullptr) { grid.setWaveform (nullptr); waveTake = nullptr; waveEnd = -1; }
        return;
    }
    const auto end = take->getEndSample();
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (take.get() == waveTake && end == waveEnd)
        return;
    if (processor.getCapture().isReceiving() || now - waveBuiltAt < 1000.0)
        return;
    waveBuiltAt = now;
    waveTake = take.get();
    waveEnd = end;

    auto peaks = std::make_shared<WavePeaks>();
    peaks->sampleRate = take->getSampleRate();
    peaks->block = 128;
    const auto nBlocks = (size_t) ((end + peaks->block - 1) / peaks->block);
    peaks->lo.assign (nBlocks, 0.0f);
    peaks->hi.assign (nBlocks, 0.0f);
    std::vector<float> buf ((size_t) peaks->block * 512);
    for (juce::int64 pos = 0; pos < end; pos += (juce::int64) buf.size())
    {
        const int n = (int) std::min<juce::int64> ((juce::int64) buf.size(), end - pos);
        take->read (pos, buf.data(), n);
        for (int i = 0; i < n; ++i)
        {
            const auto b = (size_t) ((pos + i) / peaks->block);
            peaks->lo[b] = std::min (peaks->lo[b], buf[(size_t) i]);
            peaks->hi[b] = std::max (peaks->hi[b], buf[(size_t) i]);
        }
    }
    for (size_t b = 0; b < nBlocks; ++b)
        peaks->maxAbs = std::max ({ peaks->maxAbs, peaks->hi[b], -peaks->lo[b] });
    grid.setWaveform (peaks);
}

void MoonTuneAudioProcessorEditor::updateEngineButtons()
{
    const int e = processor.getRenderOptions().engine;
    cleanButton.setToggleState (e == Engine::harmonyClean, juce::dontSendNotification);
    trueVoiceButton.setToggleState (e == Engine::trueVoice, juce::dontSendNotification);
    hybridButton.setToggleState (e == Engine::hybrid, juce::dontSendNotification);
}

// Keeps the DAW playhead in view: on opening the editor, when the host relocates while stopped,
// and page by page while playing. Scrolling by hand pauses this until the next Play.
void MoonTuneAudioProcessorEditor::updateFollow()
{
    const bool playing = processor.isHostPlaying();
    const double ph = processor.getPlayheadSeconds();
    if (playing && ! wasPlaying)
        followSuspended = false;
    wasPlaying = playing;

    if (! processor.hasSeenHostPosition())
        return;

    if (! initialJumpDone)
    {
        initialJumpDone = true;
        lastPlayhead = ph;
        if (! grid.isTimeVisible (ph, 0.02))
            grid.showTime (ph, 0.3);
        return;
    }
    const bool moved = std::abs (ph - lastPlayhead) > 0.02;
    const bool jumped = std::abs (ph - lastPlayhead) > 0.5;
    lastPlayhead = ph;

    if (! processor.isFollowPlayhead())
        return;
    if (jumped && ! playing)
        followSuspended = false;   // the user put the DAW cursor somewhere: show it
    if (followSuspended)
        return;
    if (playing && ! grid.isTimeVisible (ph, 0.04))
        grid.showTime (ph, 0.1);
    else if (! playing && moved && ! grid.isTimeVisible (ph, 0.02))
        grid.showTime (ph, 0.3);
}

void MoonTuneAudioProcessorEditor::timerCallback()
{
    auto& model = processor.getNoteModel();
    auto& analysis = processor.getAnalysis();
    const auto& capture = processor.getCapture();

    grid.setRenderOptions (processor.getRenderOptions());
    grid.setSnap (processor.isSnapToScaleEnabled(), processor.getSnapStrength());

    if (model.getVersion() != seenNoteVersion)
    {
        seenNoteVersion = model.getVersion();
        grid.refreshNotes();
        if (! processor.getHistory().isInGesture())
            inspector.refresh();
        const bool hasNotes = model.size() > 0;
        if (hasNotes && ! hadNotes)
            grid.fitToNotes();
        hadNotes = hasNotes;
    }

    if (analysis.getResultVersion() != seenResultVersion)
    {
        seenResultVersion = analysis.getResultVersion();
        grid.setAnalysis (analysis.getResult());
    }

    if (locator >= 0.0 && ! processor.isHostPlaying() && ! processor.isPreviewing() && processor.hasSeenHostPosition()
        && locatorHostPos >= 0.0 && std::abs (processor.getPlayheadSeconds() - locatorHostPos) > 0.05
        && std::abs (processor.getPlayheadSeconds() - lastPlayheadForLocator) > 0.05)
    {
        locator = -1.0;   // the DAW cursor was moved in the DAW: play from there from now on
        grid.setLocator (-1.0);
    }
    lastPlayheadForLocator = processor.getPlayheadSeconds();
    if (processor.isPreviewing())
        grid.setPlayhead (processor.getAuditionSeconds(), true);
    else
        grid.setPlayhead (processor.getPlayheadSeconds(), processor.isHostPlaying() || processor.isCaptureArmed()
                                                             || processor.hasSeenHostPosition());
    {
        const bool playingNow = processor.isPreviewing() || (processor.canControlHost() && processor.isHostPlaying());
        const juce::String t = playingNow ? "STOP" : "PLAY";
        if (playButton.getButtonText() != t) playButton.setButtonText (t);
    }
    {
        const auto tempo = processor.getHostTempo();
        grid.setMusicalGrid (tempo.bpm, tempo.ppqAtZero, tempo.numerator, tempo.denominator,
                             processor.getGridDivision(), processor.isGridTriplet());
    }
    updateFollow();
    updateWaveform();
    {
        const auto& v = grid.getView();
        const double viewEnd = v.startSeconds + (grid.getWidth() - NoteGridView::pitchRulerWidth) / v.pixelsPerSecond;
        double total = 0.0;
        if (auto take = capture.getTake()) total = (double) take->getEndSample() / take->getSampleRate();
        overview.update (model.getSnapshot(), total + 1.0, v.startSeconds, viewEnd, processor.getPlayheadSeconds(),
                         processor.hasSeenHostPosition());
    }
    updateUndoButtons();

    juce::String status;
    auto colour = Colours::textDim;

    const bool ara = processor.isAraMode();
    if (recButton.isEnabled() == ara)
    {
        recButton.setEnabled (! ara);
        clearButton.setEnabled (! ara);
        recButton.setTooltip (ara ? "Not needed in ARA mode: MoonTune reads the vocal directly from the DAW event."
                                  : "Record: while this is on, MoonTune listens to the track whenever the DAW plays "
                                    "and draws the notes when playback stops.");
        grid.setEmptyMessage (ara ? juce::String ("ARA mode: MoonTune reads the vocal from the DAW event...\n"
                                                  "Click the time ruler to play from that point.")
                                  : juce::String ("No vocal yet.\n"
                                                  "1. Press REC.   2. Play the vocal in your DAW.   3. Stop: MoonTune draws the notes.\n"
                                                  "4. Drag notes up/down (pitch) and left/right (time); the DAW plays the result.\n\n"
                                                  "Wheel: scroll pitch  |  Shift+wheel: scroll time  |  Ctrl+wheel: zoom time  |  Alt+wheel: zoom pitch"));
    }

    if (ara && processor.getAraStatus().isNotEmpty() && analysis.getStatus() != AnalysisManager::Status::analysing)
    {
        status = processor.getAraStatus();
        colour = Colours::accent;
    }
    else if (processor.isCaptureArmed() && capture.isReceiving())
    {
        status = juce::String::fromUTF8 ("\xe2\x97\x8f Recording ") + juce::String (capture.getSessionSeconds(), 1) + " s";
        colour = Colours::meterRed;
    }
    else if (analysis.getStatus() == AnalysisManager::Status::analysing)
    {
        status = analysis.getMessage() + " " + juce::String (juce::roundToInt (analysis.getProgress() * 100.0f)) + " %";
        colour = Colours::accent;
    }
    else if (analysis.getStatus() == AnalysisManager::Status::failed)
    {
        status = analysis.getMessage();
        colour = Colours::noteOutOfScale;
    }
    else if (processor.isCaptureArmed())
    {
        status = model.size() > 0 ? "REC armed, " + juce::String (model.size()) + " notes. Play again to re-record a part. "
                                    "Switch REC off to hear your edits."
                                  : juce::String ("REC armed: press Play in your DAW");
        colour = Colours::meterRed.withAlpha (0.8f);
    }
    else if (processor.getRenderer().isRendering())
    {
        status = "Updating sound...";
        colour = Colours::accent;
    }
    else if (model.size() > 0)
    {
        status = juce::String (model.size()) + " notes" + (ara ? " (ARA)" : "")
               + (processor.getRenderer().hasRenderedAudio() ? juce::String (". Edits play back in the DAW.")
                                                               : juce::String (". Drag notes to edit."));
    }
    else
    {
        status = ara ? "ARA: waiting for the vocal from the DAW." : "Press REC and play the vocal.";
    }

    if (statusLabel.getText() != status)
        statusLabel.setText (status, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, colour);
    if (recButton.getToggleState() != processor.isCaptureArmed())
        recButton.setToggleState (processor.isCaptureArmed(), juce::dontSendNotification);
}

void MoonTuneAudioProcessorEditor::analyseKey()
{
    const auto result = KeyDetector::detect (processor.getNoteModel().getSnapshot());

    juce::PopupMenu menu;
    menu.addSectionHeader (result.hasSuggestion() ? (result.ambiguous ? "Possible keys (not certain)" : "Suggested key")
                                                  : "Analyze Key");
    for (size_t i = 0; i < result.candidates.size(); ++i)
    {
        const auto& c = result.candidates[i];
        menu.addItem ((int) i + 1, c.key.getName() + "   " + juce::String (juce::roundToInt (c.confidence * 100.0f)) + " %",
                      true, processor.getKeyScale() == c.key);
    }
    menu.addSeparator();
    menu.addItem (1000, result.summary, false);
    if (result.hasSuggestion())
        menu.addItem (1001, "Choose one to apply, or close to keep your current key.", false);

    auto candidates = result.candidates;
    juce::Component::SafePointer<MoonTuneAudioProcessorEditor> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&keyButton),
                        [safe, candidates] (int choice)
                        {
                            if (safe == nullptr || choice < 1 || choice > (int) candidates.size())
                                return;
                            const auto& key = candidates[(size_t) choice - 1].key;
                            auto& state = safe->processor.getParameterState();
                            auto set = [&state] (const juce::ParameterID& id, float plain)
                            {
                                auto* param = state.getParameter (id.getParamID());
                                param->setValueNotifyingHost (param->convertTo0to1 (plain));
                            };
                            set (ParamIDs::keyRoot, (float) key.getRoot());
                            set (ParamIDs::scaleType, (float) key.getType());
                        });
}

void MoonTuneAudioProcessorEditor::showCorrectPitch()
{
    const auto scale = processor.getKeyScale();
    auto panel = std::make_unique<CorrectPitchPanel> (scale.isChromatic() ? juce::String ("Chromatic (nearest semitone)")
                                                                          : scale.getName(),
                                                      ! grid.getSelection().empty());
    juce::Component::SafePointer<MoonTuneAudioProcessorEditor> safe (this);
    beginCorrectionPreview();
    panel->onPreview = [safe] (const ScaleCorrector::Settings& s, bool selectedOnly)
    {
        if (safe != nullptr) safe->previewCorrection (s, selectedOnly);
    };
    panel->onApply = [safe] { if (safe != nullptr) safe->endCorrectionPreview (true); };
    panel->onCancel = [safe] { if (safe != nullptr) safe->endCorrectionPreview (false); };
    panel->onRemove = [safe] (bool selectedOnly)
    {
        if (safe == nullptr) return;
        safe->endCorrectionPreview (false);
        safe->removeCorrection (selectedOnly);
    };
    panel->scaleProvider = [safe] { return safe != nullptr ? safe->processor.getKeyScale() : KeyScale(); };
    panel->onTogglePitchClass = [safe] (int pc) { if (safe != nullptr && safe->grid.onTogglePitchClass) safe->grid.onTogglePitchClass (pc); };
    panel->onResetScale = [safe] { if (safe != nullptr && safe->grid.onResetScaleEdits) safe->grid.onResetScaleEdits(); };
    panel->startPreview();
    juce::CallOutBox::launchAsynchronously (std::move (panel), correctButton.getScreenBounds(), nullptr);
}

void MoonTuneAudioProcessorEditor::beginCorrectionPreview()
{
    if (correctionLive)
        endCorrectionPreview (true);
    auto& h = processor.getHistory();
    if (h.isInGesture())
        return;
    correctionOriginal = processor.getNoteModel().getSnapshot();
    h.beginGesture();
    correctionLive = true;
}

void MoonTuneAudioProcessorEditor::previewCorrection (const ScaleCorrector::Settings& s, bool selectedOnly)
{
    if (! correctionLive)
        return;
    const std::set<juce::int64> ids = selectedOnly ? grid.getSelection() : std::set<juce::int64>();
    processor.getHistory().updateGesture (ScaleCorrector::apply (correctionOriginal, ids, processor.getKeyScale(), s));
    grid.refreshNotes();
    inspector.refresh();
}

void MoonTuneAudioProcessorEditor::endCorrectionPreview (bool keep)
{
    if (! correctionLive)
        return;
    correctionLive = false;
    auto& h = processor.getHistory();
    if (! keep)
        h.updateGesture (correctionOriginal);
    h.endGesture ("Correct pitch");
    correctionOriginal.clear();
    grid.refreshNotes();
    inspector.refresh();
}

void MoonTuneAudioProcessorEditor::applyCorrection (const ScaleCorrector::Settings& s, bool selectedOnly)
{
    const std::set<juce::int64> ids = selectedOnly ? grid.getSelection() : std::set<juce::int64>();
    auto corrected = ScaleCorrector::apply (processor.getNoteModel().getSnapshot(), ids, processor.getKeyScale(), s);
    processor.getHistory().perform ("Correct pitch", std::move (corrected));
    grid.refreshNotes();
    inspector.refresh();
}

void MoonTuneAudioProcessorEditor::applyCorrectionForTesting (const ScaleCorrector::Settings& s, bool selectedOnly)
{
    applyCorrection (s, selectedOnly);
}

void MoonTuneAudioProcessorEditor::removeCorrection (bool selectedOnly)
{
    const std::set<juce::int64> ids = selectedOnly ? grid.getSelection() : std::set<juce::int64>();
    processor.getHistory().perform ("Remove correction", ScaleCorrector::remove (processor.getNoteModel().getSnapshot(), ids));
    grid.refreshNotes();
    inspector.refresh();
}

void MoonTuneAudioProcessorEditor::restoreView()
{
    NoteGridView::View v;
    v.startSeconds      = (double) processor.getUiProperty (uiIds::viewStart, v.startSeconds);
    v.pixelsPerSecond   = (double) processor.getUiProperty (uiIds::viewPps, v.pixelsPerSecond);
    v.topPitch          = (float) (double) processor.getUiProperty (uiIds::viewTop, (double) v.topPitch);
    v.pixelsPerSemitone = (float) (double) processor.getUiProperty (uiIds::viewPpst, (double) v.pixelsPerSemitone);
    grid.setView (v);
}

void MoonTuneAudioProcessorEditor::storeView (const NoteGridView::View& v)
{
    processor.setUiProperty (uiIds::viewStart, v.startSeconds);
    processor.setUiProperty (uiIds::viewPps, v.pixelsPerSecond);
    processor.setUiProperty (uiIds::viewTop, (double) v.topPitch);
    processor.setUiProperty (uiIds::viewPpst, (double) v.pixelsPerSemitone);
}

void MoonTuneAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);

    auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (Colours::panel);
    g.fillRect (header);
    g.setColour (Colours::gridLineC);
    g.fillRect (header.removeFromBottom (1));

    auto titleArea = header.reduced (16, 0).removeFromLeft (150);
    g.setColour (Colours::accent);
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText ("MoonTune", titleArea.removeFromLeft (104), juce::Justification::centredLeft);
    g.setColour (Colours::textDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("v" MOONTUNE_VERSION_STRING, titleArea, juce::Justification::centredLeft);

    // Background behind the processing switches (same strip as the scale toolbar).
    auto strip = getLocalBounds().withTrimmedTop (headerHeight).removeFromTop (ScaleToolbar::preferredHeight);
    g.setColour (Colours::panel.darker (0.15f));
    g.fillRect (strip.removeFromRight (440));
    auto nav = getLocalBounds().withTrimmedTop (headerHeight + ScaleToolbar::preferredHeight).removeFromTop (navHeight);
    g.setColour (Colours::panel);
    g.fillRect (nav);
    g.setColour (Colours::gridLineC);
    g.fillRect (nav.removeFromBottom (1));
}

void MoonTuneAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    auto header = area.removeFromTop (headerHeight).reduced (16, 10);

    header.removeFromLeft (150);
    recButton.setBounds (header.removeFromLeft (52));
    header.removeFromLeft (6);
    clearButton.setBounds (header.removeFromLeft (58));
    header.removeFromLeft (12);
    undoButton.setBounds (header.removeFromLeft (56));
    header.removeFromLeft (4);
    redoButton.setBounds (header.removeFromLeft (56));
    header.removeFromLeft (12);
    keyButton.setBounds (header.removeFromLeft (100));
    header.removeFromLeft (6);
    correctButton.setBounds (header.removeFromLeft (118));
    header.removeFromLeft (12);

    bypassButton.setBounds (header.removeFromRight (56));
    header.removeFromRight (10);
    meter.setBounds (header.removeFromRight (70).withSizeKeepingCentre (70, 8));
    header.removeFromRight (10);
    outputGainSlider.setBounds (header.removeFromRight (140));
    outputGainLabel.setBounds (header.removeFromRight (48));
    header.removeFromRight (8);
    statusLabel.setBounds (header);

    auto toolbar = area.removeFromTop (ScaleToolbar::preferredHeight);
    auto stageArea = toolbar.removeFromRight (440).reduced (8, 8);
    scaleToolbar.setBounds (toolbar);
    stagesLabel.setBounds (stageArea.removeFromLeft (62));
    const int sw = stageArea.getWidth() / (int) stages.size();
    for (auto& s : stages)
        s.button->setBounds (stageArea.removeFromLeft (sw).reduced (2, 0));

    auto nav = area.removeFromTop (navHeight).reduced (16, 4);
    engineLabel.setBounds (nav.removeFromLeft (46));
    nav.removeFromLeft (6);
    cleanButton.setBounds (nav.removeFromLeft (108));
    trueVoiceButton.setBounds (nav.removeFromLeft (84));
    hybridButton.setBounds (nav.removeFromLeft (62));
    nav.removeFromLeft (10);
    gridLabel.setBounds (nav.removeFromLeft (30));
    nav.removeFromLeft (6);
    gridBox.setBounds (nav.removeFromLeft (70));
    tripletButton.setBounds (nav.removeFromLeft (40));
    nav.removeFromLeft (10);
    followButton.setBounds (nav.removeFromLeft (70));
    auditionButton.setBounds (nav.removeFromLeft (82));
    nav.removeFromLeft (6);
    showAllButton.setBounds (nav.removeFromLeft (72));
    nav.removeFromLeft (6);
    playButton.setBounds (nav.removeFromLeft (52));
    nav.removeFromLeft (10);
    editToolButton.setBounds (nav.removeFromLeft (52));
    splitToolButton.setBounds (nav.removeFromLeft (56));
    gainToolButton.setBounds (nav.removeFromLeft (54));
    nav.removeFromLeft (6);
    breathButton.setBounds (nav.removeFromLeft (66));
    nav.removeFromLeft (4);
    waveButton.setBounds (nav.removeFromLeft (66));
    nav.removeFromLeft (8);
    overview.setBounds (nav);

    inspector.setBounds (area.removeFromBottom (NoteInspector::preferredHeight));
    grid.setBounds (area);

    if (constructed)
    {
        processor.setUiProperty (uiIds::width, getWidth());
        processor.setUiProperty (uiIds::height, getHeight());
    }
}

} // namespace moontune
