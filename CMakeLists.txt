cmake_minimum_required(VERSION 3.30)
project(buildray VERSION 0.12.0 LANGUAGES CXX)

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

add_library(buildray_core src/process.cpp src/core.cpp src/pipeline.cpp src/comparison.cpp src/profiling.cpp src/dependencies.cpp src/cmake.cpp src/linking.cpp src/header_cleanup.cpp src/unity.cpp)
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
find_path(BUILDRAY_CLANG_INCLUDE_DIR clang/Tooling/Tooling.h HINTS ${LLVM_INCLUDE_DIRS} REQUIRED)
find_library(BUILDRAY_CLANG_CPP_LIBRARY NAMES clang-cpp HINTS ${LLVM_LIBRARY_DIRS})
if(NOT BUILDRAY_CLANG_CPP_LIBRARY)
    find_file(BUILDRAY_CLANG_CPP_LIBRARY
        NAMES libclang-cpp.so.${LLVM_VERSION_MAJOR}.1 libclang-cpp.so.${LLVM_VERSION_MAJOR}
        HINTS ${LLVM_LIBRARY_DIRS} REQUIRED)
endif()
add_executable(buildray-header-tool src/header_tool.cpp)
target_include_directories(buildray-header-tool SYSTEM PRIVATE ${BUILDRAY_CLANG_INCLUDE_DIR} ${LLVM_INCLUDE_DIRS})
target_link_libraries(buildray-header-tool PRIVATE ${BUILDRAY_CLANG_CPP_LIBRARY} ${BUILDRAY_LLVM_LIBS})
target_compile_options(buildray-header-tool PRIVATE -Wall -Wextra -Wpedantic)
if(BUILDRAY_ENABLE_SANITIZERS)
    target_compile_options(buildray-header-tool PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(buildray-header-tool PRIVATE -fsanitize=address,undefined)
endif()
include(GNUInstallDirs)
install(TARGETS buildray buildray-header-tool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

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
    add_test(NAME dependencies COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/dependencies_cli.py $<TARGET_FILE:buildray> ${BUILDRAY_CLANGXX})
    set_tests_properties(dependencies PROPERTIES TIMEOUT 180)
    add_test(NAME cmake_links COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake_cli.py $<TARGET_FILE:buildray> ${BUILDRAY_CLANGXX})
    set_tests_properties(cmake_links PROPERTIES TIMEOUT 180)
    find_program(BUILDRAY_LLD NAMES ld.lld-21 ld.lld-20 ld.lld REQUIRED)
    add_test(NAME unused_dependencies COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unused_cli.py $<TARGET_FILE:buildray> ${BUILDRAY_CLANGXX} ${BUILDRAY_LLD})
    set_tests_properties(unused_dependencies PROPERTIES TIMEOUT 240)
    add_test(NAME batched_dependencies COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/batched_dependencies_cli.py $<TARGET_FILE:buildray>
        ${BUILDRAY_CLANGXX} ${BUILDRAY_LLD} ${CMAKE_CURRENT_SOURCE_DIR}/action/run.py)
    set_tests_properties(batched_dependencies PROPERTIES TIMEOUT 360)
    add_test(NAME headers COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/headers_cli.py $<TARGET_FILE:buildray>
        ${BUILDRAY_CLANGXX} ${BUILDRAY_CLANG_TIDY})
    set_tests_properties(headers PROPERTIES TIMEOUT 300)
    add_test(NAME unity COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unity_cli.py $<TARGET_FILE:buildray> ${BUILDRAY_CLANGXX})
    set_tests_properties(unity PROPERTIES TIMEOUT 240)
    add_test(NAME incremental COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/incremental_cli.py $<TARGET_FILE:buildray> ${BUILDRAY_CLANGXX})
    set_tests_properties(incremental PROPERTIES TIMEOUT 240)
    add_test(NAME parallelism COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/parallelism_cli.py $<TARGET_FILE:buildray>
        ${BUILDRAY_CLANGXX} ${CMAKE_CURRENT_SOURCE_DIR}/action/run.py)
    set_tests_properties(parallelism PROPERTIES TIMEOUT 240)
endif()
