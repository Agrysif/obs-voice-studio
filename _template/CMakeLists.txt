cmake_minimum_required(VERSION 3.20)

# Имя плагина = имя папки (например, папка "my-filter" -> my-filter.dll).
get_filename_component(PLUGIN_NAME "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
string(REGEX REPLACE "^_+" "" PLUGIN_NAME "${PLUGIN_NAME}")

project(${PLUGIN_NAME} VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include("${CMAKE_CURRENT_SOURCE_DIR}/../_shared/cmake/ObsPluginCommon.cmake")

add_library(${PLUGIN_NAME} MODULE
  src/plugin-main.cpp)
target_compile_definitions(${PLUGIN_NAME} PRIVATE
  PLUGIN_NAME="${PLUGIN_NAME}"
  PLUGIN_VERSION="${PROJECT_VERSION}")
obs_plugin_setup(${PLUGIN_NAME})
