#!/bin/bash

# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set -e;

# 编译Cangjie编译器 + cjdb
cd ${WORKSPACE}/cangjie_compiler;
python3 build.py clean;
python3 build.py build -t ${build_type} --build-cjdb ${add_opts_buildpy};
python3 build.py install;

source output/envsetup.sh;
# 验证安装
cjc -v;

# 编译运行时
cd ${WORKSPACE}/cangjie_runtime/runtime;
python3 build.py clean;
python3 build.py build -t ${build_type} -v ${cangjie_version};
python3 build.py install;
cp -rf ${WORKSPACE}/cangjie_runtime/runtime/output/common/${kernel}_${build_type}_${cmake_arch}/{lib,runtime} ${WORKSPACE}/cangjie_compiler/output;

# 编译标准库
cd ${WORKSPACE}/cangjie_runtime/std;
python3 build.py clean;
python3 build.py build -t ${build_type} \
    --target-lib=${WORKSPACE}/cangjie_runtime/runtime/output \
    --target-lib=$OPENSSL_PATH;
python3 build.py install;
cp -rf ${WORKSPACE}/cangjie_runtime/std/output/* ${WORKSPACE}/cangjie_compiler/output/;

# 编译STDX扩展库
cd ${WORKSPACE}/cangjie_stdx;
python3 build.py clean;
python3 build.py build -t ${build_type} \
  --include=${WORKSPACE}/cangjie_compiler/include \
  --target-lib=$OPENSSL_PATH;
python3 build.py install;

export CANGJIE_STDX_PATH=${WORKSPACE}/cangjie_stdx/target/${kernel}_${cmake_arch}_cjnative/static/stdx;

# 编译cjpm
cd ${WORKSPACE}/cangjie_tools/cjpm/build;
python3 build.py clean;
python3 build.py build -t ${build_type} --set-rpath $RPATH;
python3 build.py install;

# 编译cjfmt
cd ${WORKSPACE}/cangjie_tools/cjfmt;
cd build;
python3 build.py clean;
python3 build.py build -t ${build_type};
python3 build.py install;

# 编译hle
cd ${WORKSPACE}/cangjie_tools/hyperlangExtension/build;
python3 build.py clean;
python3 build.py build -t ${build_type};
python3 build.py install;

# 编译lsp
cd ${WORKSPACE}/cangjie_tools/cangjie-language-server/build;
python3 build.py clean;
python3 build.py build -t ${build_type} -j 16;
python3 build.py install;