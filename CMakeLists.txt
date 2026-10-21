cmake_minimum_required(VERSION 3.28)
project(s-panel LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded") # /MT, zero runtime deps

add_library(taskbar SHARED
  src/dll/dllmain.cpp
  src/dll/hooks.cpp
  src/dll/offsets.cpp
  src/dll/resolve.cpp
  src/shared/config.cpp
  vendor/minhook/src/buffer.c
  vendor/minhook/src/hook.c
  vendor/minhook/src/trampoline.c
  vendor/minhook/src/hde/hde64.c
)
target_include_directories(taskbar PRIVATE
  vendor/minhook/include
  src/shared
)
target_link_libraries(taskbar PRIVATE ole32 oleaut32 runtimeobject shcore version)
target_compile_definitions(taskbar PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

add_executable(loader src/loader/loader.cpp src/shared/config.cpp)
target_include_directories(loader PRIVATE src/shared)
target_compile_definitions(loader PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

add_executable(test_config tests/test_config.cpp src/shared/config.cpp)
target_include_directories(test_config PRIVATE src/shared)
target_compile_definitions(test_config PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

add_executable(test_host tests/test_host.cpp)
target_compile_definitions(test_host PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

add_executable(symdump tools/symdump.cpp)
target_compile_definitions(symdump PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(symdump PRIVATE dbghelp version)

add_executable(featcheck tools/featcheck.cpp)
target_compile_definitions(featcheck PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

add_executable(pdatascan tools/pdatascan.cpp)
target_compile_definitions(pdatascan PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

add_executable(callgraph tools/callgraph.cpp vendor/minhook/src/hde/hde64.c)
target_include_directories(callgraph PRIVATE vendor/minhook/src/hde)
target_compile_definitions(callgraph PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

add_executable(probe tools/probe.cpp)
target_compile_definitions(probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

add_executable(test_resolve tests/test_resolve.cpp src/dll/resolve.cpp)
target_include_directories(test_resolve PRIVATE src/dll)
target_compile_definitions(test_resolve PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(test_resolve PRIVATE version)

set_target_properties(taskbar loader PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
set_target_properties(test_config test_host symdump featcheck pdatascan callgraph probe test_resolve PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)