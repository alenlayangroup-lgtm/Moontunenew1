#pragma once

#include <vector>
#include "Analysis/AnalysisResult.h"
#include "Model/NoteModel.h"

namespace moontune
{

// Monotonic piecewise-linear mapping between source time (the recording) and
// output time (what is played back after time edits).
//
// Anchors come from the note edges: a note's original [start, end] maps to its
// edited [outStart, outEnd]. Audio between notes is stretched to fit, so the
// result stays continuous (no gaps or overlaps).
//
// Consonant protection: inside a stretched segment, audible unvoiced material
// (consonants, breaths) keeps its original speed where possible and the voiced
// part absorbs the change, so "s", "t", "k" do not get smeared.
class TimeMap
{
public:
    struct Point { double src, out; };

    TimeMap() = default;   // identity

    static TimeMap build (const std::vector<Note>& notes, const AnalysisResult* frames,
                          double rangeStart, double rangeEnd, bool protectConsonants);

    double toSource (double outSeconds) const noexcept;   // output time -> source time
    double toOutput (double srcSeconds) const noexcept;   // source time -> output time
    double rateAt (double outSeconds) const noexcept;     // d(src)/d(out): >1 = faster, <1 = slower
    bool isIdentity() const noexcept { return points.empty(); }
    const std::vector<Point>& getPoints() const noexcept { return points; }

    static constexpr double minSegmentSeconds = 0.002;

private:
    std::vector<Point> points;   // sorted, strictly increasing in both src and out
};

} // namespace moontune
