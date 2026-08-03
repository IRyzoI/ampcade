cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(AmpCade VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Install VST3/AU into the user's plugin folders after building (dev machines).
# CI turns this off — Windows runners can't write to Common Files.
if(APPLE)
    option(AMPCADE_COPY_PLUGIN "Copy plugin to user plugin dirs after build" ON)
else()
    option(AMPCADE_COPY_PLUGIN "Copy plugin to user plugin dirs after build" OFF)
endif()

add_subdirectory(third_party/JUCE)

# ------------------------------------------------------------------ NAM core
set(NAM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/NeuralAmpModelerCore)
file(GLOB NAM_SOURCES ${NAM_DIR}/NAM/*.cpp ${NAM_DIR}/NAM/*/*.cpp)

# OBJECT rather than STATIC, but note that this is NOT on its own enough to keep
# NAM's self-registering architectures alive: juce_add_plugin's shared-code target
# is a static library, so these objects still reach each plugin format through
# AmpCade_SharedCode.a and get dropped unless referenced. That is what
# keepNamArchitecturesLinked in src/NamSlot.cpp is for, and what the POST_BUILD
# check below enforces. (v0.1–v0.2.1 shipped with every architecture stripped.)
add_library(nam_core OBJECT ${NAM_SOURCES})
target_include_directories(nam_core PUBLIC
    ${NAM_DIR}
    ${NAM_DIR}/Dependencies/eigen
    ${NAM_DIR}/Dependencies/nlohmann
    ${NAM_DIR}/Dependencies/AudioDSPTools)
target_compile_definitions(nam_core PUBLIC NAM_SAMPLE_FLOAT)
set_target_properties(nam_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    # Eigen-heavy translation units want real optimization even in dev builds
    target_compile_options(nam_core PRIVATE -O3)
endif()
if(WIN32)
    target_compile_definitions(nam_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ------------------------------------------------------------------ Plugin
juce_add_plugin(AmpCade
    COMPANY_NAME "AmpCade"
    BUNDLE_ID com.ampcade.ampcade
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon_1024.png"
    FORMATS Standalone VST3 AU
    PLUGIN_MANUFACTURER_CODE Ampc
    PLUGIN_CODE Acad
    PRODUCT_NAME "AmpCade"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    NEEDS_WEB_BROWSER TRUE
    NEEDS_WEBVIEW2 TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "AmpCade needs access to your audio interface to hear your guitar."
    COPY_PLUGIN_AFTER_BUILD ${AMPCADE_COPY_PLUGIN}
    VST3_CATEGORIES Fx Distortion
    AU_MAIN_TYPE kAudioUnitType_Effect)

target_sources(AmpCade PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/NamSlot.cpp
    src/Tone3000Client.cpp
    src/Library.cpp)

target_include_directories(AmpCade PRIVATE src)

juce_add_binary_data(AmpCadeAssets SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/modules/juce_gui_extra/native/javascript/index.js
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/presets/Clean.ampcade
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/presets/Lonestar.ampcade)

target_compile_definitions(AmpCade PRIVATE
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    # NamSlot.h uses std::min/max and is compiled into the plugin targets too —
    # without NOMINMAX, windows.h's min/max macros break it under MSVC.
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
    $<$<PLATFORM_ID:Windows>:JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1>)

target_link_libraries(AmpCade
    PRIVATE
        AmpCadeAssets
        nam_core
        juce::juce_audio_utils
        juce::juce_cryptography
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags)

# The NAM architectures reach the plugin through AmpCade_SharedCode.a and the
# linker will drop them unless something references them (see
# keepNamArchitecturesLinked in src/NamSlot.cpp). Verify every format actually
# contains them — the offline smoke test cannot catch this, because a console app
# links the nam_core objects directly instead of through the archive.
if(NOT WIN32)
    foreach(fmt Standalone VST3 AU)
        if(TARGET AmpCade_${fmt})
            add_custom_command(TARGET AmpCade_${fmt} POST_BUILD
                COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-nam-linked.sh"
                        "$<TARGET_FILE:AmpCade_${fmt}>"
                VERBATIM)
        endif()
    endforeach()
endif()

# ------------------------------------------------------------------ Offline smoke test
juce_add_console_app(AmpCadeSmoke PRODUCT_NAME "ampcade-smoke")
target_sources(AmpCadeSmoke PRIVATE test/SmokeTest.cpp src/NamSlot.cpp
    src/Tone3000Client.cpp src/Library.cpp)
target_include_directories(AmpCadeSmoke PRIVATE src)
target_compile_definitions(AmpCadeSmoke PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    AMPCADE_SMOKE=1
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>)
target_link_libraries(AmpCadeSmoke
    PRIVATE
        nam_core
        juce::juce_core
        juce::juce_cryptography
        juce::juce_events
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)

# ------------------------------------------------------------------ State test
# Session-state round trips (quit -> relaunch), driving the real processor with
# no UI: AMPCADE_SMOKE strips the editor out of PluginProcessor.cpp and the
# BinaryData factory presets out of Library.cpp. Kept apart from the smoke test
# so the audio-path check stays quick — this one drags in juce_audio_processors
# and the modules behind it. Neither is built by CI; both are run by hand.
juce_add_console_app(AmpCadeState PRODUCT_NAME "ampcade-state")
target_sources(AmpCadeState PRIVATE test/StateTest.cpp src/PluginProcessor.cpp
    src/NamSlot.cpp src/Tone3000Client.cpp src/Library.cpp)
target_include_directories(AmpCadeState PRIVATE src)
target_compile_definitions(AmpCadeState PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    AMPCADE_SMOKE=1
    # setStateInformation queues the slot restore and the launch landing onto
    # the message thread; the test pumps them with runDispatchLoopUntil, which
    # only exists when modal loops are permitted.
    JUCE_MODAL_LOOPS_PERMITTED=1
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>)
target_link_libraries(AmpCadeState
    PRIVATE
        nam_core
        juce::juce_core
        juce::juce_cryptography
        juce::juce_events
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)
