#!/bin/bash

# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# This script needs to be placed in the output directory of Cangjie compiler.
# ** NOTE: Please use `source' command to execute this script. **
if ! (return 0 2>/dev/null); then
    echo "[ERROR] This script must be sourced, not executed directly."
    echo "        Please run: 'source $0' or '. $0'"
    exit 1
fi

# Get current shell name.
shell_path=$(readlink -f /proc/$$/exe)
shell_name=${shell_path##*/}

# Get the absolute path of this script according to different shells.
case "${shell_name}" in
    "zsh")
        # check whether compinit has been executed 
        if (( ${+_comps} )); then
            # if compinit already executed, delete completion functions of cjc, cjc-frontend firstly
            compdef -d cjc cjc-frontend
        else
            autoload -Uz compinit
            compinit
        fi

        # auto complete cjc, cjc-frontend
        compdef _gnu_generic cjc cjc-frontend
        script_dir=$(cd "$(dirname "${(%):-%N}")"; pwd)
        ;;
    "sh" | "bash")
        script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")"; pwd)
        ;;
    *)
        echo "[ERROR] Unsupported shell: ${shell_name}, please switch to bash, sh or zsh."
        return 1
        ;;
esac

# Remove Cangjie-related paths from an environment variable
_remove_cangjie_paths() {
    env_var_name="$1"
    env_var_value=""

    # Read environment variable by name in a way that works in both bash and zsh.
    eval "env_var_value=\${$env_var_name}"

    if [ -n "${env_var_value}" ]; then
        # IMPORTANT: use explicit ':' splitting with parameter expansion.
        # Do not rely on shell word-splitting behavior, which differs across sh/bash/zsh
        item=""
        new_env_var_value=""
        remaining="${env_var_value}"
        while :; do
            case "${remaining}" in
                *:*)
                    item="${remaining%%:*}"
                    remaining="${remaining#*:}"
                    ;;
                *)
                    item="${remaining}"
                    remaining=""
                    ;;
            esac
            case "${item}" in
                "${CANGJIE_HOME}/"*) ;;
                "${HOME}/.cjpm/bin") ;;
                *)
                    new_env_var_value="${new_env_var_value}${new_env_var_value:+:}${item}"
                    ;;
            esac
            [ -n "${remaining}" ] || break
        done
        eval "export ${env_var_name}=\"\${new_env_var_value}\""
    fi
}
if [ -n "${CANGJIE_HOME-}" ]; then
    _remove_cangjie_paths "PATH"
fi

export CANGJIE_HOME="${script_dir}"
export PATH="${CANGJIE_HOME}/bin:${CANGJIE_HOME}/tools/bin:${CANGJIE_HOME}/tools/lib:${CANGJIE_HOME}/runtime/lib/windows_x86_64_cjnative:${CANGJIE_HOME}/third_party/llvm/lldb/lib:${PATH}:${HOME}/.cjpm/bin"
