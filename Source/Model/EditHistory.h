#pragma once

#include <juce_core/juce_core.h>
#include <deque>
#include <vector>
#include "NoteModel.h"

namespace moontune
{

// Undo / Redo for note edits.
//
// Every edit is stored as a pair of complete note-list snapshots (before / after).
// Note lists are small (hundreds of notes), so this is cheap and can never get out
// of step with the model the way fine-grained commands can.
//
// If the note list is changed from outside (a new recording replaced notes), the
// history is cleared instead of letting an undo bring back stale notes.
//
// Message thread only.
class EditHistory
{
public:
    explicit EditHistory (NoteModel& m) : model (m) {}

    // Applies `after` to the model and records the step. No-op if nothing changed.
    void perform (const juce::String& name, std::vector<Note> after);

    // For continuous gestures (dragging): begin() remembers the state, update() applies
    // intermediate states without recording, end() records one step for the whole gesture.
    void beginGesture();
    void updateGesture (std::vector<Note> current);
    void endGesture (const juce::String& name);
    bool isInGesture() const noexcept { return inGesture; }

    bool canUndo() const;
    bool canRedo() const;
    juce::String getUndoName() const;
    juce::String getRedoName() const;
    bool undo();
    bool redo();
    void clear();

    static constexpr size_t maxSteps = 300;

private:
    struct Step { juce::String name; std::vector<Note> before, after; };

    void apply (std::vector<Note> notes);
    void checkExternalChange();

    NoteModel& model;
    std::deque<Step> undoStack, redoStack;
    juce::uint32 expectedVersion = 0;
    bool versionKnown = false;

    bool inGesture = false;
    std::vector<Note> gestureStart;
};

} // namespace moontune
