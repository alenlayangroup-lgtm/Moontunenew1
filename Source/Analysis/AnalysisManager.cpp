#include "AnalysisManager.h"

#include <algorithm>

namespace moontune
{

AnalysisManager::AnalysisManager (NoteModel& model)
    : juce::Thread ("MoonTune analysis"), noteModel (model)
{
    storageDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                     .getChildFile ("Moon Music Label").getChildFile ("MoonTune").getChildFile ("Takes");
    startThread (juce::Thread::Priority::low);
}

AnalysisManager::~AnalysisManager()
{
    stop();
}

void AnalysisManager::stop()
{
    cancel.store (true);
    signalThreadShouldExit();
    wake.signal();
    stopThread (10000);
}

void AnalysisManager::setStorageDirectory (const juce::File& dir)
{
    const std::lock_guard<std::mutex> lock (mutex);
    storageDir = dir;
}

juce::File AnalysisManager::getTakeFile (const juce::String& takeId) const
{
    const std::lock_guard<std::mutex> lock (mutex);
    // Ids are UUIDs; anything else is reduced to safe file-name characters.
    return storageDir.getChildFile (juce::File::createLegalFileName (takeId) + ".flac");
}

void AnalysisManager::analyseCapture (std::shared_ptr<TakeAudio> take, TakeAudio::Range capturedRange)
{
    if (take == nullptr)
        return;
    {
        const std::lock_guard<std::mutex> lock (mutex);
        Job job;
        job.type = Job::Type::analyseCapture;
        job.take = std::move (take);
        job.range = capturedRange;
        queue.push_back (std::move (job));
    }
    wake.signal();
}

void AnalysisManager::loadTake (const juce::String& takeId, std::vector<TakeAudio::Range> ranges)
{
    if (takeId.isEmpty())
        return;
    {
        const std::lock_guard<std::mutex> lock (mutex);
        Job job;
        job.type = Job::Type::loadTake;
        job.takeId = takeId;
        job.ranges = std::move (ranges);
        queue.push_back (std::move (job));
    }
    wake.signal();
}

void AnalysisManager::analyseImported (std::shared_ptr<TakeAudio> take, std::vector<std::pair<double, double>> replace,
                                       bool replaceAllWhenEmpty)
{
    if (take == nullptr)
        return;
    {
        const std::lock_guard<std::mutex> lock (mutex);
        // Only the newest layout matters: drop queued imports.
        queue.erase (std::remove_if (queue.begin(), queue.end(), [] (const Job& j) { return j.type == Job::Type::analyseImported; }),
                     queue.end());
        Job job;
        job.type = Job::Type::analyseImported;
        job.take = std::move (take);
        job.replace = std::move (replace);
        job.replaceAllWhenEmpty = replaceAllWhenEmpty;
        queue.push_back (std::move (job));
    }
    wake.signal();
}

void AnalysisManager::clear()
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        queue.clear();
        result.reset();
        message.clear();
    }
    cancel.store (true);          // abort a running analysis; its result is discarded
    status.store (Status::idle);
    progress.store (0.0f);
    ++resultVersion;
}

std::shared_ptr<const AnalysisResult> AnalysisManager::getResult() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return result;
}

juce::String AnalysisManager::getMessage() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return message;
}

void AnalysisManager::setMessage (Status s, const juce::String& text)
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        message = text;
    }
    status.store (s);
}

void AnalysisManager::publish (std::shared_ptr<const AnalysisResult> r)
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        result = std::move (r);
    }
    ++resultVersion;
}

bool AnalysisManager::waitUntilIdle (int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (juce::Time::getMillisecondCounter() < end)
    {
        {
            const std::lock_guard<std::mutex> lock (mutex);
            if (queue.empty() && ! busy)
                return true;
        }
        juce::Thread::sleep (5);
    }
    return false;
}

void AnalysisManager::run()
{
    while (! threadShouldExit())
    {
        Job job;
        bool haveJob = false;
        {
            const std::lock_guard<std::mutex> lock (mutex);
            if (! queue.empty())
            {
                job = std::move (queue.front());
                queue.pop_front();
                busy = true;
                haveJob = true;
                cancel.store (false);   // inside the lock: a clear() after this point cancels this job
            }
        }

        if (! haveJob)
        {
            wake.wait (200);
            continue;
        }

        try
        {
            process (job);
        }
        catch (const std::exception& e)
        {
            setMessage (Status::failed, juce::String ("Analysis failed: ") + e.what());
        }
        catch (...)
        {
            setMessage (Status::failed, "Analysis failed (unknown error).");
        }

        {
            const std::lock_guard<std::mutex> lock (mutex);
            busy = false;
        }
    }
}

void AnalysisManager::process (const Job& job)
{
    std::shared_ptr<TakeAudio> take = job.take;
    progress.store (0.0f);

    if (job.type == Job::Type::loadTake)
    {
        setMessage (Status::analysing, "Loading captured vocal...");
        const auto file = getTakeFile (job.takeId);
        if (! file.existsAsFile())
        {
            setMessage (Status::failed, "Captured audio for this project was not found. Notes are kept; "
                                        "record the vocal again to see pitch curves.");
            return;
        }
        take = TakeAudio::loadFlac (file, job.takeId, job.ranges);
        if (take == nullptr)
        {
            setMessage (Status::failed, "Captured audio file could not be read. Record the vocal again.");
            return;
        }
        if (onTakeLoaded)
            onTakeLoaded (take);
    }
    else if (job.type == Job::Type::analyseCapture)
    {
        setMessage (Status::analysing, "Saving captured vocal...");
        if (! take->saveFlac (getTakeFile (take->getId())))
        {
            // Analysis still runs; after reopening the project the user is asked to record again.
            DBG ("MoonTune: could not save take to disk");
        }
    }

    setMessage (Status::analysing, "Analysing vocal...");
    auto analysed = std::make_shared<AnalysisResult> (
        VocalAnalyzer::analyse (*take, {}, &cancel, [this] (float p) { progress.store (p); }));

    if (cancel.load() || threadShouldExit())
        return;   // cleared or shutting down

    if (job.type == Job::Type::analyseCapture)
    {
        const double sr = take->getSampleRate();
        mergeNotes (*analysed, (double) job.range.start / sr, (double) job.range.end / sr);
    }
    else if (job.type == Job::Type::analyseImported)
    {
        if (job.replaceAllWhenEmpty && noteModel.size() == 0)
            mergeNotes (*analysed, 0.0, 1.0e9);
        else
            for (const auto& [from, to] : job.replace)
                mergeNotes (*analysed, from, to);
    }
    else if (noteModel.size() == 0)
    {
        // Project had no notes saved: take them from the analysis.
        mergeNotes (*analysed, 0.0, 1.0e9);
    }

    publish (analysed);
    progress.store (1.0f);
    setMessage (Status::ready, juce::String ((int) noteModel.size()) + " notes");
}

void AnalysisManager::mergeNotes (const AnalysisResult& r, double from, double to)
{
    auto existing = noteModel.getSnapshot();
    std::vector<Note> merged;

    for (const auto& n : existing)
        if (n.endSeconds <= from || n.startSeconds >= to)   // outside the re-captured range: keep
            merged.push_back (n);

    juce::int64 nextId = noteModel.nextFreeId();
    for (const auto& a : r.notes)
    {
        const double mid = 0.5 * (a.startSeconds + a.endSeconds);
        if (mid < from || mid > to)
            continue;

        Note n;
        n.id = nextId++;
        n.startSeconds = a.startSeconds;
        n.endSeconds = a.endSeconds;
        n.detectedPitch = juce::jlimit (0.0f, 127.0f, a.pitch);
        if (NoteModel::isValid (n))
            merged.push_back (n);
    }

    // Breaths and consonants become their own objects (quieter / louder / silent), drawn at the
    // height of the nearest note.
    for (const auto& nz : r.noise)
    {
        const double mid = 0.5 * (nz.startSeconds + nz.endSeconds);
        if (mid < from || mid > to)
            continue;
        Note n;
        n.id = nextId++;
        n.kind = juce::jlimit ((int) Note::breath, (int) Note::plosive, nz.kind);
        n.startSeconds = nz.startSeconds;
        n.endSeconds = nz.endSeconds;
        float pitch = 60.0f;
        double best = 1.0e9;
        for (const auto& a : r.notes)
        {
            const double gap = std::min (std::abs (a.endSeconds - nz.startSeconds), std::abs (a.startSeconds - nz.endSeconds));
            if (gap < best) { best = gap; pitch = a.pitch; }
        }
        n.detectedPitch = juce::jlimit (0.0f, 127.0f, pitch);
        if (NoteModel::isValid (n))
            merged.push_back (n);
    }

    noteModel.replaceAll (std::move (merged));
}

} // namespace moontune
