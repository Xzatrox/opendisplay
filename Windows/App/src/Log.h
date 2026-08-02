#pragma once
// Simple file logger for OpenDisplay Windows app.
// Writes to OpenDisplay.log next to the executable.
// Thread-safe via mutex.

#include <fstream>
#include <mutex>
#include <string>
#include <chrono>
#include <sstream>
#include <iomanip>

class Log {
public:
    static void Init() {
        std::lock_guard<std::mutex> lock(Mutex());
        if (!Stream().is_open()) {
            // Log file next to the executable
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring wpath(path);
            auto pos = wpath.find_last_of(L'\\');
            if (pos != std::wstring::npos) wpath = wpath.substr(0, pos + 1);
            wpath += L"OpenDisplay.log";
            Stream().open(wpath, std::ios::out | std::ios::trunc);
            if (Stream().is_open()) {
                Stream() << Timestamp() << " [INIT] Log started\n";
                Stream().flush();
            }
        }
    }

    static void Info(const std::string& msg) {
        Write("INFO", msg);
    }

    static void Error(const std::string& msg) {
        Write("ERROR", msg);
    }

    static void Debug(const std::string& msg) {
#ifdef _DEBUG
        Write("DEBUG", msg);
#else
        (void)msg;
#endif
    }

    static void Flush() {
        std::lock_guard<std::mutex> lock(Mutex());
        if (Stream().is_open()) Stream().flush();
    }

private:
    static void Write(const char* level, const std::string& msg) {
        std::lock_guard<std::mutex> lock(Mutex());
        if (Stream().is_open()) {
            Stream() << Timestamp() << " [" << level << "] " << msg << "\n";
            Stream().flush();
        }
    }

    static std::string Timestamp() {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        std::tm tm = {};
        localtime_s(&tm, &t);
        std::ostringstream ss;
        ss << std::put_time(&tm, "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count();
        return ss.str();
    }

    static std::ofstream& Stream() {
        static std::ofstream s;
        return s;
    }

    static std::mutex& Mutex() {
        static std::mutex m;
        return m;
    }
};
