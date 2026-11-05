cmake_minimum_required(VERSION 3.22)

project(PureSheen VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(MSVC)
    add_compile_options(/MP /utf-8)
endif()

# Link-time-optimised links in parallel can crash the MSVC linker (C1001), and each one uses many threads by itself.
set_property(GLOBAL PROPERTY JOB_POOLS ps_link=3)
set(CMAKE_JOB_POOL_LINK ps_link)

set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE" CACHE PATH "Path to the JUCE checkout")
add_subdirectory("${JUCE_PATH}" JUCE EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Source lists (globbed per folder so new files are picked up on the next configure)
#   src/core  measurement core reused from RefMatch (loudness, true peak, FFT)  - pure C++17
#   src/dsp   the DSP modules and the chain host                                 - pure C++17
#   src/assist the Assistant (analysis features, suggestion engine, offline render) - pure C++17
#   src/plugin, src/ui                                                            - JUCE
#   src/entry the per-plugin entry point (compiled once per plugin, not in the shared library)
# ---------------------------------------------------------------------------
file(GLOB PS_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assist/*.cpp")
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
# The product list (installer/products.csv, also read by scripts/package-all.ps1): the suite and the single-module
# plugins. Columns: id, name, code, module, vst3_categories, clap_features, installer_guid, description.
# ---------------------------------------------------------------------------
set(PS_PRODUCTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/installer/products.csv")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS_PRODUCTS_FILE}")
file(STRINGS "${PS_PRODUCTS_FILE}" PS_PRODUCT_LINES)
list(POP_FRONT PS_PRODUCT_LINES)

set(PS_SEEN_IDS)
set(PS_SEEN_NAMES)
set(PS_SEEN_CODES)
set(PS_SEEN_GUIDS)
set(PS_PRODUCT_TABLE "")
foreach(line IN LISTS PS_PRODUCT_LINES)
    string(REPLACE "," ";" fields "${line}")
    list(GET fields 0 id)
    list(GET fields 1 name)
    list(GET fields 2 code)
    list(GET fields 3 module)
    list(GET fields 6 guid)
    if(id IN_LIST PS_SEEN_IDS OR name IN_LIST PS_SEEN_NAMES OR code IN_LIST PS_SEEN_CODES OR guid IN_LIST PS_SEEN_GUIDS)
        message(FATAL_ERROR "installer/products.csv: '${id}' repeats an id, name, plugin code or installer id")
    endif()
    list(APPEND PS_SEEN_IDS "${id}")
    list(APPEND PS_SEEN_NAMES "${name}")
    list(APPEND PS_SEEN_CODES "${code}")
    list(APPEND PS_SEEN_GUIDS "${guid}")
    if(module STREQUAL "-")
        string(APPEND PS_PRODUCT_TABLE "{ \"${id}\", \"${name}\", ProductKind::suite, ps::ModuleType::Equalizer1 },\n")
    elseif(module STREQUAL "Dither")
        string(APPEND PS_PRODUCT_TABLE "{ \"${id}\", \"${name}\", ProductKind::dither, ps::ModuleType::Equalizer1 },\n")
    else()
        string(APPEND PS_PRODUCT_TABLE "{ \"${id}\", \"${name}\", ProductKind::module, ps::ModuleType::${module} },\n")
    endif()
endforeach()

set(PS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(CONFIGURE OUTPUT "${PS_GENERATED_DIR}/ProductTable.inc" CONTENT "${PS_PRODUCT_TABLE}" @ONLY)

# ---------------------------------------------------------------------------
# The shared library and the dev tool: see cmake/common/CMakeLists.txt. JUCE's module sources compile there, and only
# there: in this directory they are headers, so a plugin compiles nothing but its entry point and its format wrappers.
# ---------------------------------------------------------------------------
add_subdirectory(cmake/common common)

foreach(module IN ITEMS juce_core juce_data_structures juce_events juce_graphics juce_gui_basics juce_gui_extra
                        juce_audio_basics juce_audio_devices juce_audio_formats juce_audio_processors
                        juce_audio_processors_headless juce_audio_utils)
    get_target_property(module_sources ${module} INTERFACE_JUCE_MODULE_SOURCES)
    set_source_files_properties(${module_sources} PROPERTIES HEADER_FILE_ONLY TRUE)
endforeach()

# ---------------------------------------------------------------------------
# Optional CLAP format (clap-juce-extensions, scripts\fetch_clap.bat)
#   scripts\build.bat build-clap PureSheen_CLAP
# ---------------------------------------------------------------------------
option(PURESHEEN_CLAP "Build the CLAP format (needs third_party/clap-juce-extensions)" ON)
set(PURESHEEN_CLAP_EXT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/clap-juce-extensions"
    CACHE PATH "Path to the clap-juce-extensions checkout (with submodules)")
set(PS_HAS_CLAP OFF)
if(PURESHEEN_CLAP)
    if(EXISTS "${PURESHEEN_CLAP_EXT_PATH}/CMakeLists.txt"
       AND EXISTS "${PURESHEEN_CLAP_EXT_PATH}/clap-libs/clap/CMakeLists.txt"
       AND EXISTS "${PURESHEEN_CLAP_EXT_PATH}/clap-libs/clap-helpers/CMakeLists.txt")
        add_subdirectory("${PURESHEEN_CLAP_EXT_PATH}" clap-juce-extensions EXCLUDE_FROM_ALL)
        set(PS_HAS_CLAP ON)
        message(STATUS "PureSheen: CLAP enabled")
    else()
        message(STATUS "PureSheen: CLAP disabled, ${PURESHEEN_CLAP_EXT_PATH} not found")
    endif()
endif()

# ---------------------------------------------------------------------------
# One plugin: juce_add_plugin + the entry point + the format wrappers, on top of PureSheenCommon.
# ---------------------------------------------------------------------------
function(ps_add_plugin target id name code formats vst3_categories clap_features)
    separate_arguments(vst3_categories)
    separate_arguments(clap_features)
    # FL Studio names its plugin section after the whole category ("Fx EQ" = Fx-EQ), so a third word would make a new
    # section of its own.
    list(LENGTH vst3_categories category_count)
    if(NOT category_count EQUAL 2)
        message(FATAL_ERROR "installer/products.csv: '${id}' needs exactly two VST3 categories, like 'Fx EQ'")
    endif()
    if(id STREQUAL "suite")
        set(bundle_id "com.kujar3.puresheen")
    else()
        set(bundle_id "com.kujar3.puresheen.${id}")
    endif()

    juce_add_plugin(${target}
        COMPANY_NAME "Kujar3"
        PRODUCT_NAME "${name}"
        BUNDLE_ID "${bundle_id}"
        PLUGIN_MANUFACTURER_CODE Kuja
        PLUGIN_CODE ${code}
        FORMATS ${formats}
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        VST3_CATEGORIES ${vst3_categories}
        # No moduleinfo.json: FL Studio records no category for a bundle that has one, and leaves the plugin unsorted.
        VST3_AUTO_MANIFEST FALSE
        ${PS_ICON_ARGS}
        COPY_PLUGIN_AFTER_BUILD FALSE)

    # A build folder from before that change still has the file, and the bundle would carry it along.
    add_custom_command(TARGET ${target}_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_CURRENT_BINARY_DIR}/${target}_artefacts/$<CONFIG>/VST3/${name}.vst3/Contents/Resources"
        VERBATIM)

    # JUCE regenerates the Windows version resource only when the icon changes, so a version bump would keep the old
    # file version in the binaries' properties. Make it depend on the info file that carries the version too.
    if(WIN32)
        get_target_property(code_dir ${target} JUCE_GENERATED_SOURCES_DIRECTORY)
        get_target_property(info_file ${target} JUCE_INFO_FILE)
        add_custom_command(OUTPUT "${code_dir}/${target}_resources.rc" APPEND DEPENDS "${info_file}")
    endif()

    target_sources(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/entry/PluginEntry.cpp")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${PS_GENERATED_DIR}")
    target_compile_definitions(${target}
        PUBLIC ${PS_JUCE_DEFINES}
        PRIVATE PS_PRODUCT_ID="${id}")
    target_link_libraries(${target}
        PRIVATE PureSheenCommon ${PS_JUCE_MODULES}
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

    if(PS_HAS_CLAP)
        clap_juce_extensions_plugin(TARGET ${target}
            CLAP_ID "${bundle_id}"
            CLAP_FEATURES ${clap_features})
    endif()
endfunction()

# ---------------------------------------------------------------------------
# The plugins: the suite (VST3, CLAP, Standalone) and the single-module plugins (VST3, CLAP). The single-module plugins
# are not part of "all"; build them with the target PureSheenSingles (or PureSheen_<id>_VST3 / _CLAP, one at a time).
# ---------------------------------------------------------------------------
add_custom_target(PureSheenSingles)
foreach(line IN LISTS PS_PRODUCT_LINES)
    string(REPLACE "," ";" fields "${line}")
    list(GET fields 0 id)
    list(GET fields 1 name)
    list(GET fields 2 code)
    list(GET fields 4 vst3_categories)
    list(GET fields 5 clap_features)
    if(id STREQUAL "suite")
        ps_add_plugin(PureSheen "${id}" "${name}" "${code}" "VST3;Standalone" "${vst3_categories}" "${clap_features}")
    else()
        set(target "PureSheen_${id}")
        ps_add_plugin(${target} "${id}" "${name}" "${code}" "VST3" "${vst3_categories}" "${clap_features}")
        set_target_properties(${target} ${target}_VST3 PROPERTIES EXCLUDE_FROM_ALL TRUE)
        add_dependencies(PureSheenSingles ${target}_VST3)
        if(PS_HAS_CLAP)
            set_target_properties(${target}_CLAP PROPERTIES EXCLUDE_FROM_ALL TRUE)
            add_dependencies(PureSheenSingles ${target}_CLAP)
        endif()
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Pure C++ core + DSP unit tests (no JUCE, compiles in seconds)
#   CoreTests [substring-filter]
# ---------------------------------------------------------------------------
add_executable(CoreTests ${PS_TEST_SOURCES} ${PS_CORE_SOURCES} ${PS_IMPORT_CORE_SOURCES})
target_include_directories(CoreTests PRIVATE src tests)
if(MSVC)
    target_compile_options(CoreTests PRIVATE /W4 /permissive-)
endif()
