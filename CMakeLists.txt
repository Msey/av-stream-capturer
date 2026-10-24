cmake_minimum_required(VERSION 3.24)
project(conference-fx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build the x64 configuration.")
endif()

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(ORT_DIR ${CMAKE_SOURCE_DIR}/third_party/onnxruntime)

function(cfx_compile target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W3 /EHsc /utf-8)
  endif()
endfunction()

add_library(cfx_audio STATIC src/audio/pitch_shifter.cpp)
target_include_directories(cfx_audio PUBLIC ${CMAKE_SOURCE_DIR}/src)
cfx_compile(cfx_audio)

add_library(cfx_video STATIC src/video/face_effect.cpp)
target_include_directories(cfx_video PUBLIC ${CMAKE_SOURCE_DIR}/src ${ORT_DIR}/include)
target_link_libraries(cfx_video PUBLIC ${ORT_DIR}/lib/onnxruntime.lib)
cfx_compile(cfx_video)

add_library(ConferenceFxCam SHARED src/vcam/cam_filter.cpp)
target_include_directories(ConferenceFxCam PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ConferenceFxCam PRIVATE strmiids ole32 oleaut32 uuid advapi32)
cfx_compile(ConferenceFxCam)
set_target_properties(ConferenceFxCam PROPERTIES OUTPUT_NAME "ConferenceFxCam")

add_executable(conference-fx WIN32
  src/app/main.cpp
  src/capture/devices.cpp
  src/capture/audio_engine.cpp
  src/capture/camera.cpp
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(conference-fx PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
)
target_link_libraries(conference-fx PRIVATE
  cfx_audio
  cfx_video
  mfplat
  mf
  mfreadwrite
  mfuuid
  ole32
  uuid
  advapi32
  avrt
  winmm
  d3d11
  dxgi
  d3dcompiler
  strmiids
  propsys
  dwmapi
  shell32
)
target_link_options(conference-fx PRIVATE
  "/MANIFEST:EMBED"
  "/MANIFESTUAC:NO"
  "/MANIFESTINPUT:${CMAKE_SOURCE_DIR}/src/app/app.manifest"
)
cfx_compile(conference-fx)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT conference-fx)

set(ORT_DLL ${ORT_DIR}/lib/onnxruntime.dll)
set(ORT_SHARED ${ORT_DIR}/lib/onnxruntime_providers_shared.dll)

function(cfx_copy_runtime target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_DLL} $<TARGET_FILE_DIR:${target}>/onnxruntime.dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_SHARED} $<TARGET_FILE_DIR:${target}>/onnxruntime_providers_shared.dll
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${target}>/models
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/models/ultraface.onnx $<TARGET_FILE_DIR:${target}>/models/ultraface.onnx
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ConferenceFxCam> $<TARGET_FILE_DIR:${target}>/ConferenceFxCam.dll
    VERBATIM)
endfunction()

add_dependencies(conference-fx ConferenceFxCam)
cfx_copy_runtime(conference-fx)

add_executable(pitch_test tests/pitch_test.cpp)
target_link_libraries(pitch_test PRIVATE cfx_audio)
cfx_compile(pitch_test)

add_executable(pupil_test tests/pupil_test.cpp)
target_include_directories(pupil_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
cfx_compile(pupil_test)

add_executable(face_test tests/face_test.cpp)
target_link_libraries(face_test PRIVATE cfx_video)
target_include_directories(face_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
cfx_compile(face_test)
cfx_copy_runtime(face_test)

add_executable(vcam_smoke tests/vcam_smoke.cpp)
target_link_libraries(vcam_smoke PRIVATE ole32 oleaut32 uuid strmiids)
cfx_compile(vcam_smoke)
add_dependencies(vcam_smoke ConferenceFxCam)
add_custom_command(TARGET vcam_smoke POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ConferenceFxCam> $<TARGET_FILE_DIR:vcam_smoke>/ConferenceFxCam.dll
  VERBATIM)

set(CFX_CACHE "${CMAKE_SOURCE_DIR}/third_party/cache")
set(SHERPA_ROOT "${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx")
set(ASR_ROOT "${CMAKE_SOURCE_DIR}/models/asr")
set(TTS_ROOT "${CMAKE_SOURCE_DIR}/models/tts")

function(cfx_extract_once archive destination marker)
  if(EXISTS "${destination}/${marker}")
    return()
  endif()
  if(NOT EXISTS "${archive}")
    message(FATAL_ERROR "Missing archive ${archive}")
  endif()
  file(MAKE_DIRECTORY "${destination}")
  message(STATUS "Extracting ${archive}")
  file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${destination}")
  file(WRITE "${destination}/${marker}" "ok\n")
endfunction()

cfx_extract_once(
  "${CFX_CACHE}/sherpa-onnx-v1.13.8-win-x64-shared-MT-Release.tar.bz2"
  "${SHERPA_ROOT}" ".extracted")
cfx_extract_once(
  "${CFX_CACHE}/sherpa-onnx-streaming-t-one-russian-2025-09-08.tar.bz2"
  "${ASR_ROOT}" ".tone")
cfx_extract_once(
  "${CFX_CACHE}/sherpa-onnx-nemo-ctc-punct-giga-am-v3-russian-2025-12-16.tar.bz2"
  "${ASR_ROOT}" ".giga")
cfx_extract_once(
  "${CFX_CACHE}/vits-piper-ru_RU-ruslan-medium.tar.bz2"
  "${TTS_ROOT}" ".ruslan")

file(GLOB_RECURSE SHERPA_LIB "${SHERPA_ROOT}/sherpa-onnx-c-api.lib")
file(GLOB_RECURSE SHERPA_HEADER "${SHERPA_ROOT}/c-api.h")
if(NOT SHERPA_LIB OR NOT SHERPA_HEADER)
  message(FATAL_ERROR "sherpa-onnx C API was not found after extraction")
endif()
list(GET SHERPA_LIB 0 SHERPA_LIB)
list(GET SHERPA_HEADER 0 SHERPA_HEADER)
get_filename_component(SHERPA_CAPI_DIR "${SHERPA_HEADER}" DIRECTORY)
get_filename_component(SHERPA_ONNX_DIR "${SHERPA_CAPI_DIR}" DIRECTORY)
get_filename_component(SHERPA_INCLUDE "${SHERPA_ONNX_DIR}" DIRECTORY)
get_filename_component(SHERPA_LIB_DIR "${SHERPA_LIB}" DIRECTORY)
file(GLOB SHERPA_DLLS "${SHERPA_LIB_DIR}/../bin/*.dll" "${SHERPA_LIB_DIR}/*.dll")

add_library(cfx_captions STATIC
  src/captions/model_files.cpp
  src/captions/recognizers.cpp
  src/captions/caption_engine.cpp
  src/capture/app_loopback.cpp
)
target_include_directories(cfx_captions PUBLIC ${CMAKE_SOURCE_DIR}/src ${SHERPA_INCLUDE})
target_compile_definitions(cfx_captions PRIVATE SHERPA_ONNX_BUILD_SHARED_LIBS)
target_link_libraries(cfx_captions PUBLIC ${SHERPA_LIB} ole32 uuid mmdevapi)
cfx_compile(cfx_captions)

target_sources(conference-fx PRIVATE src/captions/speech_player.cpp)
target_link_libraries(conference-fx PRIVATE cfx_captions mmdevapi)

function(cfx_copy_sherpa_dlls target)
  foreach(dll ${SHERPA_DLLS})
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" $<TARGET_FILE_DIR:${target}>
      VERBATIM)
  endforeach()
endfunction()

function(cfx_copy_speech_models target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      -DSRC=${ASR_ROOT}
      -DDST=$<TARGET_FILE_DIR:${target}>/models/asr
      -DSTAMP=${ASR_ROOT}/.giga
      -P ${CMAKE_SOURCE_DIR}/cmake/copy_tree_once.cmake
    COMMAND ${CMAKE_COMMAND}
      -DSRC=${TTS_ROOT}
      -DDST=$<TARGET_FILE_DIR:${target}>/models/tts
      -DSTAMP=${TTS_ROOT}/.ruslan
      -P ${CMAKE_SOURCE_DIR}/cmake/copy_tree_once.cmake
    VERBATIM)
endfunction()

cfx_copy_sherpa_dlls(conference-fx)
cfx_copy_speech_models(conference-fx)
cfx_copy_sherpa_dlls(face_test)

add_executable(caption_test tests/caption_test.cpp)
target_link_libraries(caption_test PRIVATE cfx_captions)
cfx_compile(caption_test)
cfx_copy_sherpa_dlls(caption_test)
cfx_copy_speech_models(caption_test)

add_executable(speech_test tests/speech_test.cpp)
target_link_libraries(speech_test PRIVATE cfx_captions)
cfx_compile(speech_test)
cfx_copy_sherpa_dlls(speech_test)
cfx_copy_speech_models(speech_test)
