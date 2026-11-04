#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>

#include "Model/NoteModel.h"

namespace moontune
{

// Whole-take overview: every note as a small mark, the visible part of the editor as a frame,
// the playhead as a line. Click or drag to move the editor there.
class OverviewBar final : public juce::Component
{
public:
    void update (const std::vector<Note>& notes, double totalSeconds, double viewStart, double viewEnd,
                 double playhead, bool playheadVisible);
    std::function<void (double seconds)> onNavigate;   // centre the editor on this time

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    double xToSeconds (float x) const noexcept;
    float secondsToX (double s) const noexcept;

    struct Mark { double start, end; float pitch; };
    std::vector<Mark> marks;
    double total = 10.0, v0 = 0.0, v1 = 1.0, play = 0.0;
    bool playVisible = false;
    float lo = 48.0f, hi = 72.0f;
};

} // namespace moontune
