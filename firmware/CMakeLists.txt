cmake_minimum_required(VERSION 3.16)
# версия прошивки (её видно на сайте, по ней сервер решает, обновлять ли) — короткий хеш коммита из CI
if(DEFINED ENV{HBOX_VERSION})
    set(PROJECT_VER $ENV{HBOX_VERSION})
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hbox)
