cmake_minimum_required(VERSION 3.20)

project(voice-studio VERSION 1.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(VOICE_STUDIO_BUILD_PLUGIN "Собирать плагин для OBS" ON)
option(VOICE_STUDIO_BUILD_TESTS "Собирать проверки (офлайн, libobs, просмотр панели)" ON)
# Откуда плагин берёт обновления (страница проекта на GitHub). Пусто — без автообновления.
set(VOICE_STUDIO_UPDATE_REPO "Agrysif/obs-voice-studio" CACHE STRING "GitHub-репозиторий для автообновления (владелец/имя)")

# --- RNNoise 0.2 (нейросетевое шумоподавление, лицензия BSD-3) --------------
set(RNN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise")
add_library(rnnoise STATIC
  "${RNN_DIR}/src/denoise.c"
  "${RNN_DIR}/src/rnn.c"
  "${RNN_DIR}/src/pitch.c"
  "${RNN_DIR}/src/kiss_fft.c"
  "${RNN_DIR}/src/celt_lpc.c"
  "${RNN_DIR}/src/nnet.c"
  "${RNN_DIR}/src/nnet_default.c"
  "${RNN_DIR}/src/parse_lpcnet_weights.c"
  "${RNN_DIR}/src/rnnoise_data.c"
  "${RNN_DIR}/src/rnnoise_tables.c"
  "${RNN_DIR}/src/x86/x86_dnn_map.c"
  "${RNN_DIR}/src/x86/x86cpu.c"
  "${RNN_DIR}/src/x86/nnet_sse4_1.c"
  "${RNN_DIR}/src/x86/nnet_avx2.c")
target_include_directories(rnnoise
  PUBLIC "${RNN_DIR}/include"
  PRIVATE "${RNN_DIR}/src")
# Выбор SSE4.1/AVX2 во время работы: плагин запустится на любом x64-процессоре
# и будет использовать самые быстрые инструкции, которые тот поддерживает.
target_compile_definitions(rnnoise PRIVATE RNN_ENABLE_X86_RTCD _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  # MSVC не объявляет __SSE2__ для x64, хотя SSE2 там есть всегда.
  target_compile_definitions(rnnoise PRIVATE __SSE2__)
  target_compile_options(rnnoise PRIVATE /W0 /MP)
  set_source_files_properties("${RNN_DIR}/src/x86/nnet_sse4_1.c" PROPERTIES COMPILE_DEFINITIONS "__SSE4_1__")
  set_source_files_properties("${RNN_DIR}/src/x86/nnet_avx2.c" PROPERTIES
    COMPILE_OPTIONS "/arch:AVX2"
    COMPILE_DEFINITIONS "__FMA__")
else()
  target_compile_options(rnnoise PRIVATE -w)
  set_source_files_properties("${RNN_DIR}/src/x86/nnet_sse4_1.c" PROPERTIES COMPILE_OPTIONS "-msse4.1")
  set_source_files_properties("${RNN_DIR}/src/x86/nnet_avx2.c" PROPERTIES COMPILE_OPTIONS "-mavx;-mavx2;-mfma")
endif()
set_target_properties(rnnoise PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- DSP-ядро (не зависит от OBS) --------------------------------------------
add_library(voice-studio-dsp STATIC
  src/dsp/analyzer.cpp
  src/dsp/denoiser.cpp
  src/dsp/pitch-shifter.cpp
  src/dsp/voice-chain.cpp)
target_include_directories(voice-studio-dsp PUBLIC src/dsp)
target_link_libraries(voice-studio-dsp PUBLIC rnnoise)
if(MSVC)
  target_compile_options(voice-studio-dsp PRIVATE /W3 /utf-8 /MP /fp:fast)
  target_compile_definitions(voice-studio-dsp PRIVATE NOMINMAX _USE_MATH_DEFINES)
endif()
set_target_properties(voice-studio-dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- Ядро автообновления: HTTPS (WinHTTP), подпись ECDSA (BCrypt), пакет файлов ------
add_library(voice-studio-update-core STATIC src/update/update-core.cpp)
target_include_directories(voice-studio-update-core PUBLIC src/update)
target_link_libraries(voice-studio-update-core PUBLIC winhttp bcrypt)
if(MSVC)
  target_compile_options(voice-studio-update-core PRIVATE /W3 /utf-8)
  target_compile_definitions(voice-studio-update-core PRIVATE NOMINMAX UNICODE _UNICODE)
endif()

if(VOICE_STUDIO_BUILD_PLUGIN)
  include("${CMAKE_CURRENT_SOURCE_DIR}/../_shared/cmake/ObsPluginCommon.cmake")
  obs_find_qt()

  # --- Стили и ключи настроек (нужен libobs: obs_data) ------------------------
  add_library(voice-studio-core STATIC src/presets.cpp)
  target_include_directories(voice-studio-core PUBLIC src)
  target_link_libraries(voice-studio-core PUBLIC OBS::libobs)
  if(MSVC)
    target_compile_options(voice-studio-core PRIVATE /W3 /utf-8)
  endif()

  # --- Живой блок в окне фильтра (Qt, без зависимостей от окна OBS) ------------
  add_library(voice-studio-ui STATIC
    src/ui/eq-view.cpp
    src/ui/live-block.cpp
    src/ui/props-injector.cpp)
  target_include_directories(voice-studio-ui PUBLIC src/ui)
  target_link_libraries(voice-studio-ui PUBLIC voice-studio-core voice-studio-dsp Qt6::Core Qt6::Gui Qt6::Widgets)
  target_compile_definitions(voice-studio-ui PUBLIC QT_NO_KEYWORDS NOMINMAX)
  if(MSVC)
    target_compile_options(voice-studio-ui PRIVATE /W3 /utf-8 /MP)
  endif()

  # --- Плагин -------------------------------------------------------------------
  add_library(voice-studio MODULE
    src/plugin-main.cpp
    src/voice-filter.cpp
    src/ui/obs-live.cpp
    src/update/updater.cpp)
  target_link_libraries(voice-studio PRIVATE voice-studio-ui voice-studio-core voice-studio-dsp
    voice-studio-update-core OBS::frontend-api)
  target_compile_definitions(voice-studio PRIVATE
    VOICE_STUDIO_VERSION="${PROJECT_VERSION}"
    VOICE_STUDIO_UPDATE_REPO="${VOICE_STUDIO_UPDATE_REPO}")
  obs_plugin_setup(voice-studio)

  if(VOICE_STUDIO_BUILD_TESTS)
    # Интеграционный тест: настоящий libobs, но без окна OBS и без вывода звука.
    add_executable(voice-studio-obs-test tests/obs-host-test.cpp)
    target_include_directories(voice-studio-obs-test PRIVATE src)
    target_link_libraries(voice-studio-obs-test PRIVATE OBS::libobs Qt6::Widgets $<$<PLATFORM_ID:Windows>:shell32>)

    # Просмотр живого блока в макете окна фильтра с «игрушечными» данными: окно или скриншот.
    add_executable(voice-studio-ui-preview tests/ui-preview.cpp)
    target_link_libraries(voice-studio-ui-preview PRIVATE voice-studio-ui $<$<PLATFORM_ID:Windows>:shell32>)
    target_compile_definitions(voice-studio-ui-preview PRIVATE VOICE_STUDIO_UPDATE_REPO="${VOICE_STUDIO_UPDATE_REPO}")

    if(MSVC)
      foreach(t voice-studio-obs-test voice-studio-ui-preview)
        target_compile_options(${t} PRIVATE /W3 /utf-8)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
      endforeach()
    endif()
  endif()
endif()

# --- Офлайн-тест DSP и инструмент обновлений --------------------------------------
if(VOICE_STUDIO_BUILD_TESTS)
  add_executable(voice-studio-test tests/offline-test.cpp)
  target_link_libraries(voice-studio-test PRIVATE voice-studio-dsp)

  # Самопроверка автообновления, сборка пакета для выпуска и проверка выпуска на GitHub.
  add_executable(voice-studio-update-tool tests/update-tool.cpp)
  target_link_libraries(voice-studio-update-tool PRIVATE voice-studio-update-core shell32)

  if(MSVC)
    foreach(t voice-studio-test voice-studio-update-tool)
      target_compile_options(${t} PRIVATE /W3 /utf-8)
      target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    endforeach()
  endif()
endif()
