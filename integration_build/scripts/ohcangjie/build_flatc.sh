#!/bin/bash

# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set -e;

mkdir build && cd build;
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release ..;
make flatc -j;
mkdir -p $1;
cp ./flatc ./$1;
zip -r $1.zip ./$1;
