# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.


set(AST_SERIALIZATION_SRC NodeSerialization.cpp ExprSerialization.cpp TokenSerialization.cpp MacroCommon.cpp MacroCall.cpp)
add_library(CangjieAstSerialization OBJECT ${AST_SERIALIZATION_SRC})
add_dependencies(CangjieAstSerialization CangjieFlatbuffersHeaders)
target_include_directories(CangjieAstSerialization PRIVATE ${FLATBUFFERS_INCLUDE_DIR})

if(CANGJIE_BUILD_CJC)
    set(MACRO_SRC
        MacroExpansion.cpp
        MacroProcess.cpp
        MacroEvaluation.cpp
        InvokeUtil.cpp
        MacroCallResolve.cpp
        TestEntryConstructor.cpp)
    list(APPEND MACRO_SRC MacroEvaluationSrv.cpp MacroEvaluationClient.cpp MacroEvalMsgSerializer.cpp)
    if(CANGJIE_CODEGEN_CJNATIVE_BACKEND)
        list(APPEND MACRO_SRC MacroEvaluationCJNative.cpp)
        list(APPEND MACRO_SRC InvokeUtilCJNative.cpp)
    endif()
    add_library(CangjieMacro OBJECT ${MACRO_SRC})
    add_dependencies(CangjieMacro CangjieFlatbuffersHeaders)
    target_include_directories(CangjieMacro PRIVATE ${FLATBUFFERS_INCLUDE_DIR})

    if(CANGJIE_CODEGEN_CJNATIVE_BACKEND)
        add_dependencies(CangjieMacro cjnative)
    endif()
    target_include_directories(CangjieMacro PRIVATE ${LLVM_INCLUDE_DIRS})
    target_include_directories(CangjieMacro PRIVATE ${BOUNDSCHECK}/include)
    target_compile_options(CangjieMacro PRIVATE ${CJC_EXTRA_WARNINGS})
endif()
