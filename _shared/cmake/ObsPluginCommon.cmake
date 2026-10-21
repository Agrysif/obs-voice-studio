# Общие настройки сборки для всех плагинов в папке "Плагины OBS".
#
# Подключение в CMakeLists.txt плагина:
#     include(${CMAKE_CURRENT_SOURCE_DIR}/../_shared/cmake/ObsPluginCommon.cmake)
#     add_library(my-plugin MODULE ...)
#     obs_plugin_setup(my-plugin)
#
# SDK (заголовки libobs + obs.lib) готовит _shared/scripts/setup-obs-sdk.ps1.

get_filename_component(OBS_PLUGINS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(OBS_SDK_ROOT "${OBS_PLUGINS_ROOT}/_sdk" CACHE PATH "Папка с подготовленным SDK OBS")

if(EXISTS "${OBS_SDK_ROOT}/current.cmake")
  include("${OBS_SDK_ROOT}/current.cmake")
endif()

if(NOT OBS_SDK_VERSION)
  message(FATAL_ERROR
    "SDK OBS не подготовлен. Запустите один раз:\n"
    "  powershell -ExecutionPolicy Bypass -File \"${OBS_PLUGINS_ROOT}/_shared/scripts/setup-obs-sdk.ps1\"")
endif()

set(OBS_SOURCE_DIR "${OBS_SDK_ROOT}/obs-studio-${OBS_SDK_VERSION}")
set(OBS_SDK_DIR "${OBS_SDK_ROOT}/obs-${OBS_SDK_VERSION}")

if(NOT TARGET OBS::libobs)
  add_library(OBS::libobs INTERFACE IMPORTED)
  set_target_properties(OBS::libobs PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/libobs;${OBS_SDK_DIR}/include"
    INTERFACE_LINK_LIBRARIES "${OBS_SDK_DIR}/lib/obs.lib")
endif()

# API главного окна OBS: доки, меню, события.
if(NOT TARGET OBS::frontend-api AND EXISTS "${OBS_SDK_DIR}/lib/obs-frontend-api.lib")
  add_library(OBS::frontend-api INTERFACE IMPORTED)
  set_target_properties(OBS::frontend-api PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/frontend/api"
    INTERFACE_LINK_LIBRARIES "${OBS_SDK_DIR}/lib/obs-frontend-api.lib;OBS::libobs")
endif()

# Qt — ровно та сборка, что внутри OBS (готовит setup-obs-sdk.ps1 -WithQt).
macro(obs_find_qt)
  if(NOT OBS_QT6_DIR)
    message(FATAL_ERROR
      "Qt для OBS не подготовлен. Запустите:\n"
      "  powershell -ExecutionPolicy Bypass -File \"${OBS_PLUGINS_ROOT}/_shared/scripts/setup-obs-sdk.ps1\" -WithQt")
  endif()
  list(APPEND CMAKE_PREFIX_PATH "${OBS_QT6_DIR}")
  find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
endmacro()

# По умолчанию ставим плагины туда, где их ищет OBS 28+ без прав администратора:
# %ProgramData%\obs-studio\plugins\<имя>\bin\64bit и ...\<имя>\data
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  file(TO_CMAKE_PATH "$ENV{ProgramData}/obs-studio/plugins" _obs_default_prefix)
  set(CMAKE_INSTALL_PREFIX "${_obs_default_prefix}" CACHE PATH "Куда устанавливать плагины" FORCE)
endif()

function(obs_plugin_setup target)
  target_link_libraries(${target} PRIVATE OBS::libobs)
  target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W3 /utf-8 /MP)
  endif()
  set_target_properties(${target} PROPERTIES PREFIX "")

  install(TARGETS ${target}
    LIBRARY DESTINATION "${target}/bin/64bit"
    RUNTIME DESTINATION "${target}/bin/64bit")
  if(MSVC)
    install(FILES "$<TARGET_PDB_FILE:${target}>" DESTINATION "${target}/bin/64bit" OPTIONAL)
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/" DESTINATION "${target}/data")
  endif()
endfunction()
