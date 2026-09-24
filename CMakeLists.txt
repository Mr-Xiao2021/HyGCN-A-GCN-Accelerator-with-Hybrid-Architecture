cmake_minimum_required(VERSION 3.0.0)
project(HyGCN)
set(CMAKE_CXX_STANDARD 17)

enable_testing()

execute_process(
        COMMAND git rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE HYGCN_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
if(NOT HYGCN_GIT_COMMIT)
    set(HYGCN_GIT_COMMIT "unknown")
endif()

add_subdirectory(./dramsim3)

file(GLOB_RECURSE hygcn_src "hygcn/*.cc" "hygcn/*.cpp" "hygcn/*.c" "hygcn/*.h")
add_library(HyGCN STATIC ${hygcn_src})
target_link_libraries(HyGCN PRIVATE args dramsim3)
target_include_directories(HyGCN PUBLIC hygcn dramsim3/src dramsim3/ext dramsim3/ext/headers)
target_compile_definitions(HyGCN PRIVATE HYGCN_GIT_COMMIT="${HYGCN_GIT_COMMIT}")

add_executable(hygcntest hygcn_test/test.cpp)
target_include_directories(hygcntest PRIVATE hygcn dramsim3/src dramsim3/ext/)
target_link_libraries(hygcntest PRIVATE dramsim3 HyGCN args)
target_compile_definitions(hygcntest PRIVATE HYGCN_GIT_COMMIT="${HYGCN_GIT_COMMIT}")
target_compile_options(hygcntest PRIVATE)
set_target_properties(hygcntest PROPERTIES
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )

add_executable(hygcn_unit_tests hygcn_test/unit.cpp)
target_include_directories(hygcn_unit_tests PRIVATE hygcn dramsim3/src dramsim3/ext dramsim3/ext/headers)
target_link_libraries(hygcn_unit_tests PRIVATE HyGCN)
set_target_properties(hygcn_unit_tests PROPERTIES
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )

add_test(NAME hygcn_unit COMMAND hygcn_unit_tests)
set_tests_properties(hygcn_unit PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME hygcn_smoke
        COMMAND hygcntest --engine paper --profile smoke --model gcn --dataset test
                --output-dir ${CMAKE_BINARY_DIR}/test-output --quiet)
set_tests_properties(hygcn_smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME hygcn_missing_dataset
        COMMAND hygcntest --engine paper --profile smoke --model gcn --dataset missing
                --output-dir ${CMAKE_BINARY_DIR}/test-output --quiet)
set_tests_properties(hygcn_missing_dataset PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        WILL_FAIL TRUE)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME paper_reference_manifest
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/validate_reference_metrics.py)
    set_tests_properties(paper_reference_manifest PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_test(NAME paper_metric_validator_selftest
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/test_metric_validator.py)
    set_tests_properties(paper_metric_validator_selftest PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_test(NAME legacy_snapshot_fixture
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_legacy_snapshot.py
                    --fixture-only)
    set_tests_properties(legacy_snapshot_fixture PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_test(NAME paper_output_determinism
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_determinism.py
                    --binary $<TARGET_FILE:hygcntest>)
    set_tests_properties(paper_output_determinism PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_test(NAME hygcn_cli_failures
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/test_cli.py
                    --binary $<TARGET_FILE:hygcntest>)
    set_tests_properties(hygcn_cli_failures PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_custom_target(legacy_regression
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_legacy_snapshot.py
                    --binary $<TARGET_FILE:hygcntest>
            DEPENDS hygcntest
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            USES_TERMINAL)

    add_custom_target(paper_benchmark
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/validate_reference_metrics.py
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/paper_benchmark.py
                    --binary $<TARGET_FILE:hygcntest>
                    --output-dir ${CMAKE_SOURCE_DIR}/res/paper
                    --force
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/validate_paper_metrics.py
                    --report ${CMAKE_SOURCE_DIR}/res/paper/benchmark_report.json
                    --output ${CMAKE_SOURCE_DIR}/res/paper/validation_report.md
            DEPENDS hygcntest
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            USES_TERMINAL)
endif()
