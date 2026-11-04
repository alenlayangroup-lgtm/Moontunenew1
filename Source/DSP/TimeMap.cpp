#include "TimeMap.h"

#include <algorithm>
#include <cmath>

namespace moontune
{

namespace
{
    double lerpMap (const std::vector<TimeMap::Point>& pts, double x, bool fromOut) noexcept
    {
        // Outside the anchored area the map is the identity.
        auto key = [fromOut] (const TimeMap::Point& p) { return fromOut ? p.out : p.src; };
        auto val = [fromOut] (const TimeMap::Point& p) { return fromOut ? p.src : p.out; };

        if (pts.empty() || x <= key (pts.front()) || x >= key (pts.back()))
            return x;

        auto it = std::upper_bound (pts.begin(), pts.end(), x,
                                    [&] (double v, const TimeMap::Point& p) { return v < key (p); });
        const auto& b = *it;
        const auto& a = *(it - 1);
        const double span = key (b) - key (a);
        const double t = span > 0.0 ? (x - key (a)) / span : 0.0;
        return val (a) + (val (b) - val (a)) * t;
    }
}

TimeMap TimeMap::build (const std::vector<Note>& notesIn, const AnalysisResult* frames,
                        double rangeStart, double rangeEnd, bool protectConsonants)
{
    TimeMap map;

    std::vector<Note> notes;
    for (const auto& n : notesIn)
        if (n.endSeconds > rangeStart && n.startSeconds < rangeEnd)
            notes.push_back (n);
    std::sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b) { return a.startSeconds < b.startSeconds; });

    bool anyEdit = false;
    for (const auto& n : notes)
        anyEdit = anyEdit || n.hasTimeEdit();
    if (! anyEdit)
        return map;

    // 1. Anchors: range edges (fixed) and note edges.
    std::vector<Point> anchors { { rangeStart, rangeStart } };
    std::vector<bool> edited { false };
    const double eps = minSegmentSeconds;
    for (const auto& n : notes)
    {
        for (const Point p : { Point { n.startSeconds, n.outStart() }, Point { n.endSeconds, n.outEnd() } })
        {
            if (p.src >= rangeEnd - eps || p.src <= rangeStart + eps)
                continue;
            const bool pEdited = std::abs (p.out - p.src) > 1.0e-7;

            if (p.src <= anchors.back().src + eps)
            {
                // Shared edge of touching notes (legato): the edited side wins, so the
                // neighbour's edge follows (moving a note later lengthens the one before it).
                if (anchors.size() > 1)
                {
                    if (pEdited && ! edited.back()) { anchors.back().out = p.out; edited.back() = true; }
                    else if (pEdited && edited.back()) anchors.back().out = 0.5 * (anchors.back().out + p.out);
                }
                continue;
            }
            anchors.push_back (p);
            edited.push_back (pEdited);
        }
    }
    anchors.push_back ({ rangeEnd, rangeEnd });
    for (size_t i = 1; i + 1 < anchors.size(); ++i)
        anchors[i].out = juce::jlimit (rangeStart + eps * (double) i, rangeEnd - eps, anchors[i].out);

    // Repair any non-monotonic output (an edit pushed past its neighbour): squeeze forward.
    for (size_t i = 1; i < anchors.size(); ++i)
        anchors[i].out = std::max (anchors[i].out, anchors[i - 1].out + eps);
    for (size_t i = anchors.size() - 1; i-- > 0;)
        anchors[i].out = std::min (anchors[i].out, anchors[i + 1].out - eps);

    // 2. Refine each segment: protect consonants if requested.
    std::vector<Point> pts { anchors.front() };
    for (size_t i = 1; i < anchors.size(); ++i)
    {
        const auto a = anchors[i - 1], b = anchors[i];
        const double srcLen = b.src - a.src, outLen = b.out - a.out;
        const bool stretched = std::abs (outLen - srcLen) > 1.0e-6;

        if (protectConsonants && stretched && frames != nullptr && ! frames->frames.empty())
        {
            const double hop = frames->hopSeconds;
            // Split the segment into runs of "protected" (audible unvoiced) and "free" frames.
            struct Run { double len; bool prot; };
            std::vector<Run> runs;
            double protLen = 0.0, freeLen = 0.0;
            double t = a.src;
            while (t < b.src - 1.0e-9)
            {
                const auto* f = frames->frameAt (t);
                const bool prot = f != nullptr && f->cls == FrameClass::Unvoiced;
                double next = (std::floor (t / hop + 0.5) + 0.5) * hop;   // end of this frame's cell
                if (next <= t + 1.0e-9) next += hop;
                next = std::min (next, b.src);
                const double len = next - t;
                if (! runs.empty() && runs.back().prot == prot) runs.back().len += len;
                else runs.push_back ({ len, prot });
                (prot ? protLen : freeLen) += len;
                t = next;
            }

            // Consonants keep their speed (protRate = 1) and voiced audio absorbs the change, as far
            // as voiced audio can reasonably be stretched (x0.25 .. x4). Beyond that, consonants take
            // only the remainder instead of the whole segment being scaled uniformly.
            if (protLen > 0.0 && freeLen > 0.0)
            {
                constexpr double minRate = 0.25, maxRate = 4.0;
                double protRate = 1.0;
                double freeRate = (outLen - protLen) / freeLen;
                if (freeRate < minRate) { freeRate = minRate; protRate = (outLen - minRate * freeLen) / protLen; }
                if (freeRate > maxRate) { freeRate = maxRate; protRate = (outLen - maxRate * freeLen) / protLen; }

                if (protRate > 0.1 && protRate < 10.0)
                {
                    double s = a.src, o = a.out;
                    for (size_t r = 0; r + 1 < runs.size(); ++r)
                    {
                        s += runs[r].len;
                        o += runs[r].len * (runs[r].prot ? protRate : freeRate);
                        pts.push_back ({ s, o });
                    }
                }
            }
        }
        pts.push_back (b);
    }

    // Drop points that would make the map non-strict (tiny runs).
    std::vector<Point> clean { pts.front() };
    for (size_t i = 1; i < pts.size(); ++i)
        if (pts[i].src > clean.back().src + 1.0e-7 && pts[i].out > clean.back().out + 1.0e-7)
            clean.push_back (pts[i]);
    if (clean.back().src < rangeEnd - 1.0e-7)
        clean.back() = { rangeEnd, rangeEnd };

    map.points = std::move (clean);
    return map;
}

double TimeMap::toSource (double outSeconds) const noexcept
{
    return lerpMap (points, outSeconds, true);
}

double TimeMap::toOutput (double srcSeconds) const noexcept
{
    return lerpMap (points, srcSeconds, false);
}

double TimeMap::rateAt (double outSeconds) const noexcept
{
    if (points.size() < 2 || outSeconds <= points.front().out || outSeconds >= points.back().out)
        return 1.0;
    auto it = std::upper_bound (points.begin(), points.end(), outSeconds,
                                [] (double v, const Point& p) { return v < p.out; });
    const auto& b = *it;
    const auto& a = *(it - 1);
    return (b.src - a.src) / (b.out - a.out);
}

} // namespace moontune
