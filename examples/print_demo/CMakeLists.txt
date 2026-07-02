cmake_minimum_required(VERSION 3.16)
# Consume the component from the repo root (two levels up). Out-of-tree users
# would instead add it to main/idf_component.yml as a git dependency.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ptouch_print_demo)
