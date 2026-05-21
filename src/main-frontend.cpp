// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <iomanip>
#include <vector>
#include <windows.h>
#include <sstream>

namespace {

std::string SetDoubleQuoted(const std::string& str)
{
    std::stringstream ss;
    ss << "\"";
    for (char c : str) {
        // backslash cannot be used as escape character in Shell Command Language. To be able to
        // use double quote in a command, we generate backslash string and join them with
        // `"`. For example, ab"cd is transformed to "ab\"cd"; ab\cd is transformed to "ab"\\"cd".
        if (c == '"') {
            ss << "\\\"";
        } else if (c == '\\') {
            ss << "\"\\\\\"";
        } else {
            ss << c;
        }
    }
    ss << "\"";
    return ss.str();
}

} // namespace

/**
 * This simple program executes (exact*) the executing command with a different executable.
 * To be specific, the program starts `cjc.exe` to process the command, but without changes
 * the executable name, i.e. argv[0], to `cjc.exe`. If `cjc.exe` is started by this program,
 * when `cjc.exe` checks `argv[0]`, it would get the current program name, not `cjc.exe`.
 */
int main(int argc, const char** argv)
{
    // Since we are using c++, there is no reason to handle unsafe char * instead of std::string.
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    // Retrive user command by concatenating all arguments.
    std::ostringstream oss;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            oss << " ";
        }
        oss << SetDoubleQuoted(args[i]);
    }
    std::string commandLine = oss.str();

    // To keep the exact same behavior with symbolic link, we access the `cjc.exe` which located
    // in the same directory current program in.
    char buffer[MAX_PATH];
    GetModuleFileName(NULL, buffer, MAX_PATH);
    std::string exePath = std::string(buffer);
    auto pos = exePath.find_last_of('\\');
    if (pos == std::string::npos) {
        return 1;
    }
    std::string cjcPath = exePath.substr(0, pos) + "\\cjc.exe";

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(cjcPath.c_str(), commandLine.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code;
    if (FALSE == GetExitCodeProcess(pi.hProcess, &exit_code)) {
        return 1;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (int)exit_code;
}
