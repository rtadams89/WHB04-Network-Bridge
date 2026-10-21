# WHB04 Network Bridge - ESP-IDF project
cmake_minimum_required(VERSION 3.16)

# Optional, git-ignored local defaults (e.g. compile-time WiFi credentials).
# See README.md, "Compile-time WiFi credentials".
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(whb04-bridge)
