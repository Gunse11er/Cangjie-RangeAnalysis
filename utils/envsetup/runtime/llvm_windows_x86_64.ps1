# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# This script needs to be placed in the root directory of installation of Cangjie compiler and libraries.

$script_dir = Split-Path -Parent $MyInvocation.MyCommand.Definition

# Windows searches for both binaries and libs in %Path%
$env:Path = $script_dir + "\lib\windows_x86_64_cjnative;" + $env:Path
