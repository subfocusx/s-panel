// test_resolve: resolver unit test on the REAL Taskbar.dll FILE bytes.
// No LoadLibrary (its DllMain needs explorer), no explorer contact.
// PASS = unique resolution to the validated RVAs (file 41326fff…).
#include "../src/dll/resolve.h"
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#pragma comment(lib, "version.lib")

static bool IsKnownBuild(const wchar_t* path) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (size == 0) return false;
    std::vector<uint8_t> ver(size);
    if (!GetFileVersionInfoW(path, 0, size, ver.data())) return false;
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(ver.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) || !ffi)
        return false;
    WORD major = HIWORD(ffi->dwFileVersionMS);
    WORD minor = LOWORD(ffi->dwFileVersionMS);
    WORD build = HIWORD(ffi->dwFileVersionLS);
    printf("file version %u.%u.%u.%u\n", major, minor, build, LOWORD(ffi->dwFileVersionLS));
    return major == 10 && minor == 0 && build == 26100;
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* path = argc >= 2 ? argv[1] : L"C:\\Windows\\System32\\Taskbar.dll";
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        printf("FAIL open GLE=%lu\n", GetLastError());
        return 1;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    uint8_t* buf = new uint8_t[(size_t)sz.QuadPart];
    DWORD rd = 0;
    ReadFile(h, buf, (DWORD)sz.QuadPart, &rd, nullptr);
    CloseHandle(h);

    ResolvedAddrs r{};
    bool ok = ResolveTaskbarAddrs(buf, rd, false, &r);
    delete[] buf;
    if (!ok) {
        printf("FAIL resolve refused\n");
        return 1;
    }
    printf("frame=0x%x icon=0x%x metrics=0x%x hash=0x%llx\n",
           r.getFrameSizeRva, r.getIconHeightRva, r.getMetricsRva,
           (unsigned long long)r.imageHash);
    if (!IsKnownBuild(path)) {
        printf("SKIP: build is not 10.0.26100.*, RVA check not applicable\n");
        return 0;
    }
    int fail = 0;
    if (r.getFrameSizeRva != 0x2e7a0) {
        printf("FAIL frame expected 0x2e7a0\n");
        fail = 1;
    }
    if (r.getIconHeightRva != 0x1a19dc) {
        printf("FAIL icon expected 0x1a19dc\n");
        fail = 1;
    }
    if (r.getMetricsRva != 0x1a057c) {
        printf("FAIL metrics expected 0x1a057c\n");
        fail = 1;
    }
    if (!fail) printf("resolve tests PASS\n");
    return fail;
}
