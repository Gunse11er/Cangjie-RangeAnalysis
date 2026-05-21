# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(Mangle_SRC BaseMangler.cpp ASTMangler.cpp CHIRMangler.cpp Compression.cpp)
set(Mangle_UNITTESTS_SRC BaseMangler.cpp ASTMangler.cpp CHIRMangler.cpp Compression.cpp)
if(CANGJIE_CODEGEN_CJNATIVE_BACKEND)
    get_filename_component(CHIRMANGLE CHIRTypeManglingUtils.cpp ABSOLUTE)
    list(APPEND Mangle_SRC ${CHIRMANGLE} CHIRManglingUtils.cpp)
endif()
add_library(CangjieMangle OBJECT ${Mangle_SRC})
add_library(CangjieUnittestsMangle OBJECT ${Mangle_UNITTESTS_SRC})

# use llvm
if(CANGJIE_CODEGEN_CJNATIVE_BACKEND)
    add_dependencies(CangjieMangle cjnative)
endif()

# IsEnumTrans2Class method in CodeGenUtils.h could be separated with llvm util methods.
target_include_directories(CangjieMangle PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(CangjieMangle PRIVATE ${CJC_WITH_LLVM_EXTRA_WARNINGS})
