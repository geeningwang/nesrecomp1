# The portable runtime core: C++17 inside, C API outside (runtime/include/nrt.h). No host dependencies.
add_library(nrt STATIC
  src/nrt_apu.cpp
  src/nrt_cpu.cpp
  src/nrt_ppu.cpp
  src/nrt_machine.cpp)
target_include_directories(nrt PUBLIC include)
target_compile_features(nrt PUBLIC cxx_std_17)
if(MSVC)
  target_compile_options(nrt PRIVATE /W4 /WX /wd4127 /GR-)
else()
  target_compile_options(nrt PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
