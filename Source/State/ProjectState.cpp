#include "ProjectState.h"

#include <cmath>

namespace moontune::ProjectState
{

juce::ValueTree createTree (juce::AudioProcessorValueTreeState& parameters,
                            const NoteModel& notes,
                            const juce::ValueTree& uiState,
                            const juce::ValueTree& takeState)
{
    juce::ValueTree root (ids::root);
    root.setProperty (ids::version, currentVersion, nullptr);
    root.setProperty (ids::appVersion, MOONTUNE_VERSION_STRING, nullptr);

    // Parameters are written by id/value pairs (not APVTS's own tree), so the
    // on-disk format does not depend on JUCE internals.
    juce::ValueTree params (ids::params);
    for (auto* p : parameters.processor.getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            juce::ValueTree child (ids::param);
            child.setProperty (ids::paramId, ranged->getParameterID(), nullptr);
            child.setProperty (ids::value, ranged->convertFrom0to1 (ranged->getValue()), nullptr);
            params.appendChild (child, nullptr);
        }
    }
    root.appendChild (params, nullptr);

    root.appendChild (notes.toValueTree(), nullptr);

    juce::ValueTree ui (ids::ui);
    if (uiState.isValid())
        ui.copyPropertiesFrom (uiState, nullptr);
    root.appendChild (ui, nullptr);

    if (takeState.isValid() && takeState.getNumProperties() > 0)
    {
        juce::ValueTree take (ids::take);
        take.copyPropertiesFrom (takeState, nullptr);
        root.appendChild (take, nullptr);
    }

    return root;
}

void save (juce::AudioProcessorValueTreeState& parameters,
           const NoteModel& notes,
           const juce::ValueTree& uiState,
           juce::MemoryBlock& destination,
           const juce::ValueTree& takeState)
{
    destination.reset();
    if (auto xml = createTree (parameters, notes, uiState, takeState).createXml())
        juce::AudioProcessor::copyXmlToBinary (*xml, destination);
}

LoadResult restore (const void* data, int sizeInBytes,
                    juce::AudioProcessorValueTreeState& parameters,
                    NoteModel& notes,
                    juce::ValueTree& uiState,
                    juce::ValueTree* takeState)
{
    if (data == nullptr || sizeInBytes <= 0)
        return { false, 0, 0, 0, "Empty state: keeping current settings." };

    auto xml = juce::AudioProcessor::getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return { false, 0, 0, 0, "State data is not readable: keeping current settings." };

    return restoreFromTree (juce::ValueTree::fromXml (*xml), parameters, notes, uiState, takeState);
}

LoadResult restoreFromTree (const juce::ValueTree& tree,
                            juce::AudioProcessorValueTreeState& parameters,
                            NoteModel& notes,
                            juce::ValueTree& uiState,
                            juce::ValueTree* takeState)
{
    LoadResult result;

    if (! tree.isValid() || ! tree.hasType (ids::root))
    {
        result.message = "State does not belong to MoonTune: keeping current settings.";
        return result;
    }

    result.version = (int) tree.getProperty (ids::version, 0);
    if (result.version <= 0)
    {
        result.message = "State has no valid version: keeping current settings.";
        return result;
    }

    // --- Migrations go here (version < currentVersion). None yet. ---

    // Parameters: every known parameter gets a value - from the file if present and finite, else default.
    const auto savedParams = tree.getChildWithName (ids::params);
    for (auto* p : parameters.processor.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (ranged == nullptr)
            continue;

        const auto saved = savedParams.getChildWithProperty (ids::paramId, ranged->getParameterID());
        const auto& range = ranged->getNormalisableRange();
        float normalised = ranged->getDefaultValue();

        if (saved.isValid() && saved.hasProperty (ids::value))
        {
            const auto plain = (float) (double) saved.getProperty (ids::value);
            if (std::isfinite (plain))
                normalised = range.convertTo0to1 (juce::jlimit (range.start, range.end, plain));
            else
                ++result.missingParameters;
        }
        else
        {
            ++result.missingParameters;
        }

        ranged->setValueNotifyingHost (normalised);
    }

    result.rejectedNotes = notes.fromValueTree (tree.getChildWithName (NoteModel::treeType));

    const auto savedUi = tree.getChildWithName (ids::ui);
    if (uiState.isValid())
    {
        uiState.removeAllProperties (nullptr);
        if (savedUi.isValid())
            uiState.copyPropertiesFrom (savedUi, nullptr);
    }

    if (takeState != nullptr)
    {
        *takeState = juce::ValueTree (ids::take);
        const auto savedTake = tree.getChildWithName (ids::take);
        if (savedTake.isValid())
            takeState->copyPropertiesFrom (savedTake, nullptr);
    }

    result.loaded = true;
    result.message = "State v" + juce::String (result.version) + " loaded";
    if (result.version > currentVersion)
        result.message << " (saved by a newer MoonTune; unknown data ignored)";
    if (result.missingParameters > 0)
        result.message << ", " << result.missingParameters << " parameter(s) reset to default";
    if (result.rejectedNotes > 0)
        result.message << ", " << result.rejectedNotes << " invalid note(s) skipped";
    result.message << ".";
    return result;
}

} // namespace moontune::ProjectState
