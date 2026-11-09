cmake_minimum_required(VERSION 3.22)
project(NightCap VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

# Use a local JUCE checkout with -DJUCE_DIR=/path/to/JUCE, otherwise download JUCE 8.0.4
if(DEFINED JUCE_DIR)
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.4 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

if(APPLE)
    set(NIGHTCAP_FORMATS AU VST3)
else()
    set(NIGHTCAP_FORMATS VST3)
endif()

set(NIGHTCAP_SOURCES
    Source/kNocTurnLink.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

function(nightcap_target target name code is_synth au_type)
    juce_add_plugin(${target}
        PRODUCT_NAME "${name}"
        COMPANY_NAME "digitui"
        BUNDLE_ID "com.digitui.${target}"
        PLUGIN_MANUFACTURER_CODE Dgtu
        PLUGIN_CODE ${code}
        FORMATS ${NIGHTCAP_FORMATS}
        IS_SYNTH ${is_synth}
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        AU_MAIN_TYPE ${au_type}
        VST3_CATEGORIES Fx Instrument
        COPY_PLUGIN_AFTER_BUILD TRUE
        MICROPHONE_PERMISSION_ENABLED FALSE)
    target_sources(${target} PRIVATE ${NIGHTCAP_SOURCES})
    target_compile_definitions(${target} PUBLIC
        JUCE_PLUGINHOST_AU=1
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_LADSPA=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(${target} PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_devices
        PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
endfunction()

nightcap_target(NightCap   "kNocTurn NightCap"    Ngti TRUE  kAudioUnitType_MusicDevice)
nightcap_target(NightCapFX "kNocTurn NightCap FX" Ngtf FALSE kAudioUnitType_MusicEffect)
