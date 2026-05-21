#!/bin/bash

# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set -e;

# 编译Cangjie编译器 for native
cd ${WORKSPACE}/cangjie_compiler;
python3 build.py clean;
python3 build.py build -t ${build_type} ${add_opts_buildpy};

# 编译Cangjie编译器 + cjdb for windows
export CMAKE_PREFIX_PATH=${MINGW_PATH}/x86_64-w64-mingw32;
python3 build.py build -t ${build_type} \
	--product cjc \
  --no-tests \
	--target windows-x86_64 \
	--target-sysroot ${MINGW_PATH}/ \
	--target-toolchain ${MINGW_PATH}/bin \
	--build-cjdb;
python3 build.py build -t ${build_type} \
	--product libs \
	--target windows-x86_64 \
	--target-sysroot ${MINGW_PATH}/ \
  --target-toolchain ${MINGW_PATH}/bin;
python3 build.py install --host windows-x86_64;
python3 build.py install;
cp -rf output-x86_64-w64-mingw32/* output;

source output/envsetup.sh;
# 验证安装
cjc -v;

# 编译运行时
cd ${WORKSPACE}/cangjie_runtime/runtime;
python3 build.py clean;
python3 build.py build -t ${build_type} \
  --target windows-x86_64 \
	--target-toolchain ${MINGW_PATH}/bin \
  -v ${cangjie_version};
python3 build.py install;
cp -rf ${WORKSPACE}/cangjie_runtime/runtime/output/common/windows_${build_type}_x86_64/{lib,runtime} ${WORKSPACE}/cangjie_compiler/output;
cp -rf ${WORKSPACE}/cangjie_runtime/runtime/output/common/windows_${build_type}_x86_64/{lib,runtime} ${WORKSPACE}/cangjie_compiler/output-x86_64-w64-mingw32;

# 编译标准库
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
cp -rf ${WORKSPACE}/cangjie_runtime/std/output/* ${WORKSPACE}/cangjie_compiler/output-x86_64-w64-mingw32/;

# 编译STDX扩展库
cd ${WORKSPACE}/cangjie_stdx;
python3 build.py clean;
python3 build.py build -t ${build_type} \
	--include=$WORKSPACE/cangjie_compiler/include \
    --target-lib=${MINGW_PATH}/x86_64-w64-mingw32/lib \
	--target windows-x86_64 \
    --target-sysroot ${MINGW_PATH}/ \
    --target-toolchain ${MINGW_PATH}/bin;
python3 build.py install;
export CANGJIE_STDX_PATH=${WORKSPACE}/cangjie_stdx/target/windows_x86_64_cjnative/static/stdx;

# 编译cjpm
cd ${WORKSPACE}/cangjie_tools/cjpm/build;
python3 build.py clean;
python3 build.py build -t ${build_type} --target windows-x86_64;
python3 build.py install;

# 编译cjfmt
cd ${WORKSPACE}/cangjie_tools/cjfmt/build;
python3 build.py clean;
python3 build.py build -t ${build_type} --target windows-x86_64;
python3 build.py install;

# 编译hle
cd ${WORKSPACE}/cangjie_tools/hyperlangExtension/build;
python3 build.py clean;
python3 build.py build -t ${build_type} --target windows-x86_64;
python3 build.py install;

# 编译lsp
cd ${WORKSPACE}/cangjie_tools/cangjie-language-server/build;
python3 build.py clean;
python3 build.py build -t ${build_type} --target windows-x86_64 -j 16;
python3 build.py install;

# 清空历史构建
mkdir -p $WORKSPACE/software;
rm -rf $WORKSPACE/software/*;

# 打包Cangjie Frontend
cd $WORKSPACE/software;
mkdir -p cangjie/lib/windows_x86_64_cjnative;
cp $WORKSPACE/cangjie_compiler/LICENSE cangjie;
cp $WORKSPACE/cangjie_compiler/Open_Source_Software_Notice.docx cangjie;
chmod -R 750 cangjie;
mv $WORKSPACE/cangjie_compiler/output-x86_64-w64-mingw32/lib/windows_x86_64_cjnative/libcangjie-ast-support.a cangjie/lib/windows_x86_64_cjnative;
find cangjie -print0 | xargs -0r touch -t "$BEP_BUILD_TIME";
find cangjie -print0 | LC_ALL=C sort -z | xargs -0 zip -o -X $WORKSPACE/software/cangjie-frontend-windows-x64-${cangjie_version}.zip;

# 打包Cangjie SDK
rm -rf cangjie && cp -R $WORKSPACE/cangjie_compiler/output-x86_64-w64-mingw32 cangjie;
cp $WORKSPACE/cangjie_tools/cjpm/dist/cjpm.exe cangjie/tools/bin;
mkdir -p cangjie/tools/config;
cp $WORKSPACE/cangjie_tools/cjfmt/build/build/bin/cjfmt.exe cangjie/tools/bin;
cp $WORKSPACE/cangjie_tools/cjfmt/config/*.toml cangjie/tools/config;
cp $WORKSPACE/cangjie_tools/hyperlangExtension/target/bin/main.exe cangjie/tools/bin/hle.exe;
cp -r $WORKSPACE/cangjie_tools/hyperlangExtension/src/dtsparser cangjie/tools;
rm -rf cangjie/tools/dtsparser/*.cj;
cp $WORKSPACE/cangjie_tools/cangjie-language-server/output/bin/LSPServer.exe cangjie/tools/bin;
cp $WORKSPACE/cangjie_compiler/LICENSE cangjie;
cp $WORKSPACE/cangjie_compiler/Open_Source_Software_Notice.docx cangjie;
chmod -R 750 cangjie;
find cangjie -print0 | xargs -0r touch -t "$BEP_BUILD_TIME";
find cangjie -print0 | LC_ALL=C sort -z | xargs -0 zip -o -X $WORKSPACE/software/cangjie-sdk-windows-x64-${cangjie_version}.zip;

# 打包Cangjie STDX
cp -R $WORKSPACE/cangjie_stdx/target/windows_x86_64_cjnative ./;
cp $WORKSPACE/cangjie_stdx/LICENSE windows_x86_64_cjnative;
cp $WORKSPACE/cangjie_stdx/Open_Source_Software_Notice.docx windows_x86_64_cjnative;
chmod -R 750 windows_x86_64_cjnative;
find windows_x86_64_cjnative -print0 | xargs -0r touch -t "$BEP_BUILD_TIME";
find windows_x86_64_cjnative -print0 | LC_ALL=C sort -z | xargs -0 zip -o -X $WORKSPACE/software/cangjie-stdx-windows-x64-${cangjie_version}.${stdx_version}.zip;

chmod 550 *.zip;

ls -lh $WORKSPACE/software