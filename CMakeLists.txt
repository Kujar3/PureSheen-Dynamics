cmake_minimum_required(VERSION 3.22)

project(PureSheen VERSION 0.3.8 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(MSVC)
    add_compile_options(/MP /utf-8)
endif()

set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE" CACHE PATH "Path to the JUCE checkout")
add_subdirectory("${JUCE_PATH}" JUCE EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Source lists (globbed per folder so new files are picked up on the next configure)
#   src/core  measurement core reused from RefMatch (loudness, true peak, FFT)  - pure C++17
#   src/dsp   the DSP modules and the chain host                                 - pure C++17
#   src/plugin, src/ui                                                            - JUCE
# ---------------------------------------------------------------------------
file(GLOB PS_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/*.cpp")
file(GLOB PS_PLUGIN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/import/*.cpp")
# The preset importer's pure C++ part (src/plugin/import/Import*.cpp: engine + mapping tables, no JUCE) is also unit
# tested by CoreTests; XmlPresetImport.cpp (juce::XmlDocument, processor, file chooser) is plug-in / dev tool only.
file(GLOB PS_IMPORT_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/import/Import*.cpp")
file(GLOB PS_UI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp")
file(GLOB PS_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")

# ---------------------------------------------------------------------------
# Embedded resources (fonts, logo)
# ---------------------------------------------------------------------------
file(GLOB PS_RESOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/*.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/images/*.png")
juce_add_binary_data(PureSheenData
    NAMESPACE PureSheenBinary
    HEADER_NAME PureSheenBinary.h
    SOURCES ${PS_RESOURCE_FILES})
set_target_properties(PureSheenData PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(PS_JUCE_MODULES
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_gui_extra)

set(PS_JUCE_DEFINES
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1)

set(PS_ICON_ARGS)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/logo_512.png")
    set(PS_ICON_ARGS ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/logo_512.png")
endif()

# ---------------------------------------------------------------------------
# The plugin (VST3 + Standalone)
# ---------------------------------------------------------------------------
juce_add_plugin(PureSheen
    COMPANY_NAME "Kujar3"
    PRODUCT_NAME "PureSheen Dynamics"
    BUNDLE_ID "com.kujar3.puresheen"
    PLUGIN_MANUFACTURER_CODE Kuja
    PLUGIN_CODE Pshd
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx Mastering Dynamics EQ
    ${PS_ICON_ARGS}
    COPY_PLUGIN_AFTER_BUILD FALSE)

# JUCE regenerates the Windows version resource only when the icon changes, so a version bump would keep the old
# file version in the binaries' properties. Make it depend on the info file that carries the version too.
if(WIN32)
    get_target_property(PS_JUCE_CODE_DIR PureSheen JUCE_GENERATED_SOURCES_DIRECTORY)
    get_target_property(PS_JUCE_INFO_FILE PureSheen JUCE_INFO_FILE)
    add_custom_command(OUTPUT "${PS_JUCE_CODE_DIR}/PureSheen_resources.rc" APPEND DEPENDS "${PS_JUCE_INFO_FILE}")
endif()

target_sources(PureSheen PRIVATE ${PS_CORE_SOURCES} ${PS_PLUGIN_SOURCES} ${PS_UI_SOURCES})
target_include_directories(PureSheen PRIVATE src)
target_compile_definitions(PureSheen PUBLIC ${PS_JUCE_DEFINES})
target_link_libraries(PureSheen
    PRIVATE PureSheenData ${PS_JUCE_MODULES}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Pure C++ core + DSP unit tests (no JUCE, compiles in seconds)
#   CoreTests [substring-filter]
# ---------------------------------------------------------------------------
add_executable(CoreTests ${PS_TEST_SOURCES} ${PS_CORE_SOURCES} ${PS_IMPORT_CORE_SOURCES})
target_include_directories(CoreTests PRIVATE src tests)
if(MSVC)
    target_compile_options(CoreTests PRIVATE /W4 /permissive-)
endif()

# ---------------------------------------------------------------------------
# Dev tool: processor self-test, editor snapshots, offline rendering
#   PureSheenDev selftest
#   PureSheenDev snapshot <outDir>
#   PureSheenDev render <in.wav> <out.wav>
#   PureSheenDev icon <out.png> [size]
# ---------------------------------------------------------------------------
juce_add_console_app(PureSheenDev PRODUCT_NAME "PureSheenDev")
file(GLOB PS_DEV_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/dev/*.cpp")
target_sources(PureSheenDev PRIVATE ${PS_DEV_SOURCES} ${PS_CORE_SOURCES} ${PS_PLUGIN_SOURCES} ${PS_UI_SOURCES})
target_include_directories(PureSheenDev PRIVATE src)
target_compile_definitions(PureSheenDev PRIVATE
    ${PS_JUCE_DEFINES}
    JucePlugin_Name="PureSheen Dynamics"
    PURESHEEN_DEV_TOOL=1)
target_link_libraries(PureSheenDev
    PRIVATE PureSheenData ${PS_JUCE_MODULES}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Optional CLAP format (clap-juce-extensions, scripts\fetch_clap.bat)
#   scripts\build.bat build-clap PureSheen_CLAP
# ---------------------------------------------------------------------------
option(PURESHEEN_CLAP "Build the CLAP format (needs third_party/clap-juce-extensions)" ON)
set(PURESHEEN_CLAP_EXT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/clap-juce-extensions"
    CACHE PATH "Path to the clap-juce-extensions checkout (with submodules)")
if(PURESHEEN_CLAP)
    if(EXISTS "${PURESHEEN_CLAP_EXT_PATH}/CMakeLists.txt"
       AND EXISTS "${PURESHEEN_CLAP_EXT_PATH}/clap-libs/clap/CMakeLists.txt"
       AND EXISTS "${PURESHEEN_CLAP_EXT_PATH}/clap-libs/clap-helpers/CMakeLists.txt")
        add_subdirectory("${PURESHEEN_CLAP_EXT_PATH}" clap-juce-extensions EXCLUDE_FROM_ALL)
        clap_juce_extensions_plugin(TARGET PureSheen
            CLAP_ID "com.kujar3.puresheen"
            CLAP_FEATURES audio-effect mastering equalizer compressor limiter stereo)
        message(STATUS "PureSheen: CLAP enabled (target PureSheen_CLAP)")
    else()
        message(STATUS "PureSheen: CLAP disabled, ${PURESHEEN_CLAP_EXT_PATH} not found")
    endif()
endif()
