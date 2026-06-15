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
# Prefer shell-provided variables when sourced, then fall back to ps.
if [ -n "${ZSH_VERSION-}" ]; then
    shell_name="zsh"
elif [ -n "${BASH_VERSION-}" ]; then
    shell_name="bash"
else
    shell_name="$(basename -- "$(ps -o comm= $$ 2>/dev/null)")"
fi

# Get the absolute path of this script according to different shells.
case "${shell_name}" in
    "zsh" | "-zsh")
        source_dir="${(%):-%N}"
        ;;
    "sh" | "-sh" | "bash" | "-bash")
        source_dir="${BASH_SOURCE[0]}"
        ;;
    *)
        echo "[ERROR] Unsupported shell: ${shell_name}, please switch to bash, sh or zsh."
        return 1
        ;;
esac

if [ -n "${source_dir}" ] && [ -L "${source_dir}" ]; then
    if command -v realpath >/dev/null 2>&1; then
        source_dir="$(realpath "${source_dir}")"
    else
        echo '`realpath` is not found, setup may not process properly.'
    fi
fi
script_dir="$(cd -P "$(dirname "${source_dir}")" 2>/dev/null && pwd -P)"
if [ -z "${script_dir}" ]; then
    echo "[ERROR] Failed to locate script directory."
    return 1
fi

# Get current hardware architecture
hw_arch=$(uname -m)
if [ "$hw_arch" = "" ]; then
    hw_arch="x86_64"
elif [ "$hw_arch" = "arm64" ]; then
    hw_arch="aarch64"
fi

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
    _remove_cangjie_paths "DYLD_LIBRARY_PATH"
    _remove_cangjie_paths "DYLD_FALLBACK_LIBRARY_PATH"
fi

export CANGJIE_HOME="${script_dir}"
export PATH="${CANGJIE_HOME}/bin:${CANGJIE_HOME}/tools/bin${PATH:+:${PATH}}:${HOME}/.cjpm/bin"
export DYLD_FALLBACK_LIBRARY_PATH="${CANGJIE_HOME}/runtime/lib/darwin_${hw_arch}_cjnative:${CANGJIE_HOME}/tools/lib${DYLD_FALLBACK_LIBRARY_PATH:+:${DYLD_FALLBACK_LIBRARY_PATH}}"
unset hw_arch

if [ -z ${SDKROOT+x} ]; then
    export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
fi

xattr -dr com.apple.quarantine "${script_dir}"/* &> /dev/null || true
codesign -s - -f --preserve-metadata=entitlements,requirements,flags,runtime "${script_dir}/third_party/llvm/bin/debugserver" &> /dev/null || true
