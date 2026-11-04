#include "KeyDetector.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace moontune
{

namespace
{
    // Krumhansl & Kessler (1982) probe-tone profiles, tonic first.
    constexpr std::array<double, 12> majorProfile { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    constexpr std::array<double, 12> minorProfile { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

    double correlate (const std::array<double, 12>& hist, const std::array<double, 12>& profile, int root)
    {
        double mh = 0, mp = 0;
        for (int i = 0; i < 12; ++i) { mh += hist[(size_t) i]; mp += profile[(size_t) i]; }
        mh /= 12.0; mp /= 12.0;

        double num = 0, dh = 0, dp = 0;
        for (int pc = 0; pc < 12; ++pc)
        {
            const double h = hist[(size_t) pc] - mh;
            const double p = profile[(size_t) ((pc - root + 12) % 12)] - mp;
            num += h * p; dh += h * h; dp += p * p;
        }
        return (dh > 0 && dp > 0) ? num / std::sqrt (dh * dp) : 0.0;
    }

    bool areRelative (const KeyScale& a, const KeyScale& b)
    {
        const bool aMaj = a.getType() == ScaleType::Major, bMaj = b.getType() == ScaleType::Major;
        if (aMaj == bMaj)
            return false;
        const auto& maj = aMaj ? a : b;
        const auto& min = aMaj ? b : a;
        return (maj.getRoot() + 9) % 12 == min.getRoot();
    }
}

KeyDetector::Result KeyDetector::detect (const std::vector<Note>& input)
{
    Result r;

    auto notes = input;
    std::sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b) { return a.startSeconds < b.startSeconds; });

    std::array<double, 12> hist {};
    std::array<bool, 12> seen {};
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& n = notes[i];
        const double dur = n.endSeconds - n.startSeconds;
        if (dur < 0.06 || ! std::isfinite (n.detectedPitch) || ! n.isNote())
            continue;   // too short to be heard as a scale degree

        const float pitch = n.detectedPitch + n.pitchShiftCents / 100.0f;
        const int pc = ((int) std::lround (pitch) % 12 + 12) % 12;

        double w = std::min (dur, 2.0);   // a single very long note must not dominate
        const bool phraseEnd = (i + 1 == notes.size()) || (notes[i + 1].startSeconds - n.endSeconds > 0.4);
        if (phraseEnd)
            w *= 1.5;

        hist[(size_t) pc] += w;
        seen[(size_t) pc] = true;
        r.sungSeconds += dur;
        ++r.noteCount;
    }

    const int distinct = (int) std::count (seen.begin(), seen.end(), true);
    if (r.noteCount == 0)
    {
        r.summary = "No notes yet: record the vocal first.";
        return r;
    }
    if (distinct < 3)
    {
        r.summary = "Too few different notes to suggest a key.";
        return r;
    }

    std::vector<Candidate> all;
    for (int root = 0; root < 12; ++root)
    {
        all.push_back ({ KeyScale (root, ScaleType::Major), (float) correlate (hist, majorProfile, root), 0.0f });
        all.push_back ({ KeyScale (root, ScaleType::NaturalMinor), (float) correlate (hist, minorProfile, root), 0.0f });
    }
    std::sort (all.begin(), all.end(), [] (const Candidate& a, const Candidate& b) { return a.correlation > b.correlation; });

    // Softmax over correlations -> relative confidence.
    constexpr double sharpness = 12.0;
    double sum = 0.0;
    for (auto& c : all) sum += std::exp (sharpness * (c.correlation - all.front().correlation));
    for (auto& c : all) c.confidence = (float) (std::exp (sharpness * (c.correlation - all.front().correlation)) / sum);

    r.enoughData = r.sungSeconds >= minSecondsForConfidence && r.noteCount >= minNotesForConfidence && distinct >= 5;
    const float gap = all[0].correlation - all[1].correlation;
    r.relativeAmbiguity = areRelative (all[0].key, all[1].key) && gap < 0.08f;
    r.ambiguous = ! r.enoughData || gap < 0.05f || r.relativeAmbiguity || all[0].correlation < 0.5f;

    const size_t keep = std::min<size_t> (r.ambiguous ? 4 : 2, all.size());
    r.candidates.assign (all.begin(), all.begin() + (std::ptrdiff_t) keep);

    if (! r.enoughData)
        r.summary = "Little material (" + juce::String (r.sungSeconds, 1) + " s, " + juce::String (r.noteCount)
                  + " notes): treat this as a guess. Record more of the song for a better estimate.";
    else if (r.relativeAmbiguity)
        r.summary = "The vocal fits " + all[0].key.getName() + " and " + all[1].key.getName()
                  + " almost equally (same notes). The full mix would decide; choose by ear.";
    else if (r.ambiguous)
        r.summary = "Several keys fit similarly well. Check the alternatives by ear.";
    else
        r.summary = "Most likely " + all[0].key.getName() + ".";

    return r;
}

} // namespace moontune
