#include "NoteGridView.h"
#include "MoonLookAndFeel.h"
#include "Analysis/VocalAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace moontune
{

namespace
{
    bool isBlackKey (int midiNote) noexcept
    {
        switch (((midiNote % 12) + 12) % 12)
        {
            case 1: case 3: case 6: case 8: case 10: return true;
            default: return false;
        }
    }

    juce::String formatTime (double seconds)
    {
        const int minutes = (int) (seconds / 60.0);
        const double rest = seconds - minutes * 60.0;
        return juce::String (minutes) + ":" + juce::String (rest, 1).paddedLeft ('0', 4);
    }

    const char* const pitchNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    Note* findNote (std::vector<Note>& v, juce::int64 id)
    {
        for (auto& n : v)
            if (n.id == id)
                return &n;
        return nullptr;
    }
}

NoteGridView::NoteGridView (NoteModel& model) : noteModel (model)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);
    refreshNotes();
}

// --- State --------------------------------------------------------------------------

void NoteGridView::setAnalysis (std::shared_ptr<const AnalysisResult> r)
{
    analysis = std::move (r);
    rebuildTimeMap();
    repaint();
}

void NoteGridView::setRenderOptions (const RenderOptions& o)
{
    if (o == options)
        return;
    options = o;
    rebuildTimeMap();
    repaint();
}

void NoteGridView::rebuildTimeMap()
{
    if (analysis == nullptr || analysis->frames.empty() || ! options.time)
    {
        timeMap = TimeMap();
        return;
    }
    const double end = (double) analysis->frames.size() * analysis->hopSeconds;
    timeMap = TimeMap::build (notes, analysis.get(), 0.0, end, options.protectConsonants);
}

double NoteGridView::toOutput (double sourceSeconds) const noexcept
{
    return timeMap.toOutput (sourceSeconds);
}

float NoteGridView::displayPitch (const Note& n) const noexcept
{
    return n.isNote() ? n.editedPitch (options.scaleCorrection) : n.detectedPitch;
}

bool NoteGridView::isSelectable (const Note& n) const noexcept
{
    return breathMode ? ! n.isNote() : true;
}

void NoteGridView::setBreathMode (bool on)
{
    if (on == breathMode) return;
    breathMode = on;
    if (on)
    {
        std::set<juce::int64> b;
        for (const auto& n : notes) if (n.kind == Note::breath) b.insert (n.id);
        setSelection (std::move (b));
    }
    else
        setSelection ({});
    repaint();
}

// Consonants touching the start of a note (ending at most 40 ms before it): they move with it.
std::set<juce::int64> NoteGridView::attachedConsonants (const std::set<juce::int64>& noteIds) const
{
    std::set<juce::int64> out;
    for (const auto& n : notes)
    {
        if (! n.isNote() || noteIds.count (n.id) == 0) continue;
        for (const auto& c : notes)
            if ((c.kind == Note::sibilant || c.kind == Note::plosive) && noteIds.count (c.id) == 0
                && c.outEnd() <= n.outStart() + 0.005 && c.outEnd() >= n.outStart() - 0.04)
                out.insert (c.id);
    }
    return out;
}

void NoteGridView::setPlayhead (double seconds, bool visible)
{
    if (juce::exactlyEqual (seconds, playhead) && visible == playheadVisible)
        return;

    const auto strip = [this] (double t) { return juce::Rectangle<int> ((int) secondsToX (t) - 2, 0, 5, getHeight()); };
    if (playheadVisible) repaint (strip (playhead));
    playhead = seconds;
    playheadVisible = visible;
    if (playheadVisible) repaint (strip (playhead));
}

void NoteGridView::setLocator (double seconds)
{
    if (juce::exactlyEqual (seconds, locator)) return;
    locator = seconds;
    repaint (0, 0, getWidth(), timeRulerHeight);
}

void NoteGridView::setTool (Tool t)
{
    if (t == tool) return;
    tool = t;
    splitHoverX = -1.0f;
    repaint();
}

void NoteGridView::setWaveform (std::shared_ptr<const WavePeaks> w)
{
    wave = std::move (w);
    repaint();
}

void NoteGridView::setWaveformVisible (bool v)
{
    if (v == waveVisible) return;
    waveVisible = v;
    repaint();
}

// The recorded vocal as a faint waveform across the grid (following time edits).
void NoteGridView::paintWaveform (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (! waveVisible || wave == nullptr || wave->hi.empty() || wave->maxAbs <= 0.0f)
        return;
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area);
    const auto clip = g.getClipBounds();
    const float mid = (float) area.getCentreY();
    const float scale = (float) area.getHeight() * 0.42f / wave->maxAbs;
    const double perBlock = wave->block / wave->sampleRate;
    const int n = (int) wave->hi.size();
    juce::RectangleList<float> bars;
    for (int x = std::max (clip.getX(), area.getX()); x < std::min (clip.getRight(), area.getRight()); ++x)
    {
        const double s0 = timeMap.toSource (xToSeconds ((float) x)), s1 = timeMap.toSource (xToSeconds ((float) x + 1.0f));
        int b0 = (int) std::floor (std::min (s0, s1) / perBlock), b1 = (int) std::ceil (std::max (s0, s1) / perBlock);
        b0 = std::max (0, b0); b1 = std::min (n, std::max (b0 + 1, b1));
        if (b0 >= n) break;
        float lo = 0.0f, hi = 0.0f;
        for (int b = b0; b < b1; ++b) { lo = std::min (lo, wave->lo[(size_t) b]); hi = std::max (hi, wave->hi[(size_t) b]); }
        if (hi - lo < 1.0e-5f) continue;
        bars.addWithoutMerging ({ (float) x, mid - hi * scale, 1.0f, std::max (1.0f, (hi - lo) * scale) });
    }
    g.setColour (Colours::textDim.withAlpha (0.16f));
    g.fillRectList (bars);
}

void NoteGridView::setMusicalGrid (double bpm, double ppqAtZero, int numerator, int denominator, int division, bool triplet)
{
    if (juce::exactlyEqual (bpm, gridBpm) && juce::exactlyEqual (ppqAtZero, gridPpqAtZero) && numerator == gridNum
        && denominator == gridDen && division == gridDivision && triplet == gridTriplet)
        return;
    gridBpm = bpm; gridPpqAtZero = ppqAtZero; gridNum = std::max (1, numerator); gridDen = std::max (1, denominator);
    gridDivision = division; gridTriplet = triplet;
    repaint();
}

double NoteGridView::snapTime (double seconds) const noexcept
{
    if (! hasMusicalGrid())
        return seconds;
    const double stepPpq = 4.0 / gridDivision * (gridTriplet ? 2.0 / 3.0 : 1.0);
    const double ppq = gridPpqAtZero + seconds * gridBpm / 60.0;
    const double snapped = std::round (ppq / stepPpq) * stepPpq;
    return (snapped - gridPpqAtZero) * 60.0 / gridBpm;
}

void NoteGridView::showTime (double seconds, double atFraction)
{
    const double width = (double) gridArea().getWidth() / view.pixelsPerSecond;
    View v = view;
    v.startSeconds = std::max (0.0, seconds - width * atFraction);
    if (std::abs (v.startSeconds - view.startSeconds) < 1.0e-6)
        return;
    setView (v);
    if (onViewChanged)
        onViewChanged (view);
}

bool NoteGridView::isTimeVisible (double seconds, double marginFraction) const noexcept
{
    const double width = (double) gridArea().getWidth() / view.pixelsPerSecond;
    return seconds >= view.startSeconds + width * marginFraction && seconds <= view.startSeconds + width * (1.0 - marginFraction);
}

void NoteGridView::fitToNotes()
{
    if (notes.empty())
        return;

    float lo = 200.0f, hi = -200.0f;
    for (const auto& n : notes)
    {
        if (! n.isNote()) continue;
        lo = std::min (lo, displayPitch (n));
        hi = std::max (hi, displayPitch (n));
    }

    View v = view;
    v.startSeconds = std::max (0.0, notes.front().outStart() - 0.5);
    const float span = (hi - lo) + 8.0f;
    const float available = (float) gridArea().getHeight();
    if (available > 0.0f && span * v.pixelsPerSemitone > available)
        v.pixelsPerSemitone = std::max (minPixelsPerSemitone, available / span);
    v.topPitch = hi + 4.5f;
    setView (v);
    if (onViewChanged)
        onViewChanged (view);
}

void NoteGridView::setView (const View& newView)
{
    view = newView;
    constrainView();
    repaint();
}

void NoteGridView::setScaleDisplay (const KeyScale& scale, float highlight)
{
    const float h = std::isfinite (highlight) ? juce::jlimit (0.0f, 1.0f, highlight) : 0.0f;
    if (scale == keyScale && juce::exactlyEqual (h, highlightAmount))
        return;
    keyScale = scale;
    highlightAmount = h;
    repaint();   // display only - no audio analysis involved
}

juce::Colour NoteGridView::laneColour (int midiNote) const noexcept
{
    if (! scaleHighlightActive())
        return isBlackKey (midiNote) ? Colours::gridBlackKey : Colours::gridWhiteKey;

    if (keyScale.isRoot (midiNote))
        return Colours::gridWhiteKey.interpolatedWith (Colours::accent, 0.16f * highlightAmount);
    if (keyScale.containsMidi (midiNote))
        return Colours::gridWhiteKey.interpolatedWith (Colours::accent, 0.05f * highlightAmount);
    return Colours::gridWhiteKey.interpolatedWith (Colours::gridOutOfScale, highlightAmount);
}

void NoteGridView::setEmptyMessage (const juce::String& message)
{
    emptyMessage = message;
    repaint();
}

void NoteGridView::syncWithModel()
{
    if (noteModel.getVersion() != seenModelVersion)
        refreshNotes();
}

void NoteGridView::refreshNotes()
{
    seenModelVersion = noteModel.getVersion();
    notes = noteModel.getSnapshot();

    std::set<juce::int64> alive;
    for (const auto& n : notes)
        if (selection.count (n.id) != 0)
            alive.insert (n.id);
    const bool changed = alive != selection;
    selection = std::move (alive);
    rebuildTimeMap();
    repaint();
    if (changed)
        selectionChanged();
}

void NoteGridView::setSelection (std::set<juce::int64> ids)
{
    if (ids == selection)
        return;
    selection = std::move (ids);
    repaint();
    selectionChanged();
}

void NoteGridView::selectAll()
{
    std::set<juce::int64> all;
    for (const auto& n : notes)
        if (isSelectable (n) && (breathMode || n.isNote()))
            all.insert (n.id);
    setSelection (std::move (all));
}

void NoteGridView::selectionChanged()
{
    if (onSelectionChanged)
        onSelectionChanged();
}

void NoteGridView::commit (const juce::String& name, std::vector<Note> newNotes)
{
    if (history != nullptr)
        history->perform (name, std::move (newNotes));
    else
    {
        for (auto& n : newNotes)
            NoteModel::clampEdits (n);
        noteModel.replaceAll (std::move (newNotes));
    }
    refreshNotes();
}

// --- Coordinates ------------------------------------------------------------------------

juce::Rectangle<int> NoteGridView::gridArea() const noexcept
{
    return getLocalBounds().withTrimmedLeft (pitchRulerWidth).withTrimmedTop (timeRulerHeight);
}

float NoteGridView::pitchToY (float pitch) const noexcept
{
    return (float) timeRulerHeight + (view.topPitch - pitch) * view.pixelsPerSemitone;
}

float NoteGridView::yToPitch (float y) const noexcept
{
    return view.topPitch - (y - (float) timeRulerHeight) / view.pixelsPerSemitone;
}

float NoteGridView::secondsToX (double seconds) const noexcept
{
    return (float) pitchRulerWidth + (float) ((seconds - view.startSeconds) * view.pixelsPerSecond);
}

double NoteGridView::xToSeconds (float x) const noexcept
{
    return view.startSeconds + (double) (x - (float) pitchRulerWidth) / view.pixelsPerSecond;
}

juce::Rectangle<float> NoteGridView::noteBounds (const Note& n) const
{
    const double start = n.outStart(), end = n.outEnd();
    return { secondsToX (start), pitchToY (displayPitch (n) + 0.5f),
             std::max (2.0f, (float) ((end - start) * view.pixelsPerSecond)), view.pixelsPerSemitone };
}

void NoteGridView::constrainView()
{
    view.pixelsPerSecond   = juce::jlimit (minPixelsPerSecond, maxPixelsPerSecond, view.pixelsPerSecond);
    view.pixelsPerSemitone = juce::jlimit (minPixelsPerSemitone, maxPixelsPerSemitone, view.pixelsPerSemitone);
    view.startSeconds      = juce::jmax (0.0, std::isfinite (view.startSeconds) ? view.startSeconds : 0.0);

    const float visibleSemitones = (float) gridArea().getHeight() / view.pixelsPerSemitone;
    const float maxTop = highestPitch + 1.0f;
    const float minTop = juce::jmin (maxTop, lowestPitch + visibleSemitones);
    if (! std::isfinite (view.topPitch))
        view.topPitch = 84.0f;
    view.topPitch = juce::jlimit (minTop, maxTop, view.topPitch);
}

void NoteGridView::viewChanged()
{
    constrainView();
    repaint();
    if (onViewChanged)
        onViewChanged (view);
}

void NoteGridView::resized()
{
    constrainView();
}

double NoteGridView::timeGridStep() const noexcept
{
    static constexpr double steps[] = { 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0 };
    for (auto s : steps)
        if (s * view.pixelsPerSecond >= 70.0)
            return s;
    return 120.0;
}

juce::String NoteGridView::pitchLabel (float midi) const
{
    const int nearest = juce::roundToInt (midi);
    const int cents = juce::roundToInt ((midi - (float) nearest) * 100.0f);
    juce::String s = juce::String (pitchNames[((nearest % 12) + 12) % 12]) + juce::String (nearest / 12 - 1);
    if (cents != 0)
        s << (cents > 0 ? " +" : " ") << cents << " c";
    return s;
}

// --- Painting ----------------------------------------------------------------------------

void NoteGridView::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);

    const auto grid = gridArea();
    paintGrid (g, grid);
    paintWaveform (g, grid);
    paintNoise (g, grid);
    paintNotes (g, grid);

    if (playheadVisible)
    {
        const float x = secondsToX (playhead);
        if (x >= (float) grid.getX() && x <= (float) grid.getRight())
        {
            g.setColour (Colours::accent.withAlpha (0.85f));
            g.fillRect (juce::Rectangle<float> (x - 0.5f, (float) grid.getY(), 1.5f, (float) grid.getHeight()));
        }
    }

    if (drag == Drag::lasso)
    {
        g.setColour (Colours::accent.withAlpha (0.12f));
        g.fillRect (lassoRect);
        g.setColour (Colours::accent.withAlpha (0.7f));
        g.drawRect (lassoRect, 1.0f);
    }
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (grid);
        if (tool == Tool::edit)
            paintHandles (g);
        if (tool == Tool::split && splitHoverX >= 0.0f)
            for (const auto& n : notes)
                if (n.id == splitHoverNote)
                {
                    const auto b = noteBounds (n);
                    juce::Path line;
                    line.startNewSubPath (splitHoverX, b.getY() - 6.0f);
                    line.lineTo (splitHoverX, b.getBottom() + 6.0f);
                    juce::Path dashed;
                    const float dashes[] = { 3.0f, 2.0f };
                    juce::PathStrokeType (1.5f).createDashedStroke (dashed, line, dashes, 2);
                    g.setColour (Colours::accent);
                    g.fillPath (dashed);
                }
    }
    paintDragInfo (g);

    paintPitchRuler (g, getLocalBounds().withWidth (pitchRulerWidth).withTrimmedTop (timeRulerHeight));
    paintTimeRuler (g, getLocalBounds().withHeight (timeRulerHeight));
    if (locator >= 0.0)
    {
        const float x = secondsToX (locator);
        if (x >= (float) pitchRulerWidth)
        {
            juce::Path tri;
            tri.addTriangle (x - 5.0f, 2.0f, x + 5.0f, 2.0f, x, 10.0f);
            g.setColour (Colours::accent);
            g.fillPath (tri);
        }
    }
    g.setColour (Colours::panel);
    g.fillRect (0, 0, pitchRulerWidth, timeRulerHeight);

    if (notes.empty() && emptyMessage.isNotEmpty())
    {
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (15.0f));
        g.drawFittedText (emptyMessage, grid.reduced (40), juce::Justification::centred, 5);
    }
}

void NoteGridView::paintGrid (juce::Graphics& g, juce::Rectangle<int> area)
{
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area);

    const int top = (int) std::ceil (view.topPitch);
    const int bottom = (int) std::floor (yToPitch ((float) area.getBottom()));

    for (int p = top; p >= bottom - 1; --p)
    {
        const float y0 = pitchToY ((float) p + 0.5f), y1 = pitchToY ((float) p - 0.5f);
        g.setColour (laneColour (p));
        g.fillRect (juce::Rectangle<float> ((float) area.getX(), y0, (float) area.getWidth(), y1 - y0));
        g.setColour (p % 12 == 0 ? Colours::gridLineC : Colours::gridLine);
        g.fillRect (juce::Rectangle<float> ((float) area.getX(), y1 - 0.5f, (float) area.getWidth(), 1.0f));
    }

    const double tEnd = xToSeconds ((float) area.getRight());
    if (gridBpm > 0.0)
    {
        // Musical grid: bars, beats and the chosen division (only where lines are >= 5 px apart).
        const double beatPpq = 4.0 / gridDen, barPpq = beatPpq * gridNum;
        const double divPpq = gridDivision > 0 ? 4.0 / gridDivision * (gridTriplet ? 2.0 / 3.0 : 1.0) : beatPpq;
        const double pxPerPpq = view.pixelsPerSecond * 60.0 / gridBpm;
        double stepPpq = divPpq;
        while (stepPpq * pxPerPpq < 5.0) stepPpq *= 2.0;
        const double p0 = std::floor ((gridPpqAtZero + view.startSeconds * gridBpm / 60.0) / stepPpq) * stepPpq;
        const double p1 = gridPpqAtZero + tEnd * gridBpm / 60.0;
        auto isMultiple = [] (double v, double m) { const double r = v / m; return std::abs (r - std::round (r)) < 1.0e-6; };
        for (double q = p0; q <= p1; q += stepPpq)
        {
            const float x = secondsToX ((q - gridPpqAtZero) * 60.0 / gridBpm);
            const bool bar = isMultiple (q, barPpq), beat = isMultiple (q, beatPpq);
            g.setColour (bar ? Colours::gridLineC.brighter (0.25f) : beat ? Colours::gridLineC : Colours::gridLine);
            g.fillRect (juce::Rectangle<float> (x, (float) area.getY(), bar ? 1.5f : 1.0f, (float) area.getHeight()));
        }
        return;
    }
    const double step = timeGridStep();
    const double t0 = std::floor (view.startSeconds / step) * step;
    g.setColour (Colours::gridLine);
    for (double t = t0; t <= tEnd; t += step)
        g.fillRect (juce::Rectangle<float> (secondsToX (t), (float) area.getY(), 1.0f, (float) area.getHeight()));
}

void NoteGridView::paintNoise (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (analysis == nullptr || analysis->noise.empty())
        return;
    for (const auto& n : notes)
        if (! n.isNote())
            return;   // breaths / consonants are objects of their own (older projects: drawn from the analysis)

    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area);
    const auto visibleStart = xToSeconds ((float) area.getX());
    const auto visibleEnd   = xToSeconds ((float) area.getRight());

    for (const auto& nz : analysis->noise)
    {
        const double s0 = toOutput (nz.startSeconds), s1 = toOutput (nz.endSeconds);
        if (s1 < visibleStart || s0 > visibleEnd)
            continue;

        // Breaths / consonants sit at the height of the nearest note, so they read as part of the phrase.
        float pitch = notes.empty() ? 60.0f : displayPitch (notes.front());
        double bestGap = 1.0e9;
        for (const auto& n : notes)
        {
            const double gap = std::min (std::abs (n.endSeconds - nz.startSeconds), std::abs (n.startSeconds - nz.endSeconds));
            if (gap < bestGap) { bestGap = gap; pitch = displayPitch (n); }
        }

        const float x0 = secondsToX (s0), x1 = secondsToX (s1);
        const float y = pitchToY (pitch);
        const float h = view.pixelsPerSemitone * 0.5f;
        g.setColour (Colours::textDim.withAlpha (0.22f));
        g.fillRoundedRectangle (juce::Rectangle<float> (x0, y - h * 0.5f, std::max (1.0f, x1 - x0), h), 2.0f);
        g.setColour (Colours::textDim.withAlpha (0.35f));
        for (float x = x0 + 2.0f; x < x1 - 1.0f; x += 4.0f)
            g.fillRect (juce::Rectangle<float> (x, y - h * 0.35f, 1.0f, h * 0.7f));
    }
}

void NoteGridView::paintNoteShape (juce::Graphics& g, const Note& n, juce::Colour colour, bool selected)
{
    const auto box = noteBounds (n);
    const float centreY = pitchToY (displayPitch (n));

    // Ghost of the original position while a note is edited and selected.
    if (selected && (std::abs (displayPitch (n) - n.detectedPitch) > 0.05f || n.hasTimeEdit()))
    {
        const juce::Rectangle<float> orig (secondsToX (n.startSeconds), pitchToY (n.detectedPitch + 0.5f),
                                           std::max (2.0f, (float) (n.duration() * view.pixelsPerSecond)),
                                           view.pixelsPerSemitone);
        juce::Path ghost;
        ghost.addRoundedRectangle (orig.reduced (0.5f, 1.5f), 3.0f);
        juce::Path dashed;
        const float dashes[] = { 3.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, ghost, dashes, 2);
        g.setColour (Colours::textDim.withAlpha (0.6f));
        g.fillPath (dashed);
    }

    if (analysis == nullptr || analysis->frames.empty())
    {
        g.setColour (colour.withAlpha (0.85f));
        g.fillRoundedRectangle (box.reduced (0.0f, 1.0f), 3.0f);
        g.setColour (selected ? Colours::text : colour.brighter (0.4f));
        g.drawRoundedRectangle (box.reduced (0.5f, 1.5f), 3.0f, selected ? 1.5f : 1.0f);
        return;
    }

    const auto& frames = analysis->frames;
    const double hop = analysis->hopSeconds;
    const int f0 = std::max (0, analysis->frameIndexAt (n.startSeconds));
    const int f1 = std::min ((int) frames.size() - 1, analysis->frameIndexAt (n.endSeconds));
    if (f1 <= f0)
        return;

    // Source time -> output time inside this note (same mapping as the renderer).
    const double srcDur = std::max (1.0e-6, n.duration());
    const double outDur = n.outEnd() - n.outStart();
    auto xAt = [&] (int fi)
    {
        const double ts = juce::jlimit (n.startSeconds, n.endSeconds, fi * hop);
        return secondsToX (n.outStart() + (ts - n.startSeconds) * outDur / srcDur);
    };

    const int step = std::max (1, (int) std::floor (2.0 / (hop * view.pixelsPerSecond * std::max (0.1, outDur / srcDur))));
    const float floorDb = analysis->peakLevelDb - 48.0f;
    const float maxHalf = view.pixelsPerSemitone * 0.55f;
    const float minHalf = std::max (1.0f, view.pixelsPerSemitone * 0.08f);
    const float gainScale = std::pow (10.0f, n.gainDb / 40.0f);   // gain edits show as thicker/thinner
    auto halfAt = [&] (int fi)
    {
        const float level = juce::jlimit (0.0f, 1.0f, (frames[(size_t) fi].levelDb - floorDb) / 42.0f);
        return juce::jlimit (minHalf, maxHalf * 1.3f, (minHalf + (maxHalf - minHalf) * level) * gainScale);
    };

    juce::Path blob;
    blob.startNewSubPath (box.getX(), centreY);
    for (int fi = f0; fi <= f1; fi += step)
        blob.lineTo (xAt (fi), centreY - halfAt (fi));
    blob.lineTo (box.getRight(), centreY - halfAt (f1));
    blob.lineTo (box.getRight(), centreY);
    blob.lineTo (box.getRight(), centreY + halfAt (f1));
    for (int fi = f1; fi >= f0; fi -= step)
        blob.lineTo (xAt (fi), centreY + halfAt (fi));
    blob.closeSubPath();

    float confidenceSum = 0.0f;
    for (int fi = f0; fi <= f1; ++fi)
        confidenceSum += frames[(size_t) fi].confidence;
    const bool reliable = confidenceSum / (float) (f1 - f0 + 1) >= 0.7f;

    g.setColour (colour.withAlpha (selected ? 0.75f : 0.55f));
    g.fillPath (blob);
    g.setColour (selected ? Colours::text : colour.brighter (0.3f));
    if (reliable)
        g.strokePath (blob, juce::PathStrokeType (selected ? 1.5f : 1.0f));
    else
    {
        juce::Path dashed;
        const float dashes[] = { 4.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, blob, dashes, 2);
        g.fillPath (dashed);
    }

    // Small marks for edits that do not show in the shape.
    if (n.eq.isActive() || std::abs (n.formantCents) > 0.5f)
    {
        g.setColour (Colours::accent);
        float mx = box.getX() + 3.0f;
        if (n.eq.isActive())                 { g.fillEllipse (mx, box.getY() + 2.0f, 4.0f, 4.0f); mx += 6.0f; }
        if (std::abs (n.formantCents) > 0.5f) { g.drawEllipse (mx, box.getY() + 2.0f, 4.0f, 4.0f, 1.0f); }
    }

    // Pitch curve as it will sound (vibrato, drift and corrections included).
    const auto curve = VocalRenderer::notePitchCurve (n, *analysis, options);
    const float octaveFix = 12.0f * std::round ((n.detectedPitch - VocalAnalyzer::centrePitch (*analysis, n.startSeconds, n.endSeconds, n.detectedPitch)) / 12.0f);
    juce::Path path;
    bool drawing = false;
    for (size_t i = 0; i < curve.size(); ++i)
    {
        const int fi = f0 + (int) i;
        if (fi > f1 || ! std::isfinite (curve[i]))
        {
            drawing = false;
            continue;
        }
        const juce::Point<float> pt (xAt (fi), pitchToY (curve[i] + octaveFix));
        if (drawing) path.lineTo (pt);
        else { path.startNewSubPath (pt); drawing = true; }
    }
    g.setColour (Colours::text.withAlpha (0.9f));
    g.strokePath (path, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void NoteGridView::paintNotes (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (notes.empty())
        return;

    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area);
    // Only notes inside the area being repainted (the playhead repaints thin strips 30x/s).
    const auto clip = g.getClipBounds().expanded (2, 0);
    const auto visibleStart = xToSeconds ((float) clip.getX());
    const auto visibleEnd   = xToSeconds ((float) clip.getRight());

    for (const auto& n : notes)
    {
        if (std::max (n.outEnd(), n.endSeconds) < visibleStart || std::min (n.outStart(), n.startSeconds) > visibleEnd)
            continue;
        const bool selected = selection.count (n.id) != 0;
        if (! n.isNote())
        {
            paintNoiseObject (g, n, selected);
            continue;
        }
        const float pitch = displayPitch (n);
        const bool outOfScale = scaleHighlightActive() && ! keyScale.containsMidi (juce::roundToInt (pitch));
        auto colour = selected ? Colours::noteSelected : (outOfScale ? Colours::noteOutOfScale : Colours::note);
        if (breathMode) colour = colour.withMultipliedAlpha (0.35f);
        paintNoteShape (g, n, colour, selected);
    }

    // One continuous pitch line: joints between neighbouring notes as the glide that is heard
    // (raised cosine over the transition time).
    if (analysis == nullptr || analysis->frames.empty())
        return;
    auto endPitch = [this] (const Note& n, bool last)
    {
        const auto c = VocalRenderer::notePitchCurve (n, *analysis, options);
        if (last) { for (auto it = c.rbegin(); it != c.rend(); ++it) if (std::isfinite (*it)) return *it; }
        else      { for (float v : c) if (std::isfinite (v)) return v; }
        return displayPitch (n);
    };
    const Note* prev = nullptr;
    for (const auto& n : notes)
    {
        if (! n.isNote()) continue;
        if (prev != nullptr && n.outStart() - prev->outEnd() <= 0.6 && n.outStart() >= prev->outEnd() - 1.0e-6
            && std::max (prev->outEnd(), n.outStart()) >= visibleStart && std::min (prev->outEnd(), n.outStart()) <= visibleEnd)
        {
            const double T = (n.transitionMs >= 0.0f ? n.transitionMs : 45.0f) / 1000.0;
            const double c = 0.5 * (prev->outEnd() + n.outStart());
            const double h = std::max (0.5 * T, 0.5 * (n.outStart() - prev->outEnd()));
            const double t0 = std::max (prev->outStart(), c - h), t1 = std::min (n.outEnd(), c + h);
            const float p0 = endPitch (*prev, true), p1 = endPitch (n, false);
            juce::Path joint;
            const int steps = 16;
            for (int i = 0; i <= steps; ++i)
            {
                const double x = (double) i / steps;
                const double t = t0 + (t1 - t0) * x;
                const float p = p0 + (p1 - p0) * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * x));
                const juce::Point<float> pt (secondsToX (t), pitchToY (p));
                if (i == 0) joint.startNewSubPath (pt); else joint.lineTo (pt);
            }
            g.setColour (Colours::text.withAlpha (breathMode ? 0.3f : 0.7f));
            g.strokePath (joint, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        prev = &n;
    }
}

// Breath (teal), sibilant "s/ts" (violet), plosive "k/t/p" (orange): a hatched block whose height
// follows the object's gain; a silenced one is only an outline.
void NoteGridView::paintNoiseObject (juce::Graphics& g, const Note& n, bool selected)
{
    const juce::Colour base = n.kind == Note::breath ? juce::Colour (0xff5fb8b0)
                            : n.kind == Note::sibilant ? juce::Colour (0xffa58ad8) : juce::Colour (0xffd9935a);
    const auto colour = selected ? Colours::noteSelected : base;
    const bool muted = n.gainDb <= Note::silentDb + 0.01f;
    const float x0 = secondsToX (n.outStart()), x1 = secondsToX (n.outEnd());
    const float y = pitchToY (n.detectedPitch);
    const float g01 = juce::jlimit (0.15f, 1.6f, std::pow (10.0f, n.gainDb / 30.0f));
    const float h = view.pixelsPerSemitone * 0.7f * (muted ? 1.0f : g01);
    const juce::Rectangle<float> r (x0, y - h * 0.5f, std::max (2.0f, x1 - x0), h);
    const float alphaScale = breathMode ? 1.0f : 0.55f;
    if (! muted)
    {
        g.setColour (colour.withAlpha (0.28f * alphaScale + (selected ? 0.2f : 0.0f)));
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (colour.withAlpha (0.6f * alphaScale + 0.2f));
        for (float x = r.getX() + 2.0f; x < r.getRight() - 1.0f; x += 4.0f)
            g.fillRect (juce::Rectangle<float> (x, r.getY() + 2.0f, 1.0f, std::max (1.0f, r.getHeight() - 4.0f)));
    }
    g.setColour (colour.withAlpha (selected ? 1.0f : 0.5f * alphaScale + 0.2f));
    g.drawRoundedRectangle (r, 2.0f, selected ? 1.5f : 1.0f);
    if (r.getWidth() >= 12.0f && view.pixelsPerSemitone >= 10.0f)
    {
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.setColour (colour.brighter (0.4f).withAlpha (muted ? 0.6f : 0.95f));
        const char* label = n.kind == Note::breath ? "br" : n.kind == Note::sibilant ? "s" : "k";
        g.drawText (muted ? juce::String (label) + " x" : juce::String (label),
                    juce::Rectangle<float> (r.getX(), r.getY() - 12.0f, std::max (24.0f, r.getWidth()), 11.0f),
                    juce::Justification::centredLeft, false);
    }
}

bool NoteGridView::handleRects (const Note& n, juce::Rectangle<float>& mod, juce::Rectangle<float>& drift) const
{
    const auto box = noteBounds (n);
    if (box.getWidth() < 36.0f)
        return false;
    const float r = 5.0f;
    mod = { box.getCentreX() - r, box.getY() - r - 3.0f, 2 * r, 2 * r };
    drift = { box.getCentreX() - r, box.getBottom() + 3.0f - r, 2 * r, 2 * r };
    return true;
}

juce::Rectangle<float> NoteGridView::tiltHandleRect (const Note& n) const
{
    const auto box = noteBounds (n);
    return { box.getRight() - 12.0f, box.getY() - 8.0f, 10.0f, 10.0f };
}

bool NoteGridView::transitionHandleRect (const Note& n, juce::Rectangle<float>& r) const
{
    const Note* prev = nullptr;
    for (const auto& o : notes)
        if (o.isNote() && o.id != n.id && o.outEnd() <= n.outStart() + 1.0e-6 && n.outStart() - o.outEnd() <= 0.6)
            if (prev == nullptr || o.outEnd() > prev->outEnd()) prev = &o;
    if (prev == nullptr)
        return false;
    const float x = secondsToX (0.5 * (prev->outEnd() + n.outStart()));
    const float y = 0.5f * (pitchToY (displayPitch (*prev)) + pitchToY (displayPitch (n)));
    r = { x - 5.0f, y - 5.0f, 10.0f, 10.0f };
    return true;
}

// Vibrato (top), drift (bottom), tilt (top right) and transition (joint with the previous note)
// handles on a single selected note, like VariAudio / Melodyne.
void NoteGridView::paintHandles (juce::Graphics& g)
{
    if (selection.size() != 1 || breathMode)
        return;
    for (const auto& n : notes)
    {
        if (selection.count (n.id) == 0 || ! n.isNote()) continue;
        juce::Rectangle<float> tr;
        if (transitionHandleRect (n, tr))
        {
            juce::Path diamond;
            diamond.addTriangle (tr.getCentreX(), tr.getY(), tr.getRight(), tr.getCentreY(), tr.getCentreX(), tr.getBottom());
            diamond.addTriangle (tr.getCentreX(), tr.getY(), tr.getX(), tr.getCentreY(), tr.getCentreX(), tr.getBottom());
            g.setColour (Colours::panel);
            g.fillPath (diamond);
            g.setColour (n.transitionMs >= 0.0f ? Colours::accent : Colours::text);
            g.strokePath (diamond, juce::PathStrokeType (1.3f));
        }
        juce::Rectangle<float> m, d;
        if (! handleRects (n, m, d)) return;
        {
            const auto t = tiltHandleRect (n);
            g.setColour (Colours::panel);
            g.fillRect (t);
            g.setColour (std::abs (n.pitchTilt) > 0.5f ? Colours::accent : Colours::text);
            g.drawRect (t, 1.3f);
            g.drawLine (t.getX() + 2.0f, t.getBottom() - 2.0f, t.getRight() - 2.0f, t.getY() + 2.0f, 1.2f);
        }
        for (auto [rect, value, label] : { std::tuple { m, n.pitchModulation, "~" }, std::tuple { d, n.pitchDrift, "/" } })
        {
            const bool changed = std::abs (value - 1.0f) > 0.005f;
            g.setColour (Colours::panel);
            g.fillEllipse (rect);
            g.setColour (changed ? Colours::accent : Colours::text);
            g.drawEllipse (rect, 1.5f);
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText (label, rect, juce::Justification::centred, false);
        }
        return;
    }
}

void NoteGridView::paintDragInfo (juce::Graphics& g)
{
    if (drag == Drag::gain)
    {
        for (const auto& x : notes)
            if (x.id == dragNoteId)
            {
                const auto box = noteBounds (x);
                const auto text = "Gain " + juce::String (x.gainDb >= 0.0f ? "+" : "") + juce::String (x.gainDb, 1) + " dB";
                juce::Rectangle<float> label (box.getX(), box.getY() - 22.0f, 100.0f, 18.0f);
                g.setColour (Colours::panel.withAlpha (0.92f));
                g.fillRoundedRectangle (label, 3.0f);
                g.setColour (Colours::accent);
                g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
                g.drawText (text, label.reduced (5.0f, 0.0f), juce::Justification::centredLeft);
            }
        return;
    }
    if (drag == Drag::modHandle || drag == Drag::driftHandle || drag == Drag::tiltHandle || drag == Drag::transitionHandle)
    {
        for (const auto& x : notes)
            if (x.id == dragNoteId)
            {
                const auto box = noteBounds (x);
                const auto text = drag == Drag::tiltHandle ? "Tilt " + juce::String (juce::roundToInt (x.pitchTilt)) + " c"
                                : drag == Drag::transitionHandle ? "Transition " + (x.transitionMs < 0.0f ? juce::String ("natural")
                                                                                                       : juce::String (juce::roundToInt (x.transitionMs)) + " ms")
                                : juce::String (drag == Drag::modHandle ? "Vibrato " : "Drift ")
                                    + juce::String (juce::roundToInt ((drag == Drag::modHandle ? x.pitchModulation : x.pitchDrift) * 100.0f)) + " %";
                juce::Rectangle<float> label (box.getX(), box.getY() - 34.0f, 110.0f, 18.0f);
                g.setColour (Colours::panel.withAlpha (0.92f));
                g.fillRoundedRectangle (label, 3.0f);
                g.setColour (Colours::accent);
                g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
                g.drawText (text, label.reduced (5.0f, 0.0f), juce::Justification::centredLeft);
            }
        return;
    }
    if (drag != Drag::pitch && drag != Drag::time && drag != Drag::startEdge && drag != Drag::endEdge)
        return;
    const Note* n = nullptr;
    for (const auto& x : notes)
        if (x.id == dragNoteId) n = &x;
    if (n == nullptr)
        return;

    juce::String text;
    if (drag == Drag::pitch)
        text = pitchLabel (displayPitch (*n));
    else
    {
        const double ms = (drag == Drag::time ? n->timeShiftSeconds
                         : drag == Drag::startEdge ? n->startEditSeconds : n->endEditSeconds) * 1000.0;
        text = (ms >= 0.0 ? "+" : "") + juce::String (ms, 0) + " ms";
    }

    const auto box = noteBounds (*n);
    juce::Rectangle<float> label (box.getX(), box.getY() - 22.0f, 90.0f, 18.0f);
    g.setColour (Colours::panel.withAlpha (0.92f));
    g.fillRoundedRectangle (label, 3.0f);
    g.setColour (Colours::accent);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText (text, label.reduced (5.0f, 0.0f), juce::Justification::centredLeft);
}

void NoteGridView::paintPitchRuler (juce::Graphics& g, juce::Rectangle<int> area)
{
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area);
    g.setColour (Colours::panel);
    g.fillRect (area);

    const int top = (int) std::ceil (view.topPitch);
    const int bottom = (int) std::floor (yToPitch ((float) area.getBottom()));
    const bool labelEveryNote = view.pixelsPerSemitone >= 13.0f;

    g.setFont (juce::FontOptions (11.0f));
    for (int p = top; p >= bottom - 1; --p)
    {
        const float y0 = pitchToY ((float) p + 0.5f);
        const auto lane = juce::Rectangle<float> ((float) area.getX(), y0, (float) area.getWidth(), view.pixelsPerSemitone);

        if (isBlackKey (p))
        {
            g.setColour (Colours::background);
            g.fillRect (lane.withWidth (area.getWidth() * 0.6f));
        }

        const bool withScale = scaleHighlightActive();
        const bool inScale = keyScale.containsMidi (p);
        const bool isRoot = withScale && keyScale.isRoot (p);
        if (withScale && inScale)
        {
            g.setColour (Colours::accent.withAlpha ((isRoot ? 0.9f : 0.45f) * juce::jmax (0.35f, highlightAmount)));
            g.fillRect (lane.withX ((float) area.getRight() - 4.0f).withWidth (3.0f).reduced (0.0f, 1.0f));
        }

        const bool isC = p % 12 == 0;
        const bool wantLabel = withScale ? (isC || isRoot || (labelEveryNote && inScale))
                                         : (isC || (labelEveryNote && ! isBlackKey (p)));
        if (wantLabel)
        {
            g.setColour (isRoot ? Colours::accent : (isC ? Colours::text : Colours::textDim));
            const auto label = juce::String (pitchNames[((p % 12) + 12) % 12]) + juce::String (p / 12 - 1);
            g.drawText (label, lane.reduced (6.0f, 0.0f), juce::Justification::centredRight, false);
        }
    }

    g.setColour (Colours::gridLineC);
    g.fillRect (area.getRight() - 1, area.getY(), 1, area.getHeight());
}

void NoteGridView::paintTimeRuler (juce::Graphics& g, juce::Rectangle<int> area)
{
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area);
    g.setColour (Colours::panel);
    g.fillRect (area);

    const double tEnd = xToSeconds ((float) area.getRight());
    g.setFont (juce::FontOptions (11.0f));

    if (gridBpm > 0.0)
    {
        const double barPpq = 4.0 / gridDen * gridNum;
        const double pxPerBar = barPpq * view.pixelsPerSecond * 60.0 / gridBpm;
        int every = 1;
        while (pxPerBar * every < 44.0) every *= 2;
        const int b0 = (int) std::floor ((gridPpqAtZero + view.startSeconds * gridBpm / 60.0) / barPpq);
        const int b1 = (int) std::ceil ((gridPpqAtZero + tEnd * gridBpm / 60.0) / barPpq);
        for (int b = b0 - (b0 % every); b <= b1; b += every)
        {
            const float x = secondsToX ((b * barPpq - gridPpqAtZero) * 60.0 / gridBpm);
            if (x < (float) pitchRulerWidth) continue;
            g.setColour (Colours::gridLineC);
            g.fillRect (juce::Rectangle<float> (x, (float) area.getBottom() - 8.0f, 1.0f, 8.0f));
            g.setColour (Colours::textDim);
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (x + 3.0f, (float) area.getY(), 50.0f, (float) area.getHeight() - 4.0f),
                        juce::Justification::centredLeft, false);
        }
        g.setColour (Colours::gridLineC);
        g.fillRect (area.getX(), area.getBottom() - 1, area.getWidth(), 1);
        return;
    }

    const double step = timeGridStep();
    const double t0 = std::floor (view.startSeconds / step) * step;
    for (double t = t0; t <= tEnd; t += step)
    {
        const float x = secondsToX (t);
        if (x < (float) pitchRulerWidth)
            continue;
        g.setColour (Colours::gridLineC);
        g.fillRect (juce::Rectangle<float> (x, (float) area.getBottom() - 6.0f, 1.0f, 6.0f));
        g.setColour (Colours::textDim);
        g.drawText (step < 1.0 ? juce::String (t, 2) + " s" : formatTime (t),
                    juce::Rectangle<float> (x + 3.0f, (float) area.getY(), 70.0f, (float) area.getHeight() - 4.0f),
                    juce::Justification::centredLeft, false);
    }

    g.setColour (Colours::gridLineC);
    g.fillRect (area.getX(), area.getBottom() - 1, area.getWidth(), 1);
}

// --- Mouse -----------------------------------------------------------------------------

void NoteGridView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const float delta = (std::abs (wheel.deltaY) >= std::abs (wheel.deltaX)) ? wheel.deltaY : -wheel.deltaX;
    if (juce::exactlyEqual (delta, 0.0f))
        return;

    const auto mods = e.mods;
    const auto pos = e.position;

    if (mods.isCommandDown() || mods.isCtrlDown())
    {
        const double anchor = xToSeconds (pos.x);
        view.pixelsPerSecond = juce::jlimit (minPixelsPerSecond, maxPixelsPerSecond,
                                             view.pixelsPerSecond * std::pow (1.0025, delta * 400.0));
        view.startSeconds = anchor - (double) (pos.x - (float) pitchRulerWidth) / view.pixelsPerSecond;
    }
    else if (mods.isAltDown())
    {
        const float anchor = yToPitch (pos.y);
        view.pixelsPerSemitone = juce::jlimit (minPixelsPerSemitone, maxPixelsPerSemitone,
                                               view.pixelsPerSemitone * (float) std::pow (1.0025, delta * 400.0));
        view.topPitch = anchor + (pos.y - (float) timeRulerHeight) / view.pixelsPerSemitone;
    }
    else if (mods.isShiftDown() || std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
        view.startSeconds -= (double) delta * 200.0 / view.pixelsPerSecond;
    else
        view.topPitch += delta * 6.0f;

    viewChanged();
    if (onUserNavigated)
        onUserNavigated();
}

int NoteGridView::noteIndexAt (juce::Point<float> position) const
{
    // Notes first (breaths / consonants sit under them), unless in BREATH mode.
    for (int pass = 0; pass < 2; ++pass)
        for (int i = (int) notes.size() - 1; i >= 0; --i)
        {
            const auto& n = notes[(size_t) i];
            if (! isSelectable (n) || (pass == 0) != n.isNote()) continue;
            auto box = n.isNote() ? noteBounds (n)
                                  : juce::Rectangle<float> (secondsToX (n.outStart()), pitchToY (n.detectedPitch) - view.pixelsPerSemitone * 0.5f,
                                                            std::max (3.0f, secondsToX (n.outEnd()) - secondsToX (n.outStart())), view.pixelsPerSemitone);
            if (box.expanded (0.0f, 1.0f).contains (position))
                return i;
        }
    return -1;
}

NoteGridView::Hit NoteGridView::hitTestNote (juce::Point<float> p, int& index) const
{
    if (tool != Tool::edit)
    {
        index = noteIndexAt (p);
        return index < 0 ? Hit::none : Hit::body;
    }
    if (selection.size() == 1 && ! breathMode)
        for (int i = 0; i < (int) notes.size(); ++i)
        {
            const auto& sn = notes[(size_t) i];
            if (selection.count (sn.id) == 0 || ! sn.isNote()) continue;
            juce::Rectangle<float> m, d, tr;
            if (handleRects (sn, m, d))
            {
                if (m.expanded (2.0f).contains (p)) { index = i; return Hit::modHandle; }
                if (d.expanded (2.0f).contains (p)) { index = i; return Hit::driftHandle; }
                if (tiltHandleRect (sn).expanded (2.0f).contains (p)) { index = i; return Hit::tiltHandle; }
            }
            if (transitionHandleRect (sn, tr) && tr.expanded (2.0f).contains (p)) { index = i; return Hit::transitionHandle; }
        }
    index = noteIndexAt (p);
    if (index >= 0 && ! notes[(size_t) index].isNote())
        return Hit::body;
    if (index < 0)
        return Hit::none;
    const auto box = noteBounds (notes[(size_t) index]);
    if (box.getWidth() >= 3.0f * edgeGrabPixels)
    {
        if (p.x - box.getX() <= edgeGrabPixels) return Hit::startEdge;
        if (box.getRight() - p.x <= edgeGrabPixels) return Hit::endEdge;
    }
    return Hit::body;
}

void NoteGridView::mouseMove (const juce::MouseEvent& e)
{
    syncWithModel();
    if (tool != Tool::edit)
    {
        const int idx = noteIndexAt (e.position);
        const float oldX = splitHoverX;
        if (tool == Tool::split && idx >= 0) { splitHoverX = e.position.x; splitHoverNote = notes[(size_t) idx].id; }
        else splitHoverX = -1.0f;
        if (! juce::exactlyEqual (oldX, splitHoverX)) repaint();
        setMouseCursor (idx < 0 ? juce::MouseCursor::NormalCursor
                        : tool == Tool::split ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::UpDownResizeCursor);
        return;
    }
    int index = -1;
    switch (hitTestNote (e.position, index))
    {
        case Hit::startEdge:
        case Hit::endEdge: setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
        case Hit::body:    setMouseCursor (juce::MouseCursor::PointingHandCursor); break;
        case Hit::modHandle:
        case Hit::tiltHandle:
        case Hit::driftHandle: setMouseCursor (juce::MouseCursor::UpDownResizeCursor); break;
        case Hit::transitionHandle: setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
        case Hit::none:
        default:           setMouseCursor (juce::MouseCursor::NormalCursor); break;
    }
}

void NoteGridView::mouseDown (const juce::MouseEvent& e)
{
    syncWithModel();   // the model may have changed (undo, analysis) since the last repaint
    grabKeyboardFocus();
    drag = Drag::none;
    dragStart = e.position;

    if (e.mods.isMiddleButtonDown() || (e.mods.isLeftButtonDown() && e.mods.isAltDown()))
    {
        drag = Drag::pan;
        panStartView = view;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        return;
    }

    // Pitch ruler: right-click adds / removes that note from the scale.
    if (e.position.x < (float) pitchRulerWidth && e.position.y > (float) timeRulerHeight)
    {
        if (e.mods.isPopupMenu())
            showScaleMenu (((juce::roundToInt (yToPitch (e.position.y)) % 12) + 12) % 12);
        return;
    }
    // Time ruler: click = move the host playhead there (ARA hosts).
    if (e.position.y < (float) timeRulerHeight && e.position.x >= (float) pitchRulerWidth)
    {
        if (e.mods.isPopupMenu())
            return;
        const double t = std::max (0.0, xToSeconds (e.position.x));
        if (e.getNumberOfClicks() >= 2) { if (onPlayFrom) onPlayFrom (t); }
        else if (onLocate) onLocate (t);
        return;
    }

    int index = -1;
    const auto hit = hitTestNote (e.position, index);

    if (hit == Hit::modHandle || hit == Hit::driftHandle || hit == Hit::tiltHandle || hit == Hit::transitionHandle)
    {
        dragNoteId = notes[(size_t) index].id;
        dragOrigin = noteModel.getSnapshot();
        drag = hit == Hit::modHandle ? Drag::modHandle : hit == Hit::driftHandle ? Drag::driftHandle
             : hit == Hit::tiltHandle ? Drag::tiltHandle : Drag::transitionHandle;
        if (history != nullptr)
            history->beginGesture();
        return;
    }

    if (! e.mods.isPopupMenu() && index >= 0 && tool == Tool::split)
    {
        const auto& n = notes[(size_t) index];
        const double outT = xToSeconds (e.position.x);
        const double frac = (outT - n.outStart()) / std::max (1.0e-6, n.outEnd() - n.outStart());
        splitNoteAt (n.id, n.startSeconds + frac * n.duration());
        splitHoverX = -1.0f;
        repaint();
        return;
    }
    if (! e.mods.isPopupMenu() && index >= 0 && tool == Tool::gain)
    {
        const auto id = notes[(size_t) index].id;
        if (selection.count (id) == 0 && ! e.mods.isShiftDown())
            setSelection ({ id });
        else if (e.mods.isShiftDown())
        {
            auto sel = selection; sel.insert (id); setSelection (sel);
        }
        dragNoteId = id;
        dragOrigin = noteModel.getSnapshot();
        drag = Drag::gain;
        if (history != nullptr)
            history->beginGesture();
        if (onAuditionNote)
            if (const auto* n = findNote (notes, id))
                onAuditionNote (n->outStart(), n->outEnd());
        return;
    }

    if (e.mods.isPopupMenu())
    {
        if (index >= 0)
        {
            const auto& n = notes[(size_t) index];
            if (selection.count (n.id) == 0)
                setSelection ({ n.id });
            // Click position in source time (for splitting).
            const double outT = xToSeconds (e.position.x);
            const double frac = (outT - n.outStart()) / std::max (1.0e-6, n.outEnd() - n.outStart());
            showNoteMenu (n.id, n.startSeconds + frac * n.duration());
        }
        return;
    }

    if (index < 0)
    {
        drag = Drag::lasso;
        lassoBase = e.mods.isShiftDown() ? selection : std::set<juce::int64>();
        lassoRect = { e.position, e.position };
        if (! e.mods.isShiftDown())
            setSelection ({});
        return;
    }

    const auto id = notes[(size_t) index].id;
    if (e.mods.isShiftDown())
    {
        auto s = selection;
        if (s.count (id) != 0) s.erase (id); else s.insert (id);
        setSelection (s);
        return;   // Shift-click only toggles selection
    }
    if (selection.count (id) == 0)
        setSelection ({ id });

    dragNoteId = id;
    dragOrigin = noteModel.getSnapshot();
    drag = hit == Hit::startEdge ? Drag::startEdge : hit == Hit::endEdge ? Drag::endEdge : Drag::pending;
    if (drag != Drag::pending && history != nullptr)
        history->beginGesture();
    if (onAuditionNote)
        if (const auto* n = findNote (notes, id))
            onAuditionNote (n->outStart(), n->outEnd());
}

void NoteGridView::auditionDragged()
{
    if (onAuditionMove)
        if (const auto* n = findNote (notes, dragNoteId))
            onAuditionMove (n->outStart(), n->outEnd());
}

void NoteGridView::showScaleMenu (int pc)
{
    juce::PopupMenu menu;
    const bool in = keyScale.containsPitchClass (pc);
    const auto name = KeyScale::pitchClassName (pc);
    menu.addSectionHeader ("Scale: " + keyScale.getName());
    menu.addItem (1, in ? "Remove " + name + " from the scale" : "Allow " + name + " in the scale",
                  ! (in && keyScale.isRoot (pc)) && onTogglePitchClass != nullptr);
    menu.addItem (2, "Reset scale edits", keyScale.isCustomised() && onResetScaleEdits != nullptr);
    juce::Component::SafePointer<NoteGridView> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                        [safe, pc] (int r)
                        {
                            if (safe == nullptr) return;
                            if (r == 1 && safe->onTogglePitchClass) safe->onTogglePitchClass (pc);
                            if (r == 2 && safe->onResetScaleEdits) safe->onResetScaleEdits();
                        });
}

float NoteGridView::draggedShift (const Note& original, float deltaSemitones, bool freeMode) const
{
    const float base = original.detectedPitch + original.pitchShiftCents / 100.0f;
    float target = base + deltaSemitones;
    if (! freeMode)
    {
        const int direction = deltaSemitones > 0.0f ? 1 : (deltaSemitones < 0.0f ? -1 : 0);
        if (snapEnabled && ! keyScale.isChromatic())
            target = keyScale.snap (target, snapStrength, direction).pitch;
        else
            target = std::round (target);   // semitone grid
    }
    return (target - original.detectedPitch) * 100.0f;
}

void NoteGridView::applyDrag (const juce::MouseEvent& e)
{
    auto edited = dragOrigin;
    const auto d = e.position - dragStart;
    dragFree = e.mods.isCommandDown() || e.mods.isCtrlDown();

    // Neighbour limits for time edits: a note may not pass an unselected neighbour.
    // Objects that move in time: the selection plus consonants attached to the start of its notes.
    std::set<juce::int64> moving = selection;
    for (auto id : attachedConsonants (selection)) moving.insert (id);

    auto limitsFor = [&] (const Note& n, double& lo, double& hi)
    {
        lo = -1.0e9; hi = 1.0e9;
        for (const auto& o : dragOrigin)
        {
            if (moving.count (o.id) != 0 || o.id == n.id) continue;
            if (o.startSeconds < n.startSeconds) lo = std::max (lo, o.outStart() + 0.03);
            else hi = std::min (hi, o.outEnd() - 0.03);
        }
    };

    if (drag == Drag::gain)
    {
        const float delta = -d.y / (dragFree ? 12.0f : 4.0f);   // 4 px per dB (Ctrl: fine)
        for (auto& n : edited)
            if (selection.count (n.id) != 0)
                if (const Note* o = findNote (dragOrigin, n.id))
                {
                    n.gainDb = juce::jlimit (-24.0f, 12.0f, o->gainDb + delta);
                    if (! dragFree) n.gainDb = std::round (n.gainDb * 2.0f) * 0.5f;   // 0.5 dB steps
                }
    }
    else if (drag == Drag::tiltHandle || drag == Drag::transitionHandle)
    {
        for (auto& n : edited)
            if (n.id == dragNoteId)
                if (const Note* o = findNote (dragOrigin, n.id))
                {
                    if (drag == Drag::tiltHandle)
                    {
                        n.pitchTilt = juce::jlimit (-1200.0f, 1200.0f, o->pitchTilt - d.y * (dragFree ? 1.0f : 5.0f));
                        if (! dragFree && std::abs (n.pitchTilt) < 8.0f) n.pitchTilt = 0.0f;
                    }
                    else
                    {
                        const float base = o->transitionMs >= 0.0f ? o->transitionMs : 45.0f;
                        n.transitionMs = juce::jlimit (0.0f, 500.0f, base + (float) (2.0 * d.x / view.pixelsPerSecond * 1000.0));
                    }
                }
    }
    else if (drag == Drag::modHandle || drag == Drag::driftHandle)
    {
        const float delta = -d.y / 80.0f;   // 80 px = 100 %
        for (auto& n : edited)
            if (n.id == dragNoteId)
            {
                const Note* o = findNote (dragOrigin, n.id);
                float& value = drag == Drag::modHandle ? n.pitchModulation : n.pitchDrift;
                const float base = drag == Drag::modHandle ? o->pitchModulation : o->pitchDrift;
                value = juce::jlimit (0.0f, 2.0f, base + delta);
                if (! dragFree && std::abs (value - 1.0f) < 0.04f) value = 1.0f;   // detent at 100 %
            }
    }
    else if (drag == Drag::pitch)
    {
        const float delta = -d.y / view.pixelsPerSemitone;
        for (auto& n : edited)
            if (selection.count (n.id) != 0 && n.isNote())
            {
                const Note* o = nullptr;
                for (const auto& x : dragOrigin) if (x.id == n.id) o = &x;
                n.pitchShiftCents = draggedShift (*o, delta, dragFree);
                n.correctionCents = 0.0f;   // a manual pitch overrides the automatic correction
            }
    }
    else if (drag == Drag::time)
    {
        double delta = d.x / view.pixelsPerSecond;
        if (! dragFree && hasMusicalGrid())
            if (const Note* lead = findNote (dragOrigin, dragNoteId))
                delta = snapTime (lead->outStart() + delta) - lead->outStart();
        for (const auto& n : dragOrigin)
            if (moving.count (n.id) != 0)
            {
                double lo, hi;
                limitsFor (n, lo, hi);
                delta = juce::jlimit (lo - n.outStart(), hi - n.outEnd(), delta);
            }
        for (auto& n : edited)
            if (moving.count (n.id) != 0)
                n.timeShiftSeconds += delta;
    }
    else if (drag == Drag::startEdge || drag == Drag::endEdge)
    {
        const double delta = d.x / view.pixelsPerSecond;
        for (auto& n : edited)
        {
            if (selection.count (n.id) == 0) continue;
            double lo, hi;
            limitsFor (n, lo, hi);
            if (drag == Drag::startEdge)
            {
                double target = n.outStart() + delta;
                if (! dragFree) target = snapTime (target);
                const double newStart = juce::jlimit (lo, n.outEnd() - 0.03, target);
                n.startEditSeconds += newStart - n.outStart();
            }
            else
            {
                double target = n.outEnd() + delta;
                if (! dragFree) target = snapTime (target);
                const double newEnd = juce::jlimit (n.outStart() + 0.03, hi, target);
                n.endEditSeconds += newEnd - n.outEnd();
            }
        }
    }

    if (history != nullptr)
        history->updateGesture (std::move (edited));
    else
        noteModel.replaceAll (std::move (edited));
    refreshNotes();
    auditionDragged();
}

void NoteGridView::mouseDrag (const juce::MouseEvent& e)
{
    switch (drag)
    {
        case Drag::pan:
        {
            const auto d = e.position - dragStart;
            view.startSeconds = panStartView.startSeconds - (double) d.x / view.pixelsPerSecond;
            view.topPitch = panStartView.topPitch + d.y / view.pixelsPerSemitone;
            viewChanged();
            if (onUserNavigated)
                onUserNavigated();
            return;
        }
        case Drag::lasso:
        {
            lassoRect = juce::Rectangle<float> (dragStart, e.position);
            auto s = lassoBase;
            for (const auto& n : notes)
                if (isSelectable (n) && (breathMode || n.isNote()) && noteBounds (n).intersects (lassoRect))
                    s.insert (n.id);
            setSelection (s);
            repaint();
            return;
        }
        case Drag::pending:
        {
            const auto d = e.position - dragStart;
            if (std::abs (d.x) < 4.0f && std::abs (d.y) < 4.0f)
                return;
            drag = std::abs (d.y) >= std::abs (d.x) ? Drag::pitch : Drag::time;   // direction lock
            setMouseCursor (drag == Drag::pitch ? juce::MouseCursor::UpDownResizeCursor
                                                : juce::MouseCursor::LeftRightResizeCursor);
            if (history != nullptr)
                history->beginGesture();
            applyDrag (e);
            return;
        }
        case Drag::pitch:
        case Drag::time:
        case Drag::startEdge:
        case Drag::endEdge:
        case Drag::modHandle:
        case Drag::driftHandle:
        case Drag::gain:
        case Drag::tiltHandle:
        case Drag::transitionHandle:
            applyDrag (e);
            return;
        case Drag::none:
        default:
            return;
    }
}

void NoteGridView::mouseUp (const juce::MouseEvent& e)
{
    const auto finished = drag;
    drag = Drag::none;

    if (finished == Drag::pan && e.getDistanceFromDragStart() < 3 && onPlayFrom)
    {
        const int idx = noteIndexAt (dragStart);
        if (idx >= 0)
            onPlayFrom (notes[(size_t) idx].outStart());
    }

    if (history != nullptr && history->isInGesture())
    {
        const char* name = finished == Drag::pitch ? "Move pitch"
                         : finished == Drag::time ? "Move in time"
                         : finished == Drag::gain ? "Note gain"
                         : finished == Drag::tiltHandle ? "Pitch tilt"
                         : finished == Drag::transitionHandle ? "Note transition"
                         : finished == Drag::modHandle ? "Vibrato depth"
                         : finished == Drag::driftHandle ? "Pitch drift" : "Change note length";
        history->endGesture (name);
    }
    if (finished == Drag::lasso || finished == Drag::pan)
        repaint();
    if (onAuditionStop)
        onAuditionStop();
    mouseMove (e);
    refreshNotes();
}

void NoteGridView::mouseDoubleClick (const juce::MouseEvent& e)
{
    syncWithModel();
    const int index = noteIndexAt (e.position);
    if (index < 0 || ! notes[(size_t) index].isNote())
        return;
    // On a selected note: every selected note; otherwise just this one. One undo step.
    if (selection.count (notes[(size_t) index].id) == 0)
        setSelection ({ notes[(size_t) index].id });
    snapSelectionToNearest();
}

bool NoteGridView::keyPressed (const juce::KeyPress& key)
{
    syncWithModel();
    const auto mods = key.getModifiers();
    const int code = key.getKeyCode();

    if (history != nullptr && mods.isCommandDown() && (code == 'Z' || code == 'z'))
    {
        if (mods.isShiftDown()) history->redo(); else history->undo();
        refreshNotes();
        return true;
    }
    if (history != nullptr && mods.isCommandDown() && (code == 'Y' || code == 'y'))
    {
        history->redo();
        refreshNotes();
        return true;
    }
    if (mods.isCommandDown() && (code == 'A' || code == 'a'))
    {
        selectAll();
        return true;
    }
    if (key == juce::KeyPress::spaceKey && ! mods.isAnyModifierKeyDown())
    {
        if (onTogglePlay) onTogglePlay();
        return true;
    }
    if (key == juce::KeyPress::escapeKey)
    {
        if (tool != Tool::edit) { setTool (Tool::edit); if (onToolChanged) onToolChanged (tool); return true; }
        setSelection ({});
        return true;
    }
    if (! mods.isAnyModifierKeyDown())
    {
        const auto ch = juce::CharacterFunctions::toUpperCase (key.getTextCharacter());
        const Tool t = ch == 'E' ? Tool::edit : ch == 'S' ? Tool::split : ch == 'G' ? Tool::gain : tool;
        if (t != tool || ch == 'E' || ch == 'S' || ch == 'G')
            if (ch == 'E' || ch == 'S' || ch == 'G')
            {
                setTool (t);
                if (onToolChanged) onToolChanged (tool);
                return true;
            }
    }
    if (selection.empty())
        return false;

    if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey))
    {
        deleteNotes (selection);
        return true;
    }
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        const float dir = code == juce::KeyPress::upKey ? 1.0f : -1.0f;
        nudgePitch (dir * (mods.isShiftDown() ? 10.0f : 100.0f));
        return true;
    }
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        const double dir = code == juce::KeyPress::rightKey ? 1.0 : -1.0;
        nudgeTime (dir * (mods.isShiftDown() ? 0.001 : 0.010));
        return true;
    }
    return false;
}

void NoteGridView::showNoteMenu (juce::int64 noteId, double sourceSeconds)
{
    if (const auto* obj = findNote (notes, noteId); obj != nullptr && ! obj->isNote())
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (obj->kind == Note::breath ? "Breath" : obj->kind == Note::sibilant ? "Consonant (s / ts)" : "Consonant (k / t / p)");
        menu.addItem (1, "Silence");
        menu.addItem (2, "Original level (0 dB)");
        menu.addItem (3, "Remove the marker (leave the sound as it is)");
        juce::Component::SafePointer<NoteGridView> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                            [safe, noteId] (int choice)
                            {
                                if (safe == nullptr || choice == 0) return;
                                auto ids = safe->selection.count (noteId) != 0 ? safe->selection : std::set<juce::int64> { noteId };
                                if (choice == 3) { safe->deleteNotes (ids); return; }
                                auto all = safe->noteModel.getSnapshot();
                                for (auto& x : all)
                                    if (ids.count (x.id) != 0 && ! x.isNote())
                                        x.gainDb = choice == 1 ? Note::silentDb : 0.0f;
                                safe->commit (choice == 1 ? "Silence" : "Restore level", std::move (all));
                            });
        return;
    }
    juce::PopupMenu menu;
    menu.addItem (10, "Snap to nearest note");
    menu.addItem (11, "Reset pitch edits");
    menu.addItem (12, "Reset timing edits");
    menu.addItem (13, "Reset all edits");
    menu.addSeparator();
    menu.addSectionHeader ("Fix analysis");
    menu.addItem (1, "Split note here");
    menu.addItem (2, "Merge with next note");
    menu.addItem (3, "Detected an octave too low (+12)");
    menu.addItem (4, "Detected an octave too high (-12)");
    menu.addItem (5, selection.size() > 1 ? "Delete selected notes" : "Delete note (not a note)");

    juce::Component::SafePointer<NoteGridView> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                        [safe, noteId, sourceSeconds] (int choice)
                        {
                            if (safe == nullptr) return;
                            switch (choice)
                            {
                                case 1: safe->splitNoteAt (noteId, sourceSeconds); break;
                                case 2: safe->mergeWithNext (noteId); break;
                                case 3: safe->shiftOctave (noteId, 1); break;
                                case 4: safe->shiftOctave (noteId, -1); break;
                                case 5: safe->deleteNotes (safe->selection.empty() ? std::set<juce::int64> { noteId }
                                                                                    : safe->selection); break;
                                case 10: safe->snapSelectionToNearest(); break;
                                case 11: safe->resetEdits (true, false, false); break;
                                case 12: safe->resetEdits (false, true, false); break;
                                case 13: safe->resetEdits (true, true, true); break;
                                default: break;
                            }
                        });
}

// --- Edits ---------------------------------------------------------------------------------

void NoteGridView::nudgePitch (float cents)
{
    auto all = noteModel.getSnapshot();
    for (auto& n : all)
        if (selection.count (n.id) != 0 && n.isNote())
        {
            n.pitchShiftCents += cents;
            n.correctionCents = 0.0f;
        }
    commit ("Nudge pitch", std::move (all));
}

void NoteGridView::nudgeTime (double seconds)
{
    auto all = noteModel.getSnapshot();
    for (auto& n : all)
        if (selection.count (n.id) != 0)
            n.timeShiftSeconds += seconds;
    commit ("Nudge time", std::move (all));
}

void NoteGridView::snapSelectionToNearest()
{
    auto all = noteModel.getSnapshot();
    for (auto& n : all)
        if (selection.count (n.id) != 0 && n.isNote())
        {
            const float current = n.detectedPitch + n.pitchShiftCents / 100.0f;
            const float target = (snapEnabled && ! keyScale.isChromatic()) ? (float) keyScale.nearestScaleNote (current)
                                                                            : std::round (current);
            n.pitchShiftCents = (target - n.detectedPitch) * 100.0f;
            n.correctionCents = 0.0f;
        }
    commit ("Snap to nearest note", std::move (all));
}

void NoteGridView::resetEdits (bool pitch, bool time, bool sound)
{
    auto all = noteModel.getSnapshot();
    for (auto& n : all)
    {
        if (selection.count (n.id) == 0) continue;
        if (pitch)
        {
            n.pitchShiftCents = 0.0f; n.pitchModulation = 1.0f; n.pitchDrift = 1.0f;
            n.correctionCents = 0.0f; n.correctionDrift = 0.0f; n.correctionSpeedMs = 0.0f;
        }
        if (time)
        {
            n.timeShiftSeconds = 0.0; n.startEditSeconds = 0.0; n.endEditSeconds = 0.0;
        }
        if (sound)
        {
            n.formantCents = 0.0f; n.gainDb = 0.0f; n.eq = NoteEq{};
        }
    }
    commit ("Reset edits", std::move (all));
}

// Manual fixes of analysis errors (they change what was detected, not the sound).

void NoteGridView::splitNoteAt (juce::int64 noteId, double seconds)
{
    auto all = noteModel.getSnapshot();
    auto* n = findNote (all, noteId);
    if (n == nullptr || ! n->isNote() || seconds - n->startSeconds < 0.05 || n->endSeconds - seconds < 0.05)
        return;

    Note second = *n;
    second.id = noteModel.nextFreeId();
    second.startSeconds = seconds;
    second.startEditSeconds = 0.0;
    n->endSeconds = seconds;
    n->endEditSeconds = 0.0;
    if (analysis != nullptr)
    {
        n->detectedPitch = VocalAnalyzer::centrePitch (*analysis, n->startSeconds, n->endSeconds, n->detectedPitch);
        second.detectedPitch = VocalAnalyzer::centrePitch (*analysis, second.startSeconds, second.endSeconds, second.detectedPitch);
    }
    all.push_back (second);
    const auto secondId = second.id;
    commit ("Split note", std::move (all));
    setSelection ({ noteId, secondId });
}

void NoteGridView::mergeWithNext (juce::int64 noteId)
{
    auto all = noteModel.getSnapshot();   // sorted by start
    for (size_t i = 0; i + 1 < all.size(); ++i)
    {
        if (all[i].id != noteId)
            continue;
        auto& a = all[i];
        const auto& b = all[i + 1];
        if (b.startSeconds - a.endSeconds > 0.3)
            return;   // a real gap: these are separate notes
        a.endSeconds = b.endSeconds;
        a.endEditSeconds = b.endEditSeconds + (b.timeShiftSeconds - a.timeShiftSeconds);
        if (analysis != nullptr)
            a.detectedPitch = VocalAnalyzer::centrePitch (*analysis, a.startSeconds, a.endSeconds, a.detectedPitch);
        all.erase (all.begin() + (std::ptrdiff_t) i + 1);
        commit ("Merge notes", std::move (all));
        setSelection ({ noteId });
        return;
    }
}

void NoteGridView::shiftOctave (juce::int64 noteId, int octaves)
{
    auto all = noteModel.getSnapshot();
    for (auto& n : all)
        if (n.id == noteId || (selection.count (noteId) != 0 && selection.count (n.id) != 0))
            n.detectedPitch = juce::jlimit (0.0f, 127.0f, n.detectedPitch + 12.0f * (float) octaves);
    commit ("Fix octave", std::move (all));
}

void NoteGridView::deleteNotes (const std::set<juce::int64>& ids)
{
    auto all = noteModel.getSnapshot();
    all.erase (std::remove_if (all.begin(), all.end(), [&ids] (const Note& n) { return ids.count (n.id) != 0; }), all.end());
    commit ("Delete notes", std::move (all));
    setSelection ({});
}

} // namespace moontune
