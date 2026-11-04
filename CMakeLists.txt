cmake_minimum_required(VERSION 3.22)

# macOS: one universal binary for Apple Silicon and Intel Macs (set before project()).
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")
endif()

project(MoonTune VERSION 0.3.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MOONTUNE_BUILD_TESTS "Build MoonTune unit tests" ON)
option(MOONTUNE_ARA "Build the plugin as an ARA 2 extension" ON)
set(MOONTUNE_JUCE_PATH "" CACHE PATH "Path to a local JUCE checkout (empty = download JUCE 8.0.10)")
set(MOONTUNE_WEBSITE "https://www.moonmusiclabel.com" CACHE STRING "Company web site shown by hosts (ARA requires one)")
set(MOONTUNE_ARA_SDK_PATH "" CACHE PATH "Path to a local Celemony ARA_SDK checkout (empty = download ARA SDK 2.2.0)")

# Static MSVC runtime: the VST3 does not depend on the VC++ redistributable on the user's machine.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------
# JUCE
# ---------------------------------------------------------------------------
if(MOONTUNE_JUCE_PATH)
    add_subdirectory(${MOONTUNE_JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.10
        GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---------------------------------------------------------------------------
# ARA 2 SDK (Celemony, Apache 2.0). JUCE builds the ARA plugin glue from it.
# ---------------------------------------------------------------------------
if(MOONTUNE_ARA)
    if(NOT MOONTUNE_ARA_SDK_PATH)
        include(FetchContent)
        FetchContent_Declare(ARA_SDK
            GIT_REPOSITORY https://github.com/Celemony/ARA_SDK.git
            GIT_TAG        releases/2.2.0
            GIT_SHALLOW    ON
            GIT_SUBMODULES ARA_API ARA_Library)
        FetchContent_MakeAvailable(ARA_SDK)
        set(MOONTUNE_ARA_SDK_PATH ${ara_sdk_SOURCE_DIR})
    endif()
    juce_set_ara_sdk_path(${MOONTUNE_ARA_SDK_PATH})
endif()

# ---------------------------------------------------------------------------
# Source lists. The plugin and the test runner compile the same sources,
# so the tests exercise exactly the code that ships.
# ---------------------------------------------------------------------------
set(MOONTUNE_CORE_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Model/NoteModel.cpp
    Source/State/ProjectState.cpp
    Source/DSP/PassthroughEngine.cpp
    Source/DSP/TimeMap.cpp
    Source/DSP/VocalRenderer.cpp
    Source/DSP/RenderManager.cpp
    Source/Model/EditHistory.cpp
    Source/Music/ScaleCorrector.cpp
    Source/Music/KeyScale.cpp
    Source/Music/KeyDetector.cpp
    Source/Capture/TakeAudio.cpp
    Source/Capture/AudioCapture.cpp
    Source/Analysis/PitchDetector.cpp
    Source/Analysis/VocalAnalyzer.cpp
    Source/Analysis/AnalysisManager.cpp
    Source/UI/MoonLookAndFeel.cpp
    Source/UI/ScaleToolbar.cpp
    Source/UI/NoteInspector.cpp
    Source/UI/CorrectPitchPanel.cpp
    Source/UI/NoteGridView.cpp
    Source/UI/OverviewBar.cpp
    Source/UI/LevelMeter.cpp)

function(moontune_configure target)
    target_sources(${target} PRIVATE ${MOONTUNE_CORE_SOURCES})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_DISPLAY_SPLASH_SCREEN=0
        MOONTUNE_VERSION_STRING="${PROJECT_VERSION}")
    if(UNIX AND NOT APPLE)
        # Linux is used only for compile checks and tests; these X11 extensions are optional in JUCE.
        target_compile_definitions(${target} PRIVATE
            JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0 JUCE_USE_XRENDER=0)
    endif()
    target_link_libraries(${target} PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_gui_basics
        juce::juce_data_structures
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
endfunction()

# ---------------------------------------------------------------------------
# Plugin: VST3 (Windows, macOS) + Audio Unit (macOS), ARA 2 extension
# ---------------------------------------------------------------------------
if(APPLE)
    set(MOONTUNE_FORMATS VST3 AU)
else()
    set(MOONTUNE_FORMATS VST3)
endif()

juce_add_plugin(MoonTune
    COMPANY_NAME             "Moon Music Label"
    COMPANY_WEBSITE          "${MOONTUNE_WEBSITE}"
    PRODUCT_NAME             "MoonTune"
    BUNDLE_ID                com.moonmusiclabel.moontune
    PLUGIN_MANUFACTURER_CODE MnML
    PLUGIN_CODE              MnTn
    FORMATS                  ${MOONTUNE_FORMATS}
    IS_ARA_EFFECT            ${MOONTUNE_ARA}
    ARA_FACTORY_ID           "\"com.moonmusiclabel.moontune.arafactory.1\""
    ARA_DOCUMENT_ARCHIVE_ID  "\"com.moonmusiclabel.moontune.aradocumentarchive.1\""
    AU_MAIN_TYPE             kAudioUnitType_Effect
    IS_SYNTH                 FALSE
    NEEDS_MIDI_INPUT         FALSE
    NEEDS_MIDI_OUTPUT        FALSE
    IS_MIDI_EFFECT           FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES          Fx Pitch\ Shift
    COPY_PLUGIN_AFTER_BUILD  FALSE)

moontune_configure(MoonTune)
target_sources(MoonTune PRIVATE Source/ARA/MoonTuneARA.cpp)
target_compile_definitions(MoonTune PRIVATE JUCE_VST3_CAN_REPLACE_VST2=0)

# ---------------------------------------------------------------------------
# Tests (console app, no host required)
# ---------------------------------------------------------------------------
if(MOONTUNE_BUILD_TESTS)
    enable_testing()
    juce_add_console_app(MoonTuneTests PRODUCT_NAME "MoonTuneTests")
    moontune_configure(MoonTuneTests)
    target_sources(MoonTuneTests PRIVATE
        Tests/TestMain.cpp
        Tests/PassthroughTests.cpp
        Tests/StateTests.cpp
        Tests/NoteModelTests.cpp
        Tests/KeyScaleTests.cpp
        Tests/AnalysisTests.cpp
        Tests/CaptureTests.cpp
        Tests/RenderTests.cpp
        Tests/EditingTests.cpp
        Tests/RealFileCheck.cpp)
    target_compile_definitions(MoonTuneTests PRIVATE
        JucePlugin_Name="MoonTune"
        JucePlugin_IsSynth=0
        JucePlugin_WantsMidiInput=0
        JucePlugin_ProducesMidiOutput=0
        JucePlugin_IsMidiEffect=0
        MOONTUNE_TEST_BUILD=1)
    add_test(NAME MoonTuneTests COMMAND MoonTuneTests)
endif()

# The voice engines' inner loops are long float reductions: allow the compiler to reorder them
# so they vectorise (results stay deterministic per build; bit-exact paths only copy samples).
if (MSVC)
    set_source_files_properties (Source/DSP/VocalRenderer.cpp PROPERTIES COMPILE_OPTIONS "/fp:fast")
else()
    set_source_files_properties (Source/DSP/VocalRenderer.cpp PROPERTIES COMPILE_OPTIONS
        "-fno-math-errno;-fassociative-math;-fno-signed-zeros;-fno-trapping-math")
endif()
