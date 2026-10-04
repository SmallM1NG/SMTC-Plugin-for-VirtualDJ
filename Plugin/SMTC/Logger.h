#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <string>

namespace smtc {
enum class LogLevel { Debug, Info, Warning, Error };
inline const char* LevelName(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Warning: return "WARNING";
    case LogLevel::Error: return "ERROR";
    default: return "INFO";
    }
}
class LogSink {
    // 由外层 mutex 串行写入和关闭；仅写同目录单文件，超过 10 MiB 清空。
public:
    std::mutex mutex;
    ~LogSink() { Close(); }
    void Close() noexcept {
        if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
        disabled_ = false;
    }
    void Write(const char* line, DWORD length) {
        if (disabled_) return;
        if (path_.empty()) {
            wchar_t buffer[32768]{};
            HMODULE module{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&LevelName), &module)) return;
            const DWORD count = GetModuleFileNameW(module, buffer, _countof(buffer));
            if (!count || count >= _countof(buffer)) return;
            path_ = buffer;
            path_.resize(path_.find_last_of(L'\\') + 1);
            path_ += L"SMTC.log";
        }
        if (Append(line, length)) return;
        Close();
        // 写入失败后停止文件日志；显式关闭后才允许下一次启用重新尝试。
        disabled_ = true;
    }
private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
    bool disabled_ = false;
    unsigned long long bytes_ = 0;
    bool Append(const char* line, DWORD length) {
        if (file_ == INVALID_HANDLE_VALUE) {
            file_ = CreateFileW(path_.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file_ == INVALID_HANDLE_VALUE) return false;
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file_, &size)) { Close(); return false; }
            bytes_ = size.QuadPart;
            LARGE_INTEGER offset{};
            if (!SetFilePointerEx(file_, offset, nullptr, FILE_END)) return false;
        }
        if (bytes_ + length > 10ull * 1024 * 1024) {
            LARGE_INTEGER offset{};
            if (!SetFilePointerEx(file_, offset, nullptr, FILE_BEGIN) || !SetEndOfFile(file_)) return false;
            bytes_ = 0;
        }
        DWORD written{};
        if (!WriteFile(file_, line, length, &written, nullptr) || written != length) { Close(); return false; }
        bytes_ += written;
        return true;
    }
};
inline LogSink& Logger() { static LogSink sink; return sink; }
inline void CloseLog() noexcept {
    try { auto& sink = Logger(); std::lock_guard lock(sink.mutex); sink.Close(); } catch (...) {}
}
// 运行日志仅用英语，不记录曲名、路径、网址或本地化系统错误。
inline void LogEvent(LogLevel level, const char* category, const char* format, ...) noexcept {
    try {
        char content[1536]{};
        va_list args; va_start(args, format);
        _vsnprintf_s(content, sizeof(content), _TRUNCATE, format, args);
        va_end(args);
        for (auto& ch : content) if (ch && (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) > 126)) ch = '?';
        SYSTEMTIME time{}; GetSystemTime(&time);
        char line[2048]{};
        sprintf_s(line, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ [%s] [%s] [pid=%lu tid=%lu] %s\r\n",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
            LevelName(level), category, GetCurrentProcessId(), GetCurrentThreadId(), content);
        auto& sink = Logger();
        std::lock_guard lock(sink.mutex);
        OutputDebugStringA(line);
        sink.Write(line, static_cast<DWORD>(strlen(line)));
    } catch (...) { OutputDebugStringA("SMTC logging failed; diagnostic entry could not be written.\n"); }
}
}
