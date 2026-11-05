# Targets that compile JUCE's module sources: the shared library and the dev tool. They live in this subdirectory
# because source file properties are per directory: the top directory marks the JUCE sources HEADER_FILE_ONLY, so the
# plugins declared there (the suite and every single-module plugin) do not compile them again.

# ---------------------------------------------------------------------------
# Everything the plugins share, compiled once: core, DSP, processor, UI, JUCE.
# ---------------------------------------------------------------------------
add_library(PureSheenCommon STATIC ${PS_CORE_SOURCES} ${PS_PLUGIN_SOURCES} ${PS_UI_SOURCES})
target_include_directories(PureSheenCommon PUBLIC "${PROJECT_SOURCE_DIR}/src" "${PS_GENERATED_DIR}")
target_compile_definitions(PureSheenCommon
    PUBLIC ${PS_JUCE_DEFINES}
    # Read by module code compiled here; the per-plugin values only matter in the format wrappers.
    PRIVATE JucePlugin_Name="PureSheen" JucePlugin_Build_Standalone=0 JucePlugin_Enable_ARA=0)
target_link_libraries(PureSheenCommon
    PRIVATE PureSheenData
    # juce_audio_plugin_client has no sources of its own here: juce_gui_basics checks that it is available.
    PUBLIC ${PS_JUCE_MODULES} juce::juce_audio_plugin_client
           juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
set_target_properties(PureSheenCommon PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(PureSheenCommon PUBLIC PURESHEEN_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# The helper Claude starts to reach the suite (tools/bridge/BridgeMain.cpp): an MCP server over stdin / stdout. juce_core
# only, so it stays small.
# ---------------------------------------------------------------------------
juce_add_console_app(PureSheenBridge PRODUCT_NAME "PureSheenBridge")
target_sources(PureSheenBridge PRIVATE "${PROJECT_SOURCE_DIR}/tools/bridge/BridgeMain.cpp")
target_include_directories(PureSheenBridge PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(PureSheenBridge PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    PURESHEEN_VERSION="${PROJECT_VERSION}")
target_link_libraries(PureSheenBridge
    PRIVATE juce::juce_core
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
set_target_properties(PureSheenBridge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/PureSheenBridge_artefacts/$<CONFIG>")

# ---------------------------------------------------------------------------
# Dev tool: processor self-test, editor snapshots, offline rendering
#   PureSheenDev selftest
#   PureSheenDev snapshot <outDir>
#   PureSheenDev render <in.wav> <out.wav>
#   PureSheenDev icon <out.png> [size]
# ---------------------------------------------------------------------------
juce_add_console_app(PureSheenDev PRODUCT_NAME "PureSheenDev")
file(GLOB PS_DEV_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tools/dev/*.cpp")
target_sources(PureSheenDev PRIVATE ${PS_DEV_SOURCES} ${PS_CORE_SOURCES} ${PS_PLUGIN_SOURCES} ${PS_UI_SOURCES})
target_include_directories(PureSheenDev PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PS_GENERATED_DIR}")
target_compile_definitions(PureSheenDev PRIVATE
    ${PS_JUCE_DEFINES}
    JucePlugin_Name="PureSheen Dynamics"
    PURESHEEN_VERSION="${PROJECT_VERSION}"
    PURESHEEN_DEV_TOOL=1)
target_link_libraries(PureSheenDev
    PRIVATE PureSheenData ${PS_JUCE_MODULES}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
# JUCE regenerates the Windows version resource only when the icon changes: without this, a version bump would keep the
# old file version in these two programs' properties (the plugin targets have the same line).
if(WIN32)
    foreach(app PureSheenBridge PureSheenDev)
        get_target_property(code_dir ${app} JUCE_GENERATED_SOURCES_DIRECTORY)
        get_target_property(info_file ${app} JUCE_INFO_FILE)
        add_custom_command(OUTPUT "${code_dir}/${app}_resources.rc" APPEND DEPENDS "${info_file}")
    endforeach()
endif()

set_target_properties(PureSheenDev PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/PureSheenDev_artefacts/$<CONFIG>")
