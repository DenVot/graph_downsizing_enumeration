cmake_minimum_required(VERSION 3.22)
project(graph_downsizing_enumeration VERSION 0.4.0 LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED)

option(GRAPH_DOWNSIZING_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(GRAPH_DOWNSIZING_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)
if(GRAPH_DOWNSIZING_SANITIZERS AND GRAPH_DOWNSIZING_THREAD_SANITIZER)
    message(FATAL_ERROR "Use separate builds for address/undefined and thread sanitizers")
endif()

add_library(graph_downsizing_core
    src/combinations.cpp
    src/parallel_search.cpp
    src/worker_pool.cpp
    src/alpha.cpp
    src/config.cpp
    src/connectivity.cpp
    src/graph.cpp
    src/global_cut.cpp
    src/maximal_kvcc_enumerator.cpp
    src/solver.cpp
    src/solution.cpp
    src/tree.cpp
)
target_compile_features(graph_downsizing_core PUBLIC cxx_std_20)
set_target_properties(graph_downsizing_core PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(graph_downsizing_core PUBLIC include)
target_link_libraries(graph_downsizing_core PRIVATE nlohmann_json::nlohmann_json PUBLIC Boost::headers Threads::Threads)

function(graph_downsizing_compile_options target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
        if(GRAPH_DOWNSIZING_SANITIZERS)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        elseif(GRAPH_DOWNSIZING_THREAD_SANITIZER)
            target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=thread)
        endif()
    elseif(GRAPH_DOWNSIZING_SANITIZERS OR GRAPH_DOWNSIZING_THREAD_SANITIZER)
        message(FATAL_ERROR "Sanitizers require GCC or Clang")
    endif()
endfunction()

graph_downsizing_compile_options(graph_downsizing_core)
add_executable(graph_downsizing src/main.cpp)
target_link_libraries(graph_downsizing PRIVATE graph_downsizing_core)
target_compile_definitions(graph_downsizing PRIVATE GRAPH_DOWNSIZING_VERSION="${PROJECT_VERSION}")
graph_downsizing_compile_options(graph_downsizing)

if(BUILD_TESTING)
    add_executable(graph_downsizing_tests tests/test_core.cpp)
    target_link_libraries(graph_downsizing_tests PRIVATE graph_downsizing_core)
    graph_downsizing_compile_options(graph_downsizing_tests)
    foreach(group alpha config graph solution)
        add_test(NAME ${group} COMMAND graph_downsizing_tests ${group})
    endforeach()
    add_executable(graph_downsizing_algorithm_tests tests/test_algorithms.cpp)
    target_link_libraries(graph_downsizing_algorithm_tests PRIVATE graph_downsizing_core)
    graph_downsizing_compile_options(graph_downsizing_algorithm_tests)
    foreach(group flow global_cut enumeration_framework enumeration tree solver)
        add_test(NAME ${group} COMMAND graph_downsizing_algorithm_tests ${group})
    endforeach()
    add_executable(graph_downsizing_parallel_tests tests/test_parallel.cpp)
    target_link_libraries(graph_downsizing_parallel_tests PRIVATE graph_downsizing_core)
    graph_downsizing_compile_options(graph_downsizing_parallel_tests)
    add_test(NAME parallel COMMAND graph_downsizing_parallel_tests)
    set_tests_properties(parallel PROPERTIES TIMEOUT 60)
    add_test(NAME cli COMMAND ${CMAKE_COMMAND}
        -DEXECUTABLE=$<TARGET_FILE:graph_downsizing>
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.cmake)
endif()

install(TARGETS graph_downsizing RUNTIME DESTINATION bin)
