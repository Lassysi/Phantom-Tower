#include "tower_language.h"

#include <windows.h>
#include <string>

namespace tower::language {
namespace {
Language current = Language::Chinese;
bool loaded = false;

std::wstring Directory() {
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!n || n > 32768) return {};
    std::wstring path(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", path.data(), n);
    if (!got || got >= n) return {};
    path.resize(got);
    return path + L"\\PBZTower";
}
void Load() {
    if (loaded) return;
    loaded = true;
    current = DefaultForWindowsLanguage(GetUserDefaultUILanguage());
    const auto dir = Directory();
    if (dir.empty()) return;
    const auto path = dir + L"\\language.txt";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char bytes[33]{};
    DWORD n = 0;
    const bool ok = ReadFile(f, bytes, sizeof bytes, &n, nullptr) && n < sizeof bytes;
    CloseHandle(f);
    if (ok) current = Parse(std::string_view(bytes, n));
}
}  // namespace

Language Current() { Load(); return current; }

bool Set(Language value) {
    Load();
    const auto dir = Directory();
    if (dir.empty() || (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)) return false;
    const auto path = dir + L"\\language.txt";
    // PID-scoped temporary file; replacement is a single operation in the same directory.
    const auto temp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const auto content = Encode(value);
    DWORD n = 0;
    const bool written = WriteFile(f, content.data(), static_cast<DWORD>(content.size()), &n, nullptr) &&
                         n == content.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!written || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    current = value;
    return true;
}
}  // namespace tower::language
