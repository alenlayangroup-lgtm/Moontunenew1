#include "EditHistory.h"

namespace moontune
{

void EditHistory::checkExternalChange()
{
    if (versionKnown && model.getVersion() != expectedVersion)
    {
        undoStack.clear();
        redoStack.clear();
    }
}

void EditHistory::apply (std::vector<Note> notes)
{
    for (auto& n : notes)
        NoteModel::clampEdits (n);
    model.replaceAll (std::move (notes));
    expectedVersion = model.getVersion();
    versionKnown = true;
}

void EditHistory::perform (const juce::String& name, std::vector<Note> after)
{
    checkExternalChange();
    auto before = model.getSnapshot();
    for (auto& n : after)
        NoteModel::clampEdits (n);
    if (before == after)
        return;

    apply (after);
    undoStack.push_back ({ name, std::move (before), model.getSnapshot() });
    if (undoStack.size() > maxSteps)
        undoStack.pop_front();
    redoStack.clear();
}

void EditHistory::beginGesture()
{
    checkExternalChange();
    gestureStart = model.getSnapshot();
    inGesture = true;
}

void EditHistory::updateGesture (std::vector<Note> current)
{
    if (inGesture)
        apply (std::move (current));
}

void EditHistory::endGesture (const juce::String& name)
{
    if (! inGesture)
        return;
    inGesture = false;
    auto after = model.getSnapshot();
    if (after == gestureStart)
        return;
    undoStack.push_back ({ name, std::move (gestureStart), std::move (after) });
    if (undoStack.size() > maxSteps)
        undoStack.pop_front();
    redoStack.clear();
}

bool EditHistory::canUndo() const
{
    return ! undoStack.empty() && (! versionKnown || model.getVersion() == expectedVersion);
}

bool EditHistory::canRedo() const
{
    return ! redoStack.empty() && (! versionKnown || model.getVersion() == expectedVersion);
}

juce::String EditHistory::getUndoName() const { return undoStack.empty() ? juce::String() : undoStack.back().name; }
juce::String EditHistory::getRedoName() const { return redoStack.empty() ? juce::String() : redoStack.back().name; }

bool EditHistory::undo()
{
    checkExternalChange();
    if (undoStack.empty())
        return false;
    auto step = std::move (undoStack.back());
    undoStack.pop_back();
    apply (step.before);
    redoStack.push_back (std::move (step));
    return true;
}

bool EditHistory::redo()
{
    checkExternalChange();
    if (redoStack.empty())
        return false;
    auto step = std::move (redoStack.back());
    redoStack.pop_back();
    apply (step.after);
    undoStack.push_back (std::move (step));
    return true;
}

void EditHistory::clear()
{
    undoStack.clear();
    redoStack.clear();
    versionKnown = false;
}

} // namespace moontune
