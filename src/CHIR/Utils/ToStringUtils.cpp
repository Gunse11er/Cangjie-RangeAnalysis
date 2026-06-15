// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

/**
 * @file
 *
 * This file implements tostring utils API for CHIR
 */

#include "cangjie/CHIR/Utils/ToStringUtils.h"

#include "cangjie/CHIR/IR/Type/Type.h"
#include <cstddef>

namespace Cangjie::CHIR {
std::string IndentToString(size_t indent)
{
    constexpr size_t unit = 2;  // indent length
    return std::string(unit * indent, ' ');
}

std::string GetGenericTypeConstaintsStr(const std::vector<GenericType*>& genericTypeParams)
{
    if (genericTypeParams.empty()) {
        return "";
    }
    std::vector<std::string> genericTypeStr;
    for (auto genericTypeParam : genericTypeParams) {
        auto upperBounds = genericTypeParam->GetUpperBounds();
        if (upperBounds.empty()) {
            continue;
        }
        auto constaintStr = TypeVecToString("", upperBounds, "", " & ");
        genericTypeStr.emplace_back(genericTypeParam->ToString() + " <: " + constaintStr);
    }
    if (genericTypeStr.empty()) {
        return "";
    }
    auto result = "[" + StringJoin(genericTypeStr, ", ") + "]";
    return "genericConstraints: " + result;
}

std::string PackageAccessLevelToString(const Package::AccessLevel& level)
{
    static std::unordered_map<Package::AccessLevel, std::string> PACKAGE_ACCESS_LEVEL_TO_STRING_MAP = {
        {Package::AccessLevel::INTERNAL, "internal"},
        {Package::AccessLevel::PROTECTED, "protected"},
        {Package::AccessLevel::PUBLIC, "public"},
    };
    return PACKAGE_ACCESS_LEVEL_TO_STRING_MAP.at(level);
}

std::string OverflowToString(Cangjie::OverflowStrategy ofStrategy)
{
    static const std::unordered_map<Cangjie::OverflowStrategy, std::string> OVERFLOW_STR_MAP {
        {Cangjie::OverflowStrategy::NA, "NA"},
        {Cangjie::OverflowStrategy::CHECKED, "CHECKED"},
        {Cangjie::OverflowStrategy::WRAPPING, "WRAPPING"},
        {Cangjie::OverflowStrategy::THROWING, "THROWING"},
        {Cangjie::OverflowStrategy::SATURATING, "SATURATING"},
    };
    return "Overflow: " + OVERFLOW_STR_MAP.at(ofStrategy);
}

std::string StringJoin(const std::vector<std::string>& candidates, const std::string& delimiter)
{
    std::string result;
    for (const auto& temp : candidates) {
        if (temp.empty()) {
            continue;
        }
        result += temp + delimiter;
    }
    if (!result.empty()) {
        for (size_t i = 0; i < delimiter.size(); ++i) {
            result.pop_back();
        }
    }
    
    return result;
}

std::string IntrinsicKindToString(const IntrinsicKind kind)
{
    static std::unordered_map<IntrinsicKind, std::string> INTRINSIC_KIND_TO_STRING_MAP{
        {NOT_INTRINSIC, "notIntrinsic"}, {NOT_IMPLEMENTED, "notImplemented"},

        // For hoisting, but we should later split arraybuilder
        // into allocation and initialisation
        {ARRAY_INIT, "arrayInit"},

        // CORE
        {SIZE_OF, SIZE_OF_NAME}, {ALIGN_OF, ALIGN_OF_NAME},
        {GET_TYPE_FOR_TYPE_PARAMETER, GET_TYPE_FOR_TYPE_PARAMETER_NAME},
        {IS_SUBTYPE_TYPES, IS_SUBTYPE_TYPES_NAME},
        {ARRAY_ACQUIRE_RAW_DATA, ARRAY_ACQUIRE_RAW_DATA_NAME},
        {ARRAY_RELEASE_RAW_DATA, ARRAY_RELEASE_RAW_DATA_NAME},
        {ARRAY_BUILT_IN_COPY_TO, ARRAY_BUILT_IN_COPY_TO_NAME}, {ARRAY_GET, ARRAY_GET_NAME},
        {ARRAY_SET, ARRAY_SET_NAME}, {ARRAY_GET_UNCHECKED, ARRAY_GET_UNCHECKED_NAME},
        {ARRAY_GET_REF_UNCHECKED, ARRAY_GET_REF_UNCHECKED_NAME},
        {ARRAY_SET_UNCHECKED, ARRAY_SET_UNCHECKED_NAME}, {ARRAY_SIZE, ARRAY_SIZE_NAME},
        {ARRAY_CLONE, ARRAY_CLONE_NAME}, {ARRAY_SLICE_INIT, ARRAY_SLICE_INIT_NAME},
        {ARRAY_SLICE, ARRAY_SLICE_NAME}, {ARRAY_SLICE_RAWARRAY, ARRAY_SLICE_RAWARRAY_NAME},
        {ARRAY_SLICE_START, ARRAY_SLICE_START_NAME}, {ARRAY_SLICE_SIZE, ARRAY_SLICE_SIZE_NAME},
        {ARRAY_SLICE_GET_ELEMENT, ARRAY_SLICE_GET_ELEMENT_NAME},
        {ARRAY_SLICE_SET_ELEMENT, ARRAY_SLICE_SET_ELEMENT_NAME},
        {ARRAY_SLICE_GET_ELEMENT_UNCHECKED, ARRAY_SLICE_GET_ELEMENT_UNCHECKED_NAME},
        {ARRAY_SLICE_SET_ELEMENT_UNCHECKED, ARRAY_SLICE_SET_ELEMENT_UNCHECKED_NAME},
        {FILL_IN_STACK_TRACE, FILL_IN_STACK_TRACE_NAME},
        {DECODE_STACK_TRACE, DECODE_STACK_TRACE_NAME},
        {DUMP_CURRENT_THREAD_INFO, DUMP_CURRENT_THREAD_INFO_NAME},
        {DUMP_ALL_THREADS_INFO, DUMP_ALL_THREADS_INFO_NAME},
        {CHR, "chr"}, {ORD, "ord"},
        {CPOINTER_GET_POINTER_ADDRESS, CPOINTER_GET_POINTER_ADDRESS_NAME},
        {CPOINTER_INIT0, "pointerInit0"}, // CPointer constructor with no parameters
        {CPOINTER_INIT1, "pointerInit1"}, // CPointer constructor with one parameter
        {CPOINTER_READ, CPOINTER_READ_NAME}, {CPOINTER_WRITE, CPOINTER_WRITE_NAME},
        {CPOINTER_ADD, CPOINTER_ADD_NAME},
        {CSTRING_INIT, "_CString_init_"},
        {CSTRING_CONVERT_CSTR_TO_PTR, CSTRING_CONVERT_CSTR_TO_PTR_NAME},
        {BIT_CAST, BIT_CAST_NAME},
        {INOUT_PARAM, "_inout_"},
        {REGISTER_WATCHED_OBJECT, "registerWatchedObject"},
        {OBJECT_REFEQ, OBJECT_REFEQ_NAME},
        {RAW_ARRAY_REFEQ, RAW_ARRAY_REFEQ_NAME},
        {FUNC_REFEQ, FUNC_REFEQ_NAME},
        {OBJECT_ZERO_VALUE, OBJECT_ZERO_VALUE_NAME},
        {INVOKE_GC, INVOKE_GC_NAME}, {SET_GC_THRESHOLD, SET_GC_THRESHOLD_NAME},
        {GET_MAX_HEAP_SIZE, GET_MAX_HEAP_SIZE_NAME},
        {GET_ALLOCATE_HEAP_SIZE, GET_ALLOCATE_HEAP_SIZE_NAME},
        {DUMP_CJ_HEAP_DATA, DUMP_CJ_HEAP_DATA_NAME},
        {GET_GC_COUNT, GET_GC_COUNT_NAME},
        {GET_GC_TIME_US, GET_GC_TIME_US_NAME},
        {GET_GC_FREED_SIZE, GET_GC_FREED_SIZE_NAME},
        {START_CJ_CPU_PROFILING, START_CJ_CPU_PROFILING_NAME},
        {STOP_CJ_CPU_PROFILING, STOP_CJ_CPU_PROFILING_NAME},
        {GET_REAL_HEAP_SIZE, GET_REAL_HEAP_SIZE_NAME},
        {GET_THREAD_NUMBER, GET_THREAD_NUMBER_NAME},
        {GET_BLOCKING_THREAD_NUMBER, GET_BLOCKING_THREAD_NUMBER_NAME},
        {GET_NATIVE_THREAD_NUMBER, GET_NATIVE_THREAD_NUMBER_NAME},
        {VARRAY_SET, VARRAY_SET_NAME}, {VARRAY_GET, VARRAY_GET_NAME},

        // About Future
        {FUTURE_INIT, FUTURE_INIT_NAME},
        {FUTURE_IS_COMPLETE, FUTURE_IS_COMPLETE_NAME}, {FUTURE_WAIT, FUTURE_WAIT_NAME},
        {FUTURE_NOTIFYALL, FUTURE_NOTIFYALL_NAME},
        {IS_THREAD_OBJECT_INITED, IS_THREAD_OBJECT_INITED_NAME},
        {GET_THREAD_OBJECT, GET_THREAD_OBJECT_NAME},
        {SET_THREAD_OBJECT, SET_THREAD_OBJECT_NAME},
        {OVERFLOW_CHECKED_ADD, OVERFLOW_CHECKED_ADD_NAME},
        {OVERFLOW_CHECKED_SUB, OVERFLOW_CHECKED_SUB_NAME},
        {OVERFLOW_CHECKED_MUL, OVERFLOW_CHECKED_MUL_NAME},
        {OVERFLOW_CHECKED_DIV, OVERFLOW_CHECKED_DIV_NAME},
        {OVERFLOW_CHECKED_MOD, OVERFLOW_CHECKED_MOD_NAME},
        {OVERFLOW_CHECKED_POW, OVERFLOW_CHECKED_POW_NAME},
        {OVERFLOW_CHECKED_INC, OVERFLOW_CHECKED_INC_NAME},
        {OVERFLOW_CHECKED_DEC, OVERFLOW_CHECKED_DEC_NAME},
        {OVERFLOW_CHECKED_NEG, OVERFLOW_CHECKED_NEG_NAME},
        {OVERFLOW_THROWING_ADD, OVERFLOW_THROWING_ADD_NAME},
        {OVERFLOW_THROWING_SUB, OVERFLOW_THROWING_SUB_NAME},
        {OVERFLOW_THROWING_MUL, OVERFLOW_THROWING_MUL_NAME},
        {OVERFLOW_THROWING_DIV, OVERFLOW_THROWING_DIV_NAME},
        {OVERFLOW_THROWING_MOD, OVERFLOW_THROWING_MOD_NAME},
        {OVERFLOW_THROWING_POW, OVERFLOW_THROWING_POW_NAME},
        {OVERFLOW_THROWING_INC, OVERFLOW_THROWING_INC_NAME},
        {OVERFLOW_THROWING_DEC, OVERFLOW_THROWING_DEC_NAME},
        {OVERFLOW_THROWING_NEG, OVERFLOW_THROWING_NEG_NAME},
        {OVERFLOW_SATURATING_ADD, OVERFLOW_SATURATING_ADD_NAME},
        {OVERFLOW_SATURATING_SUB, OVERFLOW_SATURATING_SUB_NAME},
        {OVERFLOW_SATURATING_MUL, OVERFLOW_SATURATING_MUL_NAME},
        {OVERFLOW_SATURATING_DIV, OVERFLOW_SATURATING_DIV_NAME},
        {OVERFLOW_SATURATING_MOD, OVERFLOW_SATURATING_MOD_NAME},
        {OVERFLOW_SATURATING_POW, OVERFLOW_SATURATING_POW_NAME},
        {OVERFLOW_SATURATING_INC, OVERFLOW_SATURATING_INC_NAME},
        {OVERFLOW_SATURATING_DEC, OVERFLOW_SATURATING_DEC_NAME},
        {OVERFLOW_SATURATING_NEG, OVERFLOW_SATURATING_NEG_NAME},
        {OVERFLOW_WRAPPING_ADD, OVERFLOW_WRAPPING_ADD_NAME},
        {OVERFLOW_WRAPPING_SUB, OVERFLOW_WRAPPING_SUB_NAME},
        {OVERFLOW_WRAPPING_MUL, OVERFLOW_WRAPPING_MUL_NAME},
        {OVERFLOW_WRAPPING_DIV, OVERFLOW_WRAPPING_DIV_NAME},
        {OVERFLOW_WRAPPING_MOD, OVERFLOW_WRAPPING_MOD_NAME},
        {OVERFLOW_WRAPPING_POW, OVERFLOW_WRAPPING_POW_NAME},
        {OVERFLOW_WRAPPING_INC, OVERFLOW_WRAPPING_INC_NAME},
        {OVERFLOW_WRAPPING_DEC, OVERFLOW_WRAPPING_DEC_NAME},
        {OVERFLOW_WRAPPING_NEG, OVERFLOW_WRAPPING_NEG_NAME},
        {REFLECTION_INTRINSIC_START_FLAG, "reflectionIntrinsicStart"},
        // REFLECTION
    #define REFLECTION_KIND_TO_RUNTIME_FUNCTION(REFLECTION_KIND, CJ_FUNCTION, RUNTIME_FUNCTION) \
        {REFLECTION_KIND, #CJ_FUNCTION},
    #include "cangjie/CHIR/Utils/LLVMReflectionIntrinsics.def"
    #undef REFLECTION_KIND_TO_RUNTIME_FUNCTION
        {REFLECTION_INTRINSIC_END_FLAG, "reflectionIntrinsicEnd"},
        {SLEEP, SLEEP_NAME},
        {SOURCE_FILE, SOURCE_FILE_NAME}, {SOURCE_LINE, SOURCE_LINE_NAME},
        {IDENTITY_HASHCODE, IDENTITY_HASHCODE_NAME},
        {IDENTITY_HASHCODE_FOR_ARRAY, IDENTITY_HASHCODE_FOR_ARRAY_NAME},

        // SYNC
        {ATOMIC_LOAD, ATOMIC_LOAD_NAME}, {ATOMIC_STORE, ATOMIC_STORE_NAME},
        {ATOMIC_SWAP, ATOMIC_SWAP_NAME},
        {ATOMIC_COMPARE_AND_SWAP, ATOMIC_COMPARE_AND_SWAP_NAME},
        {ATOMIC_FETCH_ADD, ATOMIC_FETCH_ADD_NAME}, {ATOMIC_FETCH_SUB, ATOMIC_FETCH_SUB_NAME},
        {ATOMIC_FETCH_AND, ATOMIC_FETCH_AND_NAME}, {ATOMIC_FETCH_OR, ATOMIC_FETCH_OR_NAME},
        {ATOMIC_FETCH_XOR, ATOMIC_FETCH_XOR_NAME}, {MUTEX_INIT, MUTEX_INIT_NAME},
        {CJ_MUTEX_LOCK, MUTEX_LOCK_NAME}, {MUTEX_TRY_LOCK, MUTEX_TRY_LOCK_NAME},
        {MUTEX_CHECK_STATUS, MUTEX_CHECK_STATUS_NAME}, {MUTEX_UNLOCK, MUTEX_UNLOCK_NAME},
        {WAITQUEUE_INIT, WAITQUEUE_INIT_NAME}, {MONITOR_INIT, MONITOR_INIT_NAME},
        {MOITIOR_WAIT, MOITIOR_WAIT_NAME}, {MOITIOR_NOTIFY, MOITIOR_NOTIFY_NAME},
        {MOITIOR_NOTIFY_ALL, MOITIOR_NOTIFY_ALL_NAME},
        {MULTICONDITION_WAIT, MULTICONDITION_WAIT_NAME},
        {MULTICONDITION_NOTIFY, MULTICONDITION_NOTIFY_NAME},
        {MULTICONDITION_NOTIFY_ALL, MULTICONDITION_NOTIFY_ALL_NAME},
        {VECTOR_COMPARE_32, VECTOR_COMPARE_32_NAME},
        {VECTOR_INDEX_BYTE_32, VECTOR_INDEX_BYTE_32_NAME},
        {CJ_CORE_CAN_USE_SIMD, CJ_CORE_CAN_USE_SIMD_NAME},
        {CROSS_ACCESS_BARRIER, CROSS_ACCESS_BARRIER_NAME},
        {CREATE_EXPORT_HANDLE, CREATE_EXPORT_HANDLE_NAME},
        {GET_EXPORTED_REF, GET_EXPORTED_REF_NAME},
        {REMOVE_EXPORTED_REF, REMOVE_EXPORTED_REF_NAME},
        {GET_JSLAMBDA_ADDR, GET_JSLAMBDA_ADDR_NAME},
        {CJ_TLS_DYN_SET_SESSION_CALLBACK, CJ_TLS_DYN_SET_SESSION_CALLBACK_NAME},
        {CJ_TLS_DYN_SSL_INIT, CJ_TLS_DYN_SSL_INIT_NAME},
        // CodeGen
        {CG_UNSAFE_BEGIN, "cgUnsafeBegin"}, {CG_UNSAFE_END, "cgUnsafeEnd"},
        // CHIR 2: Exception intrinsic
        {BEGIN_CATCH, "beginCatch"},

        // CHIR 2: Math intrinsic
        {ABS, "abs"}, {FABS, "fabs"}, {FLOOR, "floor"}, {CEIL, "ceil"}, {TRUNC, "trunc"},
        {SIN, "sin"}, {COS, "cos"}, {EXP, "exp"}, {EXP2, "exp2"}, {LOG, "log"},
        {LOG2, "log2"}, {LOG10, "log10"}, {SQRT, "sqrt"}, {ROUND, "round"}, {POW, "pow"},
        {POWI, "powi"},
        // preinitialize intrinsic
        {PREINITIALIZE, "preinitialize"},
        {IS_THREAD_OBJECT_INITED, IS_THREAD_OBJECT_INITED_NAME},

        // Box cast intrinsic
        {OBJECT_AS, "object.as"}, {IS_NULL, "isNull"},
        {BLACK_BOX, "blackBox"},

        // spawn related
        {EXCLUSIVE_SCOPE, "exclusiveScopeImpl"},
    };
    return INTRINSIC_KIND_TO_STRING_MAP.at(kind);
}

std::string AddNewLineOrNot(const std::string& message)
{
    if (message.empty()) {
        return "";
    }
    return message + "\n";
}

std::string CommentToString(const std::vector<std::string>& message)
{
    return CommentToString(StringJoin(message, ", "));
}

std::string CommentToString(const std::string& message)
{
    if (message.empty()) {
        return "";
    }
    return " // " + message;
}
}