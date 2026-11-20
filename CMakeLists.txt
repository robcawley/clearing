cmake_minimum_required(VERSION 3.22)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
project(CLEARING VERSION 0.8.0)
set(CMAKE_CXX_STANDARD 20)

# A JUCE 8 checkout: ./JUCE if there is one, otherwise NEON RIOT's when the projects sit side by side.
if(EXISTS "${CMAKE_SOURCE_DIR}/JUCE/CMakeLists.txt")
    set(CLEARING_JUCE "${CMAKE_SOURCE_DIR}/JUCE")
else()
    set(CLEARING_JUCE "${CMAKE_SOURCE_DIR}/../NEON RIOT/JUCE")
endif()
set(JUCE_DIR "${CLEARING_JUCE}" CACHE PATH "Path to a JUCE 8 checkout")
add_subdirectory("${JUCE_DIR}" JUCE)

juce_add_plugin(Clearing COMPANY_NAME "Rob Cawley" COMPANY_WEBSITE "https://robcawley.com"
    BUNDLE_ID com.robcawley.clearing PLUGIN_MANUFACTURER_CODE Rcwl PLUGIN_CODE Clrg
    PRODUCT_NAME "CLEARING" DESCRIPTION "Automatic mix EQ" IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE NEEDS_MIDI_OUTPUT FALSE IS_MIDI_EFFECT FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect FORMATS AU COPY_PLUGIN_AFTER_BUILD TRUE VERSION 0.8.0)

juce_add_binary_data(ClearingAssets SOURCES
    Resources/Fonts/Rajdhani-SemiBold.ttf Resources/Fonts/Rajdhani-Bold.ttf
    Resources/Fonts/Rajdhani-Medium.ttf Resources/Fonts/ShareTechMono-Regular.ttf)

# The core is standard C++ only (no JUCE).
set(CORE Source/Core/Planner.cpp Source/Core/Balance.cpp Source/Core/Harmonics.cpp Source/Core/MixBalance.cpp)
set(ENGINE ${CORE} Source/Hub.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)

target_sources(Clearing PRIVATE ${ENGINE})
target_include_directories(Clearing PRIVATE Source)
target_compile_definitions(Clearing PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(Clearing PRIVATE ClearingAssets juce::juce_audio_utils PUBLIC juce::juce_recommended_config_flags)

# Renders the real editor with real instances talking through the hub, and checks the EQ takes effect.
juce_add_console_app(ClearingSnapshot PRODUCT_NAME ClearingSnapshot)
target_sources(ClearingSnapshot PRIVATE Tools/Snapshot.cpp ${ENGINE})
target_include_directories(ClearingSnapshot PRIVATE Source)
target_compile_definitions(ClearingSnapshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(ClearingSnapshot PRIVATE ClearingAssets juce::juce_audio_utils juce::juce_recommended_config_flags)

add_executable(ClearingCoreCheck Tools/CoreCheck.cpp ${CORE})
target_include_directories(ClearingCoreCheck PRIVATE Source)
add_executable(ClearingHubCheck Tools/HubCheck.cpp Source/Hub.cpp)
target_include_directories(ClearingHubCheck PRIVATE Source)

# MCP server: lets an LLM inspect and drive the CLEARING instances in a session.
juce_add_console_app(ClearingMcp PRODUCT_NAME clearing-mcp)
target_sources(ClearingMcp PRIVATE Tools/ClearingMcp.cpp Source/Hub.cpp ${CORE})
target_include_directories(ClearingMcp PRIVATE Source)
target_link_libraries(ClearingMcp PRIVATE ClearingAssets juce::juce_graphics juce::juce_recommended_config_flags)
target_compile_definitions(ClearingMcp PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
set_target_properties(ClearingMcp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" OUTPUT_NAME clearing-mcp)

# Stands in for Logic in the MCP test: real instances in a separate process.
juce_add_console_app(ClearingFakeSession PRODUCT_NAME ClearingFakeSession)
target_sources(ClearingFakeSession PRIVATE Tools/FakeSession.cpp ${ENGINE})
target_include_directories(ClearingFakeSession PRIVATE Source)
target_compile_definitions(ClearingFakeSession PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(ClearingFakeSession PRIVATE ClearingAssets juce::juce_audio_utils juce::juce_recommended_config_flags)
