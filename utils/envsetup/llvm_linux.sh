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

# Detect the current shell type
_cj_detect_shell_name() {
    # Prefer shell-provided version variables when sourced
    if [ -n "${ZSH_VERSION:-}" ]; then
        printf '%s\n' "zsh"
        return 0
    fi
    if [ -n "${BASH_VERSION:-}" ]; then
        printf '%s\n' "bash"
        return 0
    fi

    # Fallback: detect by process name. In some container setups /proc/$$/exe can be misleading
    # (e.g. rosetta), so we avoid it.
    if command -v ps >/dev/null 2>&1; then
        comm="$(ps -o comm= -p "$$" 2>/dev/null | tr -d '\n')"
        comm="${comm##*/}"
        case "$comm" in
            -*) comm="${comm#-}" ;;
        esac
        case "$comm" in
            zsh|bash) printf '%s\n' "$comm"; return 0 ;;
            sh|dash|ash|busybox) printf '%s\n' "sh"; return 0 ;;
        esac
    fi

    # If ps is not available, try reading from /proc filesystem
    if [ -r "/proc/$$/comm" ]; then
        comm="$(tr -d '\n' < "/proc/$$/comm" 2>/dev/null)"
        comm="${comm##*/}"
        case "$comm" in
            -*) comm="${comm#-}" ;;
        esac
        case "$comm" in
            zsh|bash) printf '%s\n' "$comm"; return 0 ;;
            sh|dash|ash|busybox) printf '%s\n' "sh"; return 0 ;;
        esac
    fi

    # Unrecognized shell
    printf '%s\n' "unknown"
    return 0
}

# Resolve relative path to absolute path
_cj_resolve_abs_path() {
    input="$1"
    [ -n "$input" ] || return 1

    # Handle relative and absolute paths
    case "${input}" in
        /*) p="$input" ;;
        *) p="$PWD/$input" ;;
    esac

    # Try multiple tools to resolve absolute path, in order of preference:
    # 1. realpath
    # 2. readlink -f
    # 3. python3
    # 4. python
    # 5. pure shell implementation
    if command -v realpath >/dev/null 2>&1; then
        realpath "$p" 2>/dev/null && return 0
    fi
    if command -v readlink >/dev/null 2>&1; then
        readlink -f "$p" 2>/dev/null && return 0
    fi
    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$p" 2>/dev/null && return 0
    fi
    if command -v python >/dev/null 2>&1; then
        python -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$p" 2>/dev/null && return 0
    fi

    # Pure shell implementation: cd to directory then pwd
    d="${p%/*}"
    b="${p##*/}"
    (cd -P "$d" 2>/dev/null && printf '%s/%s\n' "$(pwd -P)" "$b") || return 1
}

# Get absolute path of the script directory from the script path
_cj_script_dir_from_path() {
    p="$1"
    [ -n "$p" ] || return 1

    # First resolve to absolute path
    abs="$(_cj_resolve_abs_path "$p" 2>/dev/null || true)"
    [ -n "$abs" ] || abs="$p"

    # Get directory part
    d="${abs%/*}"
    [ -n "$d" ] || d="."
    # Change to directory and output absolute path
    (cd -P "$d" 2>/dev/null && pwd -P) || return 1
}

# ==================== MAIN LOGIC ====================

# Detect current shell type
shell_name="$(_cj_detect_shell_name)"

# Get script directory based on different shell types
case "$shell_name" in
    zsh)
        # zsh completion setup (zsh-only syntax stays in eval for POSIX sh compatibility)
        eval '
            if (( ${+_comps} )); then
                compdef -d cjc cjc-frontend
            else
                autoload -Uz compinit
                compinit
            fi
            compdef _gnu_generic cjc cjc-frontend
        ' 2>/dev/null || true

        # ${(%):-%N} is zsh-specific syntax to get current script path
        # Note: This is intentionally not wrapped in eval to get the correct script path
        # This relies on the fact that most modern shells won't fail at parse time for
        # unknown parameter expansions when they're in an unexecuted code branch
        script_dir="$(_cj_script_dir_from_path "${(%):-%N}" 2>/dev/null || true)"
        if [ -z "$script_dir" ]; then
            echo "[ERROR] Failed to locate script directory in zsh." >&2
            return 1
        fi
        ;;
    bash)
        # shellcheck disable=SC3054
        # ${BASH_SOURCE[0]} is bash-specific syntax to get current script path
        script_dir="$(_cj_script_dir_from_path "${BASH_SOURCE[0]}" 2>/dev/null || true)"
        if [ -z "$script_dir" ]; then
            echo "[ERROR] Failed to locate script directory in bash." >&2
            return 1
        fi
        ;;
    sh)
        # Check if script is executed directly (not sourced)
        case "$0" in
            *"envsetup.sh")
                echo "[ERROR] This script must be sourced, not executed directly."
                echo "        Please run: 'source $0' or '. $0'"
                return 1
                ;;
        esac
        # POSIX sh (dash/ash/busybox sh) cannot reliably know the sourced filename.
        # Require running from the directory containing envsetup.sh.
        if [ -f "envsetup.sh" ]; then
            script_dir="$(pwd -P 2>/dev/null || pwd)"
        else
            echo "[ERROR] Switch to the directory containing envsetup.sh and run it." >&2
            return 1
        fi
        ;;
    *)
        echo "[ERROR] Unsupported shell: ${shell_name}, please switch to bash, sh or zsh." >&2
        return 1
        ;;
esac

# ==================== ENVIRONMENT VARIABLE CONFIGURATION ====================

# Detect system name
sys_name=$(uname -s)
if [ "$sys_name" = "HarmonyOS" ]; then
    sys_name="_ohos"
else
    sys_name=""
fi

# Detect hardware architecture
hw_arch=$(uname -m)
if [ "$hw_arch" = "" ]; then
    hw_arch="x86_64"
fi
hw_arch="${sys_name}_${hw_arch}"

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
    _remove_cangjie_paths "LD_LIBRARY_PATH"
fi

# Set Cangjie related environment variables
export CANGJIE_HOME="${script_dir}"
export PATH="${CANGJIE_HOME}/bin:${CANGJIE_HOME}/tools/bin${PATH:+:${PATH}}:${HOME}/.cjpm/bin"
export LD_LIBRARY_PATH="${CANGJIE_HOME}/runtime/lib/linux${hw_arch}_cjnative:${CANGJIE_HOME}/tools/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

# Clean up temporary variables
unset hw_arch
unset sys_name
