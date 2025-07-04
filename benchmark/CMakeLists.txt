# Copyright 2023 Intel Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# The svs-benchmark consists of two components:
#
# (1) A shared-library containing most of the implementation logic.
# (2) A thin executable that provides a command-line interface to the shared-library.
#
# This is because svs-benchmark provides utilities that can be reused for testing.
# This is done by linking the test executable to svs-benchmark shared library.

# As a compile-time optimization, if we're just building the tests, we disable all
# specializations compiled in the shared library.
if (${SVS_BUILD_BENCHMARK})
    set(SVS_BENCHMARK_MINIMAL OFF)
else()
    set(SVS_BENCHMARK_MINIMAL ON)
endif()

# Core executables.
set(SHARED_LIBRARY_FILES
    src/benchmark.cpp
    src/datasets.cpp
    # vamana
    src/vamana/uncompressed.cpp
    src/vamana/search.cpp
    src/vamana/build.cpp
    src/vamana/test.cpp
    src/vamana/iterator.cpp
    # inverted
    src/inverted/inverted.cpp
    src/inverted/memory/common.cpp
    src/inverted/memory/register.cpp
    src/inverted/memory/uncompressed.cpp
    src/inverted/memory/executables/memory_build.cpp
    src/inverted/memory/executables/memory_search.cpp
    src/inverted/memory/executables/memory_test.cpp
)

add_library(svs_benchmark_library SHARED ${SHARED_LIBRARY_FILES})
target_include_directories(svs_benchmark_library PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)

# Minimal
if (${SVS_BENCHMARK_MINIMAL})
    message("Compiling minimal benchmark library")
    target_compile_options(svs_benchmark_library PUBLIC -DSVS_BENCHMARK_MINIMAL=1)
else()
    target_compile_options(svs_benchmark_library PUBLIC -DSVS_BENCHMARK_MINIMAL=0)
endif()

# Test Generators
if (${SVS_BUILD_BENCHMARK_TEST_GENERATORS})
    message("Compiling test reference generators")
    target_compile_options(
        svs_benchmark_library PUBLIC -DSVS_BENCHMARK_BUILD_TEST_GENERATORS=1
    )
else()
    target_compile_options(
        svs_benchmark_library PUBLIC -DSVS_BENCHMARK_BUILD_TEST_GENERATORS=0
    )
endif()

# Super Search
if (${SVS_BENCHMARK_VAMANA_SUPERSEARCH})
    message("Compiling Vamana super search")
    target_compile_options(
        svs_benchmark_library PUBLIC -DSVS_BENCHMARK_VAMANA_SUPERSEARCH=1
    )
else()
    target_compile_options(
        svs_benchmark_library PUBLIC -DSVS_BENCHMARK_VAMANA_SUPERSEARCH=0
    )
endif()

if (SVS_EXPERIMENTAL_CLANG_TIDY AND CLANG_TIDY_COMMAND)
    set_target_properties(
        svs_benchmark_library
        PROPERTIES
            CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}"
    )
endif()

# Enable link-time optimization for the benchmarking library.
# Only do this if construction of the executable was explicitly requested.
# Otherwise, prefer slightly lower compilation times.
if (${SVS_BUILD_BENCHMARK} AND ${CMAKE_BUILD_TYPE} STREQUAL "Release")
    set_property(TARGET svs_benchmark_library PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Link the library with the main SVS library.
target_link_libraries(
    svs_benchmark_library
    PUBLIC
        svs::svs
        svs_compile_options
        svs_x86_options_base
        fmt::fmt
)

# Build the shared library.
set(EXE_FILES src/main.cpp)
add_executable(svs_benchmark ${EXE_FILES})
target_link_libraries(svs_benchmark PRIVATE svs_benchmark_library)
