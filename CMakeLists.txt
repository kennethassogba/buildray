cmake_minimum_required(VERSION 3.30)
project(buildray VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Buildray currently supports Linux only")
endif()

find_package(LLVM REQUIRED CONFIG)
if(LLVM_PACKAGE_VERSION VERSION_LESS 20)
    message(FATAL_ERROR "Buildray requires LLVM 20 or newer")
endif()
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION}: ${LLVM_DIR}")
if(TARGET LLVM)
    set(BUILDRAY_LLVM_LIBS LLVM)
else()
    llvm_map_components_to_libnames(BUILDRAY_LLVM_LIBS Support)
endif()

add_library(buildray_core src/process.cpp src/core.cpp src/pipeline.cpp src/comparison.cpp src/profiling.cpp)
target_compile_definitions(buildray_core PUBLIC BUILDRAY_VERSION="${PROJECT_VERSION}")
target_include_directories(buildray_core PUBLIC include)
target_include_directories(buildray_core SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
target_link_libraries(buildray_core PUBLIC ${BUILDRAY_LLVM_LIBS})
target_compile_options(buildray_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
option(BUILDRAY_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(BUILDRAY_ENABLE_SANITIZERS)
    target_compile_options(buildray_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(buildray_core PUBLIC -fsanitize=address,undefined)
endif()

add_executable(buildray src/main.cpp)
target_link_libraries(buildray PRIVATE buildray_core)
target_compile_options(buildray PRIVATE -Wall -Wextra -Wpedantic)
include(GNUInstallDirs)
install(TARGETS buildray RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

include(CTest)
if(BUILD_TESTING)
    add_executable(buildray_unit tests/unit.cpp)
    target_link_libraries(buildray_unit PRIVATE buildray_core)
    add_test(NAME unit COMMAND buildray_unit)
    find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
    find_program(BUILDRAY_CLANG_TIDY NAMES clang-tidy-21 clang-tidy-20 clang-tidy REQUIRED)
    find_program(BUILDRAY_CLANGXX NAMES clang++-21 clang++-20 clang++ REQUIRED)
    add_test(NAME integration COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py $<TARGET_FILE:buildray>
        ${BUILDRAY_CLANGXX} ${BUILDRAY_CLANG_TIDY})
    set_tests_properties(integration PROPERTIES TIMEOUT 180)
    add_test(NAME comparison COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/comparison_cli.py $<TARGET_FILE:buildray> ${BUILDRAY_CLANGXX})
    add_test(NAME action COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/action_cli.py $<TARGET_FILE:buildray>
        ${BUILDRAY_CLANGXX} ${CMAKE_CURRENT_SOURCE_DIR}/action/run.py)
    set_tests_properties(comparison action PROPERTIES TIMEOUT 180)
    add_test(NAME profiling COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/profiling_cli.py $<TARGET_FILE:buildray> ${BUILDRAY_CLANGXX})
    set_tests_properties(profiling PROPERTIES TIMEOUT 180)
endif()
