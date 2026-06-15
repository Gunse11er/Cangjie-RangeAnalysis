# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

include_directories(${CMAKE_SOURCE_DIR}/demangler)
link_directories("${CMAKE_BINARY_DIR}/cangjie-demangler/lib")

add_executable(DemangleTest DemangleTest.cpp)
add_dependencies(DemangleTest cangjie-demangler)
target_link_libraries(
    DemangleTest
    cangjie-demangle
    GTest::gtest
    GTest::gtest_main
)
target_compile_definitions(DemangleTest PRIVATE BUILD_LIB_CANGJIE_DEMANGLE)
add_test(NAME DemangleTest COMMAND DemangleTest)