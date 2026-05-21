# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

if (CANGJIE_CODEGEN_CJNATIVE_BACKEND)
    set(META_TRANSFORMATION_SRC MetaTransformPluginManager.cpp MetaTransformPluginBuilder.cpp)

    add_library(CangjieMetaTransformation OBJECT ${META_TRANSFORMATION_SRC})
    target_compile_options(CangjieMetaTransformation PRIVATE ${CJC_EXTRA_WARNINGS})
endif()
