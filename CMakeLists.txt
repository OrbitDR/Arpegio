cmake_minimum_required(VERSION 3.25)
project(arpegio VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Arpegio builds with MSVC for x64 only. Use scripts/build.bat.")
endif()

# Third-party sources come from scripts/fetch_deps.py.
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
set(WAVPACK_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_CMAKE_MODULE OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_PKGCONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/ogg EXCLUDE_FROM_ALL)
add_subdirectory(third_party/flac EXCLUDE_FROM_ALL)
# libvorbis looks Ogg up with find_package, so point it at the target built above.
set(OGG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ogg/include" CACHE PATH "" FORCE)
set(OGG_LIBRARY ogg CACHE STRING "" FORCE)
add_subdirectory(third_party/vorbis EXCLUDE_FROM_ALL)
add_subdirectory(third_party/wavpack EXCLUDE_FROM_ALL)

set(BUILD_BINDINGS OFF CACHE BOOL "" FORCE)
set(WITH_MATROSKA OFF CACHE BOOL "" FORCE)
set(WITH_MOD OFF CACHE BOOL "" FORCE)
set(WITH_SHORTEN OFF CACHE BOOL "" FORCE)
set(WITH_TRUEAUDIO OFF CACHE BOOL "" FORCE)
set(WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(utf8cpp_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/utfcpp/source" CACHE PATH "" FORCE)
add_subdirectory(third_party/taglib EXCLUDE_FROM_ALL)
# TagLib publishes its headers only on install, so the tree is listed here.
set(TAGLIB_INCLUDE_DIRS
    third_party/taglib/taglib
    third_party/taglib/taglib/toolkit
    third_party/taglib/taglib/mp4
    ${CMAKE_CURRENT_BINARY_DIR}/third_party/taglib)

add_library(sqlite3 STATIC EXCLUDE_FROM_ALL third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 SYSTEM INTERFACE third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE SQLITE_ENABLE_FTS5 SQLITE_DQS=0 SQLITE_OMIT_DEPRECATED)
target_compile_options(sqlite3 PRIVATE /w)

set(MAC_ROOT third_party/monkeysaudio/Source)
add_library(monkeysaudio STATIC EXCLUDE_FROM_ALL
    ${MAC_ROOT}/Shared/BufferIO.cpp
    ${MAC_ROOT}/Shared/CharacterHelper.cpp
    ${MAC_ROOT}/Shared/CircleBuffer.cpp
    ${MAC_ROOT}/Shared/CPUFeatures.cpp
    ${MAC_ROOT}/Shared/CRC.cpp
    ${MAC_ROOT}/Shared/GlobalFunctions.cpp
    ${MAC_ROOT}/Shared/MemoryIO.cpp
    ${MAC_ROOT}/Shared/Semaphore.cpp
    ${MAC_ROOT}/Shared/Thread.cpp
    ${MAC_ROOT}/Shared/WholeFileIO.cpp
    ${MAC_ROOT}/Shared/WinFileIO.cpp
    ${MAC_ROOT}/MACLib/APECompress.cpp
    ${MAC_ROOT}/MACLib/APECompressCore.cpp
    ${MAC_ROOT}/MACLib/APECompressCreate.cpp
    ${MAC_ROOT}/MACLib/APEDecompress.cpp
    ${MAC_ROOT}/MACLib/APEDecompressCore.cpp
    ${MAC_ROOT}/MACLib/APEHeader.cpp
    ${MAC_ROOT}/MACLib/APEInfo.cpp
    ${MAC_ROOT}/MACLib/APELink.cpp
    ${MAC_ROOT}/MACLib/APETag.cpp
    ${MAC_ROOT}/MACLib/BitArray.cpp
    ${MAC_ROOT}/MACLib/FloatTransform.cpp
    ${MAC_ROOT}/MACLib/MACLib.cpp
    ${MAC_ROOT}/MACLib/MACProgressHelper.cpp
    ${MAC_ROOT}/MACLib/MD5.cpp
    ${MAC_ROOT}/MACLib/NewPredictor.cpp
    ${MAC_ROOT}/MACLib/NNFilter.cpp
    ${MAC_ROOT}/MACLib/NNFilterGeneric.cpp
    ${MAC_ROOT}/MACLib/NNFilterAVX2.cpp
    ${MAC_ROOT}/MACLib/NNFilterAVX512.cpp
    ${MAC_ROOT}/MACLib/NNFilterSSE2.cpp
    ${MAC_ROOT}/MACLib/NNFilterSSE4.1.cpp
    ${MAC_ROOT}/MACLib/Prepare.cpp
    ${MAC_ROOT}/MACLib/UnBitArray.cpp
    ${MAC_ROOT}/MACLib/UnBitArrayBase.cpp
    ${MAC_ROOT}/MACLib/WAVInputSource.cpp
    ${MAC_ROOT}/MACLib/Interim.cpp
    ${MAC_ROOT}/MACLib/Old/NewPredictorOld.cpp
    ${MAC_ROOT}/MACLib/Old/AntiPredictorOld.cpp
    ${MAC_ROOT}/MACLib/Old/AntiPredictorExtraHighOld.cpp
    ${MAC_ROOT}/MACLib/Old/AntiPredictorFastOld.cpp
    ${MAC_ROOT}/MACLib/Old/AntiPredictorHighOld.cpp
    ${MAC_ROOT}/MACLib/Old/AntiPredictorNormalOld.cpp
    ${MAC_ROOT}/MACLib/Old/APEDecompressCoreOld.cpp
    ${MAC_ROOT}/MACLib/Old/APEDecompressOld.cpp
    ${MAC_ROOT}/MACLib/Old/UnBitArrayOld.cpp
    ${MAC_ROOT}/MACLib/Old/UnMACOld.cpp
)
target_include_directories(monkeysaudio SYSTEM INTERFACE third_party/monkeysaudio/Shared
    PRIVATE third_party/monkeysaudio/Source/Shared third_party/monkeysaudio/Source/MACLib third_party/monkeysaudio/Source/MACLib/Old)
target_compile_definitions(monkeysaudio PUBLIC PLATFORM_WINDOWS UNICODE _UNICODE)
target_compile_options(monkeysaudio PRIVATE /EHsc /w)

add_compile_options(/W4 /wd4324 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

add_library(arpegio_core STATIC
    src/dsd/dsd_pcm_plan.cpp
    src/dsd/dsd_fir.cpp
    src/dsd/dop.cpp
    src/dsd/dst_decoder.cpp
    src/formats/dsf_reader.cpp
    src/formats/wav_writer.cpp
    src/audio/ring_buffer.cpp
    src/audio/quantizer.cpp
    src/audio/volume.cpp
    src/decode/decoder.cpp
    src/decode/dsf_decoder.cpp
    src/decode/dff_decoder.cpp
    src/decode/framed_dsd.cpp
    src/decode/range_decoder.cpp
    src/decode/sacd_iso.cpp
    src/decode/wav_decoder.cpp
    src/decode/aiff_decoder.cpp
    src/decode/flac_decoder.cpp
    src/decode/wavpack_decoder.cpp
    src/decode/ape_decoder.cpp
    src/decode/mp3_decoder.cpp
    src/decode/vorbis_decoder.cpp
    src/decode/mf_decoder.cpp
    src/engine/render_plan.cpp
    src/engine/pipeline.cpp
    src/engine/player.cpp
    src/output/output.cpp
    src/output/sta_thread.cpp
    src/output/asio_output.cpp
    src/output/wasapi_output.cpp
    src/library/paths.cpp
    src/library/database.cpp
    src/library/library_store.cpp
    src/library/cue_sheet.cpp
    src/library/metadata.cpp
    src/library/mqa.cpp
    src/library/artwork.cpp
    src/library/scanner.cpp
    src/library/track_source.cpp
    src/library/folder_watcher.cpp
    src/library/library.cpp
)
target_include_directories(arpegio_core PUBLIC src)
target_include_directories(arpegio_core SYSTEM PRIVATE third_party/asiosdk/common third_party/r8brain third_party/minimp3 ${TAGLIB_INCLUDE_DIRS})
target_link_libraries(arpegio_core
    PUBLIC avrt ole32 advapi32 user32 uuid mfplat mfreadwrite mfuuid propsys shlwapi windowscodecs
    PRIVATE FLAC vorbisfile vorbis wavpack monkeysaudio sqlite3 tag)

add_library(arpegio_app STATIC
    src/app/controller.cpp
    src/app/settings.cpp
)
target_include_directories(arpegio_app SYSTEM PUBLIC third_party/json/single_include)
target_link_libraries(arpegio_app PUBLIC arpegio_core)

add_executable(arpegio WIN32
    src/app/main.cpp
    src/app/main_window.cpp
    src/app/arpegio.rc
)
target_include_directories(arpegio PRIVATE src/app)
target_include_directories(arpegio SYSTEM PRIVATE third_party/webview2/build/native/include)
target_link_libraries(arpegio PRIVATE arpegio_app dwmapi shell32
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/build/native/x64/WebView2LoaderStatic.lib)
# The page is served from a folder next to the executable. A target of its own, because a change that
# only touches the page does not relink anything.
add_custom_target(arpegio-ui ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/ui $<TARGET_FILE_DIR:arpegio>/ui)
add_dependencies(arpegio arpegio-ui)

add_executable(arpegio-cli cli/main.cpp)
target_link_libraries(arpegio-cli PRIVATE arpegio_core)

add_executable(arpegio-dsd2wav cli/dsd2wav.cpp)
target_link_libraries(arpegio-dsd2wav PRIVATE arpegio_core)

add_executable(arpegio-dsdgen tools/dsdgen.cpp)
target_link_libraries(arpegio-dsdgen PRIVATE arpegio_core)

add_executable(arpegio-fir-report tools/fir_report.cpp)
target_link_libraries(arpegio-fir-report PRIVATE arpegio_core)

add_executable(arpegio-sacd tools/sacd_info.cpp)
target_link_libraries(arpegio-sacd PRIVATE arpegio_core)

add_executable(arpegio-library tools/library_tool.cpp)
target_link_libraries(arpegio-library PRIVATE arpegio_core)

enable_testing()
add_executable(arpegio-tests
    tests/test_main.cpp
    tests/test_dsd_plan.cpp
    tests/test_dsd_fir.cpp
    tests/test_quantizer.cpp
    tests/test_volume.cpp
    tests/test_render_plan.cpp
    tests/test_player.cpp
    tests/test_library.cpp
    tests/test_controller.cpp
)
target_include_directories(arpegio-tests SYSTEM PRIVATE ${TAGLIB_INCLUDE_DIRS})
target_link_libraries(arpegio-tests PRIVATE arpegio_app tag FLAC)
add_test(NAME arpegio-tests COMMAND arpegio-tests)
