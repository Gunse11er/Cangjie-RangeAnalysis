# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

function(apply_properties)
    set(oneValueArgs FROM_TARGET TO_TARGET)
    set(multiValueArgs PROPERTY_NAMES)

    cmake_parse_arguments(
        ARG
        ""
        "${oneValueArgs}"
        "${multiValueArgs}"
        ${ARGN})

    foreach(arg ${ARG_PROPERTY_NAMES})
        get_target_property(PROPERTY_VALUE ${ARG_FROM_TARGET} ${arg})
        if(NOT ("${PROPERTY_VALUE}" MATCHES "PROPERTY_VALUE-NOTFOUND"))
            set_target_properties(${ARG_TO_TARGET} PROPERTIES ${arg} "${PROPERTY_VALUE}")
        endif()
    endforeach(arg)
endfunction()
