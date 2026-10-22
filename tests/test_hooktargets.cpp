// Dry test: hook-target inventory. Reads Taskbar.View.dll from disk (no injection,
// no explorer contact) and verifies every hook RVA used by taskbar.dll:
//   1) RVA lands in .text (a Windows update moving code invalidates our table);
//   2) PDB GUID/age in the debug directory match the pinned build.
// Prints a table; exit 0 only if ALL targets check out.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond)                                        \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("FAIL line %d: %s\n", __LINE__, #cond); \
            g_fail++;                                      \
        }                                                  \
    } while (0)

struct Target {
    const char* name;
    DWORD rva;
};

// Полный список целей хуков из hooks.cpp (Taskbar.View.dll).
static const Target kTargets[] = {
    {"GetFrameSize", 0x7F4B0},
    {"GetTrayFrameSize", 0x26CE2C},
    {"GetIconHeightInViewPixels", 0x1CA628},
    {"GetIconHeightMethod", 0x280BD4},
    {"ResourceLookup", 0x98970},
    {"GetMetrics", 0x37F3D8},
    {"IsStorageRecreationRequired", 0x914D0},
    {"UpdateButtonPadding", 0x376AFC},
    {"UpdateIconColumnDefinition", 0x376DF4},
    {"UpdateVisualStates", 0x13E54},
    {"IconViewCtor", 0xB6A50},
    {"NotifyIconViewCtor", 0x2DE748},
};

// Pinned build: Taskbar.View.dll PDB identity (must match hooks.cpp kViewGuid/kViewAge).
static const char* kWantGuid = "0AE0E302E8E34BE29CC303A7A62ABCA7";
static const DWORD kWantAge = 1;

int wmain(int argc, wchar_t** argv) {
    std::wstring path =
        argc > 1 ? argv[1]
                 : L"C:\\Windows\\SystemApps\\"
                   L"MicrosoftWindows.Client.Core_cw5n1h2txyewy\\Taskbar.View.dll";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        printf("SKIP: cannot open %ls (GLE=%lu)\n", path.c_str(), GetLastError());
        return 0; // dry test must not fail on machines without the file
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    std::vector<uint8_t> img((size_t)sz.QuadPart);
    DWORD rd = 0;
    bool ok = ReadFile(f, img.data(), (DWORD)img.size(), &rd, nullptr) && rd == img.size();
    CloseHandle(f);
    CHECK(ok);
    if (!ok) return 1;

    auto dos = (IMAGE_DOS_HEADER*)img.data();
    CHECK(dos->e_magic == IMAGE_DOS_SIGNATURE);
    auto nt = (IMAGE_NT_HEADERS64*)(img.data() + dos->e_lfanew);
    CHECK(nt->Signature == IMAGE_NT_SIGNATURE);
    CHECK(nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64);

    auto secs = IMAGE_FIRST_SECTION(nt);
    DWORD textVa = 0, textVs = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!memcmp(secs[i].Name, ".text", 5)) {
            textVa = secs[i].VirtualAddress;
            textVs = secs[i].Misc.VirtualSize;
        }
    }
    CHECK(textVa != 0);
    printf("%-28s %-10s %s\n", "target", "rva", ".text?");
    for (const auto& t : kTargets) {
        bool inText = t.rva >= textVa && t.rva - textVa < textVs;
        printf("%-28s 0x%-8lx %s\n", t.name, (unsigned long)t.rva,
               inText ? "yes" : "NO");
        CHECK(inText);
    }

    // PDB GUID/age from the CodeView record.
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    CHECK(dir.VirtualAddress != 0);
    bool guidOk = false;
    if (dir.VirtualAddress != 0) {
        // RVA -> file offset via sections.
        DWORD off = 0;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            DWORD va = secs[i].VirtualAddress;
            if (dir.VirtualAddress >= va &&
                dir.VirtualAddress < va + secs[i].Misc.VirtualSize) {
                off = secs[i].PointerToRawData + (dir.VirtualAddress - va);
                break;
            }
        }
        CHECK(off != 0);
        if (off) {
            auto entries = (IMAGE_DEBUG_DIRECTORY*)(img.data() + off);
            size_t count = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
            for (size_t i = 0; i < count; i++) {
                auto& e = entries[i];
                if (e.Type != IMAGE_DEBUG_TYPE_CODEVIEW || e.SizeOfData < 24) continue;
                // AddressOfRawData is an RVA here; map it too.
                DWORD poff = 0;
                for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++) {
                    DWORD va = secs[s].VirtualAddress;
                    if (e.AddressOfRawData >= va &&
                        e.AddressOfRawData < va + secs[s].Misc.VirtualSize) {
                        poff = secs[s].PointerToRawData + (e.AddressOfRawData - va);
                        break;
                    }
                }
                if (!poff) continue;
                const uint8_t* p = img.data() + poff;
                if (memcmp(p, "RSDS", 4) != 0) continue;
                uint32_t d1;
                uint16_t d2, d3;
                uint32_t age;
                memcpy(&d1, p + 4, 4);
                memcpy(&d2, p + 8, 2);
                memcpy(&d3, p + 10, 2);
                memcpy(&age, p + 20, 4);
                char guid[33];
                int n = snprintf(guid, sizeof(guid), "%08lX%04X%04X", (unsigned long)d1,
                                 d2, d3);
                for (int b = 0; b < 8; b++)
                    n += snprintf(guid + n, sizeof(guid) - n, "%02X", p[12 + b]);
                printf("pdb guid=%s age=%lu (want %s/%lu)\n", guid, (unsigned long)age,
                       kWantGuid, (unsigned long)kWantAge);
                guidOk = (strcmp(guid, kWantGuid) == 0 && age == kWantAge);
            }
        }
    }
    CHECK(guidOk);

    if (!g_fail) printf("hook targets PASS (%d targets)\n", (int)(sizeof(kTargets) / sizeof(kTargets[0])));
    return g_fail ? 1 : 0;
}
