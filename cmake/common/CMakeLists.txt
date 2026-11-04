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
    PURESHEEN_DEV_TOOL=1)
target_link_libraries(PureSheenDev
    PRIVATE PureSheenData ${PS_JUCE_MODULES}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
set_target_properties(PureSheenDev PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/PureSheenDev_artefacts/$<CONFIG>")
