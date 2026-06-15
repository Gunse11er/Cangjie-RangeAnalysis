# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

file(GLOB SEMA_NATIVE_FFI_JAVA_BEFORE_TYPECHECK_SRC CONFIGURE_DEPENDS *.cpp)
set(SEMA_NATIVE_FFI_JAVA_SRC ${SEMA_NATIVE_FFI_JAVA_SRC} ${SEMA_NATIVE_FFI_JAVA_BEFORE_TYPECHECK_SRC} PARENT_SCOPE)
