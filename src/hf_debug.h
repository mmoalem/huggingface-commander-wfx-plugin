//---------------------------------------------------------------------------
// hf_debug.h  –  Diagnostic logging to %TEMP%\hf_vfs_debug.log
//---------------------------------------------------------------------------
#pragma once
#include <windows.h>
#include <string>
#include <sstream>
#include <cstdio>

namespace HfDebug {

inline std::string LogPath() {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + "hf_vfs_debug.log";
}

inline void Log(const std::string& msg) {
    FILE* f = nullptr;
    fopen_s(&f, LogPath().c_str(), "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            msg.c_str());
    fclose(f);
}

inline void Log(const std::string& label, int val) {
    Log(label + ": " + std::to_string(val));
}
inline void Log(const std::string& label, long long val) {
    Log(label + ": " + std::to_string(val));
}
inline void Log(const std::string& label, const std::string& val) {
    // Truncate very long strings (e.g. presigned URLs, base64)
    std::string v = val.size() > 300 ? val.substr(0, 300) + "..." : val;
    Log(label + ": " + v);
}

} // namespace HfDebug
