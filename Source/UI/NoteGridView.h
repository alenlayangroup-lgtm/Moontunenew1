#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <set>

#include "Model/NoteModel.h"
#include "Model/EditHistory.h"
#include "Music/KeyScale.h"
#include "Analysis/AnalysisResult.h"
#include "DSP/VocalRenderer.h"
#include "DSP/TimeMap.h"

namespace moontune
{

// Main editing area: time ruler (top), pitch ruler (left), semitone grid and notes.
//
// Notes are drawn as loudness envelopes with the pitch curve as it will sound
// (after edits). Breaths / consonants are drawn as grey noise.
//
// Mouse (one tool, no mode switching):
//   drag note body          - vertical: pitch (snaps to semitones, or to the scale if
//                             Snap to Scale is on); horizontal: time. Direction locks
//                             after a few pixels. Hold Ctrl/Cmd for free pitch (cents).
//   drag note edge          - change start / end (stretch)
//   double-click note       - put the note exactly on the nearest note (scale/semitone)
//   drag on empty area      - rubber-band selection (Shift adds)
//   right-click             - note menu (reset edits, fix analysis)
//   wheel / Shift / Ctrl / Alt + wheel - scroll and zoom; middle button or Alt+drag - pan
// Keys: Up/Down semitone, Shift+Up/Down 10 cents, Left/Right 10 ms, Ctrl+A, Esc,
//       Delete, Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y.
// Min/max peaks of the recorded vocal, for the waveform drawn behind the notes.
struct WavePeaks
{
    double sampleRate = 44100.0;
    int block = 128;                     // samples per peak
    std::vector<float> lo, hi;
    float maxAbs = 0.0f;
};

class NoteGridView final : public juce::Component
{
public:
    // Mouse tools: Edit (pitch / time / length / handles), Split (scissors: click cuts the note
    // there), Gain (drag a note up / down: louder / quieter).
    enum class Tool { edit, split, gain };
    void setTool (Tool);
    Tool getTool() const noexcept { return tool; }
    std::function<void (Tool)> onToolChanged;   // changed from the keyboard (E / S / G)

    // BREATH mode: only breaths / consonants can be clicked and lassoed (notes are left alone);
    // turning it on selects every breath.
    void setBreathMode (bool);
    bool isBreathMode() const noexcept { return breathMode; }
    bool isSelectable (const Note&) const noexcept;

    void setWaveform (std::shared_ptr<const WavePeaks>);
    void setWaveformVisible (bool);

    struct View
    {
        double startSeconds = 0.0;
        double pixelsPerSecond = 100.0;
        float  topPitch = 84.5f;          // MIDI pitch at the top edge (C6 lane fully visible)
        float  pixelsPerSemitone = 14.0f;
    };

    static constexpr float  lowestPitch = 24.0f;   // C1
    static constexpr float  highestPitch = 96.0f;  // C7
    static constexpr double minPixelsPerSecond = 10.0, maxPixelsPerSecond = 4000.0;
    static constexpr float  minPixelsPerSemitone = 6.0f, maxPixelsPerSemitone = 48.0f;
    static constexpr int    pitchRulerWidth = 56, timeRulerHeight = 24;
    static constexpr float  edgeGrabPixels = 6.0f;

    explicit NoteGridView (NoteModel& model);

    void setHistory (EditHistory* h) noexcept { history = h; }
    void setView (const View& newView);
    const View& getView() const noexcept { return view; }

    void setEmptyMessage (const juce::String& message);

    // Scale highlighting and snapping.
    void setScaleDisplay (const KeyScale& scale, float highlight);
    void setSnap (bool enabled, float strength) noexcept { snapEnabled = enabled; snapStrength = strength; }
    const KeyScale& getScale() const noexcept { return keyScale; }
    bool scaleHighlightActive() const noexcept { return ! keyScale.isChromatic() && highlightAmount > 0.0f; }
    juce::Colour laneColour (int midiNote) const noexcept;

    void refreshNotes();   // re-read the model and repaint
    void setRenderOptions (const RenderOptions&);
    void setAnalysis (std::shared_ptr<const AnalysisResult>);
    void setPlayhead (double seconds, bool visible);
    double getPlayhead() const noexcept { return playhead; }

    // Musical grid from the host tempo (division 0 = off; 4 = quarter ... 64). Time drags and
    // edge drags snap to it (hold Ctrl/Cmd for free movement).
    void setMusicalGrid (double bpm, double ppqAtZero, int numerator, int denominator, int division, bool triplet);
    double snapTime (double seconds) const noexcept;
    bool hasMusicalGrid() const noexcept { return gridBpm > 0.0 && gridDivision > 0; }

    // Scroll so that `seconds` is visible (keeps zoom). `atFraction` = where it lands (0..1 of width).
    void showTime (double seconds, double atFraction);
    bool isTimeVisible (double seconds, double marginFraction = 0.0) const noexcept;

    // Callbacks
    std::function<void (double outStart, double outEnd, float pitch)> onAuditionNote;  // a note was grabbed
    std::function<void (double outStart, double outEnd, float pitch)> onAuditionMove;  // the grabbed note moved
    std::function<void()> onAuditionStop;                                 // released
    std::function<void (int pitchClass)> onTogglePitchClass;               // pitch ruler right-click
    std::function<void()> onResetScaleEdits;
    std::function<void (double seconds)> onLocate;                         // click in the time ruler
    std::function<void (double seconds)> onPlayFrom;                       // double-click in the ruler, Alt+click on a note
    std::function<void()> onTogglePlay;                                    // space bar
    void setLocator (double seconds);                                      // play-from marker in the ruler
    std::function<void()> onUserNavigated;                                 // user scrolled / zoomed
    void fitToNotes();     // scroll/zoom so all notes are visible

    // Selection
    const std::set<juce::int64>& getSelection() const noexcept { return selection; }
    void setSelection (std::set<juce::int64>);
    void selectAll();
    void stepToNote (bool forward);   // Tab / Alt+arrows: next / previous note
    std::function<void()> onSelectionChanged;
    std::function<void (const View&)> onViewChanged;

    // Editing (all go through the edit history when one is set).
    int noteIndexAt (juce::Point<float> position) const;
    void splitNoteAt (juce::int64 noteId, double sourceSeconds);
    void mergeWithNext (juce::int64 noteId);
    void shiftOctave (juce::int64 noteId, int octaves);
    void deleteNotes (const std::set<juce::int64>& ids);
    void nudgePitch (float cents);                 // selected notes
    void nudgeTime (double seconds);               // selected notes
    void snapSelectionToNearest();                 // selected notes onto the nearest note
    void resetEdits (bool pitch, bool time, bool sound);

    // Pitch-drag result for one note (exposed for tests): the new pitchShiftCents.
    float draggedShift (const Note& original, float deltaSemitones, bool freeMode) const;

    // Coordinate helpers (public for tests and future editing tools).
    float pitchToY (float pitch) const noexcept;
    float yToPitch (float y) const noexcept;
    float secondsToX (double seconds) const noexcept;
    double xToSeconds (float x) const noexcept;
    juce::Rectangle<float> noteBounds (const Note&) const;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    enum class Drag { none, pan, pending, pitch, time, startEdge, endEdge, lasso, modHandle, driftHandle, gain,
                      tiltHandle, transitionHandle };
    enum class Hit { none, body, startEdge, endEdge, modHandle, driftHandle, tiltHandle, transitionHandle };
    bool handleRects (const Note&, juce::Rectangle<float>& mod, juce::Rectangle<float>& drift) const;
    juce::Rectangle<float> tiltHandleRect (const Note&) const;
    // Transition handle sits on the joint before note n (if a note ends within 0.6 s before it).
    bool transitionHandleRect (const Note& n, juce::Rectangle<float>&) const;
    void paintNoiseObject (juce::Graphics&, const Note&, bool selected);
    std::set<juce::int64> attachedConsonants (const std::set<juce::int64>& noteIds) const;
    void paintHandles (juce::Graphics&);
    void paintWaveform (juce::Graphics&, juce::Rectangle<int> area);
    void showScaleMenu (int pitchClass);
    void auditionDragged();

    void paintGrid (juce::Graphics&, juce::Rectangle<int> area);
    void paintNotes (juce::Graphics&, juce::Rectangle<int> area);
    void paintNoise (juce::Graphics&, juce::Rectangle<int> area);
    void paintNoteShape (juce::Graphics&, const Note&, juce::Colour, bool selected);
    void paintPitchRuler (juce::Graphics&, juce::Rectangle<int> area);
    void paintTimeRuler (juce::Graphics&, juce::Rectangle<int> area);
    void paintDragInfo (juce::Graphics&);
    void showNoteMenu (juce::int64 noteId, double sourceSeconds);
    void constrainView();
    void viewChanged();
    double timeGridStep() const noexcept;
    juce::Rectangle<int> gridArea() const noexcept;
    Hit hitTestNote (juce::Point<float>, int& index) const;
    void rebuildTimeMap();
    double toOutput (double sourceSeconds) const noexcept;
    float displayPitch (const Note&) const noexcept;
    void commit (const juce::String& name, std::vector<Note> notes);
    void selectionChanged();
    void applyDrag (const juce::MouseEvent&);
    juce::String pitchLabel (float midi) const;

    NoteModel& noteModel;
    EditHistory* history = nullptr;
    juce::uint32 seenModelVersion = 0;
    void syncWithModel();
    std::vector<Note> notes;
    View view;
    juce::String emptyMessage;
    KeyScale keyScale;              // default: chromatic
    float highlightAmount = 0.5f;
    bool snapEnabled = false;
    float snapStrength = 1.0f;
    RenderOptions options;
    std::shared_ptr<const AnalysisResult> analysis;
    TimeMap timeMap;
    double playhead = 0.0;
    bool playheadVisible = false;
    std::set<juce::int64> selection;

    // Drag state
    Drag drag = Drag::none;
    juce::Point<float> dragStart;
    View panStartView;
    std::vector<Note> dragOrigin;      // model snapshot at mouse-down
    juce::int64 dragNoteId = 0;
    std::set<juce::int64> lassoBase;
    juce::Rectangle<float> lassoRect;
    bool dragFree = false;

    Tool tool = Tool::edit;
    bool breathMode = false;
    double locator = -1.0;
    std::shared_ptr<const WavePeaks> wave;
    bool waveVisible = true;
    float splitHoverX = -1.0f;            // scissors preview line
    juce::int64 splitHoverNote = 0;

    double gridBpm = 0.0, gridPpqAtZero = 0.0;
    int gridNum = 4, gridDen = 4, gridDivision = 0;
    bool gridTriplet = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteGridView)
};

} // namespace moontune
