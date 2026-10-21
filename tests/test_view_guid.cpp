// test_view_guid: проверяет GUID-сверку на реальном Taskbar.View.dll как образе
// (LoadLibraryEx, без DllMain и без explorer). Ожидаемый GUID/age — константы DLL.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

static const char* kViewGuid = "0AE0E302E8E34BE29CC303A7A62ABCA7";
static const DWORD kViewAge = 1;

static bool ReadRsds(const uint8_t* base, char* guidOut, DWORD* ageOut) {
    auto dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!dir.VirtualAddress) return false;
    auto dbg = (const IMAGE_DEBUG_DIRECTORY*)(base + dir.VirtualAddress);
    size_t n = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (size_t i = 0; i < n; i++) {
        if (dbg[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        auto rsds = base + dbg[i].AddressOfRawData;
        if (memcmp(rsds, "RSDS", 4) != 0) continue;
        const uint8_t* g = rsds + 4;
        DWORD d1; WORD d2, d3;
        memcpy(&d1, g, 4); memcpy(&d2, g + 4, 2); memcpy(&d3, g + 6, 2);
        sprintf_s(guidOut, 33, "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
                  (unsigned long)d1, d2, d3, g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
        DWORD age;
        memcpy(&age, rsds + 20, 4);
        *ageOut = age;
        return true;
    }
    return false;
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* path = argc > 1 ? argv[1] : L"C:\\Windows\\SystemApps\\MicrosoftWindows.Client.Core_cw5n1h2txyewy\\Taskbar.View.dll";
    HMODULE m = LoadLibraryExW(path, nullptr, DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) {
        printf("FAIL LoadLibraryEx GLE=%lu\n", GetLastError());
        return 1;
    }
    auto base = (const uint8_t*)((uintptr_t)m & ~(uintptr_t)3);
    char guid[40] = {};
    DWORD age = 0;
    bool ok = ReadRsds(base, guid, &age);
    FreeLibrary(m);
    if (!ok) {
        printf("FAIL no RSDS\n");
        return 2;
    }
    printf("guid=%s age=%lu\n", guid, (unsigned long)age);
    if (strcmp(guid, kViewGuid) != 0 || age != kViewAge) {
        printf("FAIL guid/age mismatch, expected %s age=%lu\n", kViewGuid, (unsigned long)kViewAge);
        return 3;
    }
    printf("view-guid PASS\n");
    return 0;
}
