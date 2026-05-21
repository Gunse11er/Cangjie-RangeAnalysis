#!/bin/bash

# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set -e;

# 编译windows frontend
export CMAKE_PREFIX_PATH=${MINGW_PATH}/x86_64-w64-mingw32;
cd ${WORKSPACE}/cangjie_compiler;
python3 build.py build -t ${build_type} \
	--product libs \
	--target windows-x86_64 \
	--target-sysroot ${MINGW_PATH}/ \
	--target-toolchain ${MINGW_PATH}/bin;
python3 build.py install;

source output/envsetup.sh;
# 验证cjc可用
cjc -v;

# 编译windows 运行时
cd ${WORKSPACE}/cangjie_runtime/runtime;
python3 build.py clean;
python3 build.py build -t ${build_type} \
    --target windows-x86_64 \
	  --target-toolchain ${MINGW_PATH}/bin \
    -v ${cangjie_version};
python3 build.py install;
cp -rf ${WORKSPACE}/cangjie_runtime/runtime/output/common/windows_${build_type}_x86_64/{lib,runtime} ${WORKSPACE}/cangjie_compiler/output;

# 编译windows 标准库
cd ${WORKSPACE}/cangjie_runtime/std;
python3 build.py clean;
python3 build.py build -t ${build_type} \
	--target windows-x86_64 \
    --target-lib=${WORKSPACE}/cangjie_runtime/runtime/output \
    --target-lib=${MINGW_PATH}/x86_64-w64-mingw32/lib \
    --target-sysroot ${MINGW_PATH}/ \
    --target-toolchain ${MINGW_PATH}/bin;
python3 build.py install;
cp -rf ${WORKSPACE}/cangjie_runtime/std/output/* ${WORKSPACE}/cangjie_compiler/output/;