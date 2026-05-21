#!/bin/bash

# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set -e;

cd ${WORKSPACE}/cangjie_stdx;
unset LD_LIBRARY_PATH;
export CANGJIE_HOME=${WORKSPACE}/cangjie_compiler/output;
export PATH=${CANGJIE_HOME}/bin:${CANGJIE_HOME}/tools/bin:$PATH:${HOME}/.cjpm/bin;
export LD_LIBRARY_PATH=${CANGJIE_HOME}/runtime/lib/linux_$(arch)_cjnative:${CANGJIE_HOME}/tools/lib
python3 build.py clean;
python3 build.py build -t release \
    --include=${WORKSPACE}/cangjie_compiler/include \
    --target-lib=$OPENSSL_PATH \
    --target ohos-aarch64 \
    --target-sysroot ${OHOS_ROOT}/out/sdk/obj/third_party/musl/sysroot \
    --target-toolchain ${OHOS_ROOT}/prebuilts/clang/ohos/linux-x86_64/llvm/bin;
python3 build.py install;

# 打包Cangjie STDX
cp -R $WORKSPACE/cangjie_stdx/target/linux_ohos_aarch64_cjnative ./;
cp $WORKSPACE/cangjie_stdx/LICENSE linux_ohos_aarch64_cjnative;
cp $WORKSPACE/cangjie_stdx/Open_Source_Software_Notice.docx linux_ohos_aarch64_cjnative;
chmod -R 750 linux_ohos_aarch64_cjnative;
zip -qr $WORKSPACE/software/cangjie-stdx-ohos-aarch64-${cangjie_version}.${stdx_version}.zip linux_ohos_aarch64_cjnative;

python3 build.py clean;
python3 build.py build -t release \
    --include=${WORKSPACE}/cangjie_compiler/include \
    --target-lib=$OPENSSL_PATH \
    --target ohos-x86_64 \
    --target-sysroot  ${OHOS_ROOT}/out/sdk/obj/third_party/musl/sysroot \
    --target-toolchain ${OHOS_ROOT}/prebuilts/clang/ohos/linux-x86_64/llvm/bin;
python3 build.py install;

# # 打包Cangjie STDX
cp -R $WORKSPACE/cangjie_stdx/target/linux_ohos_x86_64_cjnative ./;
cp $WORKSPACE/cangjie_stdx/LICENSE linux_ohos_x86_64_cjnative;
cp $WORKSPACE/cangjie_stdx/Open_Source_Software_Notice.docx linux_ohos_x86_64_cjnative;
chmod -R 750 linux_ohos_x86_64_cjnative;
zip -qr $WORKSPACE/software/cangjie-stdx-ohos-x86_64-${cangjie_version}.${stdx_version}.zip linux_ohos_x86_64_cjnative;