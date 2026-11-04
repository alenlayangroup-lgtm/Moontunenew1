#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

#include "AnalysisResult.h"
#include "VocalAnalyzer.h"
#include "Capture/TakeAudio.h"
#include "Model/NoteModel.h"

namespace moontune
{

// Runs vocal analysis on a background thread and publishes the result.
//
// Jobs:
//   - analyseCapture: a capture session ended. The take is saved to disk (FLAC), analysed,
//     and the notes inside the captured time range are replaced by the new ones.
//     Notes outside that range (including manual fixes) are kept.
//   - loadTake: a project was reopened. The take is loaded from disk and analysed for the
//     display (pitch curves, envelopes). Notes come from the project and are kept as they are.
//
// Failures never throw into the host: the status carries a readable message instead.
class AnalysisManager : private juce::Thread
{
public:
    enum class Status { idle, analysing, ready, failed };

    explicit AnalysisManager (NoteModel&);
    ~AnalysisManager() override;

    void analyseCapture (std::shared_ptr<TakeAudio>, TakeAudio::Range capturedRange);
    void loadTake (const juce::String& takeId, std::vector<TakeAudio::Range> ranges);

    // ARA: a take assembled from the host's audio events. Notes inside `replace` (seconds) are
    // replaced by the analysis; all other notes are kept. If `replaceAllWhenEmpty` and there are
    // no notes yet, every analysed note is taken. Nothing is written to disk.
    void analyseImported (std::shared_ptr<TakeAudio>, std::vector<std::pair<double, double>> replace,
                          bool replaceAllWhenEmpty);
    void clear();
    void stop();   // stop the worker thread (owner calls this before tearing down)

    std::shared_ptr<const AnalysisResult> getResult() const;
    juce::uint32 getResultVersion() const noexcept { return resultVersion.load(); }

    Status getStatus() const noexcept { return status.load(); }
    float getProgress() const noexcept { return progress.load(); }
    juce::String getMessage() const;

    // Called on the analysis thread when a take was loaded from disk.
    std::function<void (std::shared_ptr<TakeAudio>)> onTakeLoaded;

    // Where captured audio is kept. Overridable for tests.
    void setStorageDirectory (const juce::File& dir);
    juce::File getTakeFile (const juce::String& takeId) const;

    // Tests: block until the queue is empty and no job is running (or timeout).
    bool waitUntilIdle (int timeoutMs);

private:
    struct Job
    {
        enum class Type { analyseCapture, loadTake, analyseImported };
        Type type = Type::analyseCapture;
        std::shared_ptr<TakeAudio> take;
        TakeAudio::Range range;
        juce::String takeId;
        std::vector<TakeAudio::Range> ranges;
        std::vector<std::pair<double, double>> replace;
        bool replaceAllWhenEmpty = false;
    };

    void run() override;
    void process (const Job&);
    void publish (std::shared_ptr<const AnalysisResult>);
    void mergeNotes (const AnalysisResult&, double fromSeconds, double toSeconds);
    void setMessage (Status, const juce::String&);

    NoteModel& noteModel;

    mutable std::mutex mutex;   // queue, result, message, storage dir
    std::deque<Job> queue;
    bool busy = false;
    std::shared_ptr<const AnalysisResult> result;
    juce::String message;
    juce::File storageDir;

    std::atomic<juce::uint32> resultVersion { 0 };
    std::atomic<Status> status { Status::idle };
    std::atomic<float> progress { 0.0f };
    std::atomic<bool> cancel { false };
    juce::WaitableEvent wake;
};

} // namespace moontune
