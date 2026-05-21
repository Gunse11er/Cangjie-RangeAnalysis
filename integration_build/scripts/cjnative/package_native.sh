#!/bin/bash

# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set -e;

# 清空历史构建
mkdir -p $WORKSPACE/software;
rm -rf $WORKSPACE/software/*;

# 打包Cangjie Frontend
cd $WORKSPACE/software;
mkdir -p cangjie/lib/${kernel}_${cmake_arch}_cjnative;
cp $WORKSPACE/cangjie_compiler/LICENSE cangjie;
cp $WORKSPACE/cangjie_compiler/Open_Source_Software_Notice.docx cangjie;
chmod -R 750 cangjie;
mv $WORKSPACE/cangjie_compiler/output/lib/${kernel}_${cmake_arch}_cjnative/libcangjie-ast-support.a cangjie/lib/${kernel}_${cmake_arch}_cjnative;
$tar \
  --sort=name --mtime="@${SOURCE_DATE_EPOCH}" \
  --owner=0 \
  --group=0 \
  --numeric-owner \
  --pax-option=exthdr.name=$d/PaxHeaders/%f,delete=ctime \
  -cf \
  - cangjie | gzip -n > cangjie-frontend-${os}-${arch_name}-${cangjie_version}.tar.gz;

# 打包Cangjie SDK
rm -rf cangjie && cp -R $WORKSPACE/cangjie_compiler/output cangjie;
cp $WORKSPACE/cangjie_tools/cjpm/dist/cjpm cangjie/tools/bin/cjpm;
mkdir -p cangjie/tools/config;
cp $WORKSPACE/cangjie_tools/cjfmt/build/build/bin/cjfmt cangjie/tools/bin;
cp $WORKSPACE/cangjie_tools/cjfmt/config/*.toml cangjie/tools/config;
cp $WORKSPACE/cangjie_tools/hyperlangExtension/target/bin/main cangjie/tools/bin/hle;
cp -r $WORKSPACE/cangjie_tools/hyperlangExtension/src/dtsparser cangjie/tools;
rm -rf cangjie/tools/dtsparser/*.cj;
cp $WORKSPACE/cangjie_tools/cangjie-language-server/output/bin/LSPServer cangjie/tools/bin;
cp $WORKSPACE/cangjie_compiler/LICENSE cangjie;
cp $WORKSPACE/cangjie_compiler/Open_Source_Software_Notice.docx cangjie;
chmod -R 750 cangjie;
$tar \
  --sort=name --mtime="@${SOURCE_DATE_EPOCH}" \
  --owner=0 \
  --group=0 \
  --numeric-owner \
  --pax-option=exthdr.name=$d/PaxHeaders/%f,delete=ctime \
  -cf \
  - cangjie | gzip -n > cangjie-sdk-${os}-${arch_name}-${cangjie_version}.tar.gz;

# 打包Cangjie STDX
cp -R $WORKSPACE/cangjie_stdx/target/${kernel}_${cmake_arch}_cjnative ./;
cp $WORKSPACE/cangjie_stdx/LICENSE ${kernel}_${cmake_arch}_cjnative;
cp $WORKSPACE/cangjie_stdx/Open_Source_Software_Notice.docx ${kernel}_${cmake_arch}_cjnative;
chmod -R 750 ${kernel}_${cmake_arch}_cjnative;
find ${kernel}_${cmake_arch}_cjnative -print0 | xargs -0r touch -t "$BEP_BUILD_TIME";
find ${kernel}_${cmake_arch}_cjnative -print0 | LC_ALL=C sort -z | xargs -0 zip -o -X $WORKSPACE/software/cangjie-stdx-${os}-${arch_name}-${cangjie_version}.${stdx_version}.zip;

chmod 550 *.tar.gz *.zip;

ls -lh $WORKSPACE/software