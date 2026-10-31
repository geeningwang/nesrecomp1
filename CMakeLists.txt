cmake_minimum_required(VERSION 3.16)
project(nesrecomp1 C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Filled in as the milestones land (see doc/plan.md):
#   runtime/   (M1)   portable C runtime, built as a static library `nesrt`
#   platform/  (M3+)  desktop / web / stm32 front ends
if(EXISTS ${CMAKE_SOURCE_DIR}/runtime/CMakeLists.txt)
  add_subdirectory(runtime)
endif()
