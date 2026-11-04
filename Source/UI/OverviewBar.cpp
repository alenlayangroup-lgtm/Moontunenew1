#include "OverviewBar.h"
#include "MoonLookAndFeel.h"

namespace moontune
{

void OverviewBar::update (const std::vector<Note>& notes, double totalSeconds, double viewStart, double viewEnd,
                          double playhead, bool playheadVisible)
{
    std::vector<Mark> m;
    m.reserve (notes.size());
    float l = 200.0f, h = -200.0f;
    for (const auto& n : notes)
    {
        if (! n.isNote()) continue;
        const float p = n.editedPitch (true);
        m.push_back ({ n.outStart(), n.outEnd(), p });
        l = std::min (l, p); h = std::max (h, p);
    }
    const double t = std::max ({ 1.0, totalSeconds, viewEnd });
    bool changed = m.size() != marks.size() || std::abs (t - total) > 1.0e-6 || std::abs (viewStart - v0) > 1.0e-6
                || std::abs (viewEnd - v1) > 1.0e-6 || playheadVisible != playVisible || std::abs (playhead - play) > 1.0e-3;
    for (size_t i = 0; ! changed && i < m.size(); ++i)
        changed = std::abs (m[i].start - marks[i].start) > 1.0e-6 || std::abs (m[i].pitch - marks[i].pitch) > 1.0e-4f;
    if (! changed)
        return;
    marks = std::move (m);
    total = t; v0 = viewStart; v1 = viewEnd; play = playhead; playVisible = playheadVisible;
    lo = marks.empty() ? 48.0f : l - 2.0f;
    hi = marks.empty() ? 72.0f : h + 2.0f;
    repaint();
}

double OverviewBar::xToSeconds (float x) const noexcept
{
    return (double) (x - 2.0f) / std::max (1.0f, (float) getWidth() - 4.0f) * total;
}

float OverviewBar::secondsToX (double s) const noexcept
{
    return 2.0f + (float) (s / total) * std::max (1.0f, (float) getWidth() - 4.0f);
}

void OverviewBar::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (Colours::background);
    g.fillRoundedRectangle (r, 3.0f);
    const float h = r.getHeight() - 6.0f;
    g.setColour (Colours::note.withAlpha (0.8f));
    for (const auto& m : marks)
    {
        const float y = 3.0f + (hi - m.pitch) / std::max (1.0f, hi - lo) * h;
        g.fillRect (juce::Rectangle<float> (secondsToX (m.start), y - 1.0f, std::max (1.0f, secondsToX (m.end) - secondsToX (m.start)), 2.0f));
    }
    const auto frame = juce::Rectangle<float> (secondsToX (v0), 1.0f, std::max (4.0f, secondsToX (v1) - secondsToX (v0)), r.getHeight() - 2.0f);
    g.setColour (Colours::accent.withAlpha (0.12f));
    g.fillRect (frame);
    g.setColour (Colours::accent.withAlpha (0.8f));
    g.drawRect (frame, 1.0f);
    if (playVisible)
    {
        g.setColour (Colours::text);
        g.fillRect (juce::Rectangle<float> (secondsToX (play), 0.0f, 1.0f, r.getHeight()));
    }
}

void OverviewBar::mouseDown (const juce::MouseEvent& e)
{
    if (onNavigate) onNavigate (juce::jmax (0.0, xToSeconds (e.position.x)));
}

void OverviewBar::mouseDrag (const juce::MouseEvent& e)
{
    mouseDown (e);
}

} // namespace moontune
