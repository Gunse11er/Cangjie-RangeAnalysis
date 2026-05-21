# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# This script needs to be placed in the root directory of installation of Cangjie compiler and libraries.

# Set CANGJIE_HOME to the path of this batch script.
$env:CANGJIE_HOME = Split-Path -Parent $MyInvocation.MyCommand.Definition

# Windows searches for both binaries and libs in %Path%
$env:Path = $env:CANGJIE_HOME + "\runtime\lib\windows_x86_64_cjnative;" + $env:Path
$env:Path = $env:CANGJIE_HOME + "\lib\windows_x86_64_cjnative;" + $env:Path
$env:Path = $env:CANGJIE_HOME + "\bin;" + $env:Path 
$env:Path = $env:CANGJIE_HOME + "\tools\bin;" + $env:Path
$env:Path = $env:Path + ";" + $env:USERPROFILE + "\.cjpm\bin"
$env:Path = $env:CANGJIE_HOME + "\tools\lib;" + $env:Path
