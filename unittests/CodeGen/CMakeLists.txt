# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(CANGJIE_CG_DEP_OBJECTS
    $<TARGET_OBJECTS:CangjieCodeGen>
    $<TARGET_OBJECTS:CangjieCHIRBase>
    )

file(GLOB CG_TEST_SRC *.cpp)
add_executable(CGTests
    ${CG_TEST_SRC}
    ${CANGJIE_CG_DEP_OBJECTS}
    ${CMAKE_SOURCE_DIR}/src/Basic/Print.cpp)
target_link_libraries(
    CGTests
    ${LLVM_LIBS}
    GTest::gtest
    GTest::gtest_main)

add_test(NAME CGTests COMMAND CGTests)

if(CANGJIE_DOWNLOAD_DEPS)
    add_dependencies(CGTests binary-deps)
elseif(CANGJIE_CODEGEN_CJNATIVE_BACKEND)
    add_dependencies(CGTests cjnative)
endif()
target_include_directories(CGTests PRIVATE ${LLVM_INCLUDE_DIRS})
