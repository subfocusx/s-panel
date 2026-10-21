// pdatascan: offline feasibility probe. Read-only: parses Taskbar.dll PE,
// counts .pdata functions, scans .text for the mod's distinctive patterns.
// Usage: pdatascan.exe [path-to-Taskbar.dll]
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

static uint8_t* g_img = nullptr;
static size_t g_size = 0;
static uint8_t* g_text = nullptr;
static size_t g_textSz = 0;
static DWORD g_textRva = 0;
struct Func {
    DWORD begin, end;
};
static std::vector<Func> g_funcs;

static bool LoadFile(const wchar_t* path) {
    HANDLE h =
        CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    g_size = (size_t)sz.QuadPart;
    g_img = new uint8_t[g_size];
    DWORD rd = 0;
    BOOL ok = ReadFile(h, g_img, (DWORD)g_size, &rd, nullptr);
    CloseHandle(h);
    return ok && rd == g_size;
}

static const char* FindFunc(DWORD rva) {
    static char buf[64];
    for (auto& f : g_funcs)
        if (rva >= f.begin && rva < f.end) {
            snprintf(buf, sizeof buf, "func@RVA_%x..%x", f.begin, f.end);
            return buf;
        }
    return "no-func";
}

int wmain(int argc, wchar_t** argv) {
    std::wstring path = argc > 1 ? argv[1]
                                 : L"C:\\Windows\\System32\\Taskbar.dll";
    if (!LoadFile(path.c_str())) {
        printf("load failed\n");
        return 1;
    }
    auto dos = (IMAGE_DOS_HEADER*)g_img;
    auto nt = (IMAGE_NT_HEADERS64*)(g_img + dos->e_lfanew);
    auto secs = IMAGE_FIRST_SECTION(nt);
    int nsec = nt->FileHeader.NumberOfSections;
    auto& exc = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto pdata = (IMAGE_RUNTIME_FUNCTION_ENTRY*)(g_img + [&] {
        for (int i = 0; i < nsec; i++)
            if (secs[i].VirtualAddress <= exc.VirtualAddress &&
                exc.VirtualAddress < secs[i].VirtualAddress + secs[i].SizeOfRawData)
                return secs[i].PointerToRawData +
                       (exc.VirtualAddress - secs[i].VirtualAddress);
        return (DWORD)0;
    }());
    size_t nfunc = exc.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
    for (size_t i = 0; i < nfunc; i++)
        g_funcs.push_back({pdata[i].BeginAddress, pdata[i].EndAddress});
    for (int i = 0; i < nsec; i++)
        if (!memcmp(secs[i].Name, ".text", 5)) {
            g_text = g_img + secs[i].PointerToRawData;
            g_textSz = secs[i].SizeOfRawData;
            g_textRva = secs[i].VirtualAddress;
        }
    printf("functions: %zu  .text: %zu bytes\n", nfunc, g_textSz);

    // P1b: broad UCOMISD search: any REX?,66,0F,2E ... 7A within 12 bytes.
    {
        int n = 0;
        for (size_t i = 0; i + 14 < g_textSz; i++) {
            uint8_t* p = g_text + i;
            size_t o = 0;
            if (p[0] >= 0x40 && p[0] <= 0x4F) o = 1; // REX
            if (p[o] == 0x66 && p[o + 1] == 0x0F && p[o + 2] == 0x2E) {
                for (int k = 3; k < 12; k++)
                    if (p[o + k] == 0x7A) {
                        printf("P1b UCOMISD fileoff=0x%zx rva=0x%x JP@+%d in %s\n",
                               i, g_textRva + (DWORD)i, (int)(o + k),
                               FindFunc(g_textRva + (DWORD)i));
                        if (++n > 14) break;
                        break;
                    }
            }
            if (n > 14) break;
        }
        printf("P1b total(shown<=15): %d\n", n);
    }
    int p1 = 0;
    for (size_t i = 0; i + 14 < g_textSz; i++) {
        uint8_t* p = g_text + i;
        if (p[0] == 0x66 && p[1] == 0x0F && p[2] == 0x2E &&
            (p[3] & 0xC0) == 0x80 && p[8] == 0x7A &&
            (p[10] == 0x74 || p[10] == 0x75 ||
             (p[10] == 0x0F && (p[11] == 0x84 || p[11] == 0x85)))) {
            printf("P1 hit fileoff=0x%zx rva=0x%x lastH=0x%x in %s\n", i,
                   g_textRva + (DWORD)i, *(int32_t*)(p + 4),
                   FindFunc(g_textRva + (DWORD)i));
            if (++p1 > 8) break;
        }
    }
    printf("P1 total(shown<=9): %d\n", p1);

    // P2: sub rsp,28h at func start + mov r?x,[rcx+0x28x] (GetTaskbarFrameOffset)
    int p2 = 0;
    for (auto& f : g_funcs) {
        if (f.begin < g_textRva || f.begin >= g_textRva + g_textSz) continue;
        uint8_t* p = g_text + (f.begin - g_textRva);
        if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC && p[3] == 0x28 &&
            (p[4] == 0x48 || p[4] == 0x4C) && p[5] == 0x8B &&
            (p[6] & 0xC0) == 0x80) {
            LONG off = *(LONG*)(p + 7);
            printf("P2 hit func RVA_0x%x taskbarFrameOff=0x%x\n", f.begin, off);
            if (++p2 > 8) break;
        }
    }
    printf("P2 total(shown<=9): %d\n", p2);

    // P3: locate double 48.0 in .rdata + code refs (GetFrameSize anchor)
    DWORD c48rva = 0;
    int c48 = 0;
    for (int i = 0; i < nsec; i++)
        if (!memcmp(secs[i].Name, ".rdata", 6)) {
            uint8_t* r = g_img + secs[i].PointerToRawData;
            for (DWORD j = 0; j + 8 < secs[i].SizeOfRawData; j++)
                if (!memcmp(r + j, "\x00\x00\x00\x00\x00\x00\x48\x40", 8)) {
                    printf("C48 .rdata RVA_0x%x\n", secs[i].VirtualAddress + j);
                    if (!c48rva) c48rva = secs[i].VirtualAddress + j;
                    if (++c48 > 4) break;
                }
        }
    printf("C48 total(shown<=5): %d\n", c48);

    // P4: GetFrameSize = function with (cmp ecx,1) + (cmp ecx,2) + RIP-ref to C48.
    // RIP-refs: MOV r64,[rip+d] (48 8B /0) and MOVSD xmm,[rip+d] (F2 0F 10 /0).
    if (c48rva) {
        int p4 = 0, nCmp = 0, nRef = 0;
        for (auto& f : g_funcs) {
            if (f.begin < g_textRva || f.end > g_textRva + (DWORD)g_textSz ||
                f.end - f.begin > 0x500)
                continue;
            uint8_t* b = g_text + (f.begin - g_textRva);
            size_t len = f.end - f.begin;
            bool hasCmp1 = false, hasCmp2 = false, hasRef = false;
            for (size_t k = 0; k + 7 < len; k++) {
                if (b[k] == 0x83 && b[k + 1] == 0xF9 && b[k + 2] == 0x01)
                    hasCmp1 = true;
                if (b[k] == 0x83 && b[k + 1] == 0xF9 && b[k + 2] == 0x02)
                    hasCmp2 = true;
                if (b[k] == 0x48 && b[k + 1] == 0x8B &&
                    (b[k + 2] & 0xC7) == 0x05) {
                    int32_t d = *(int32_t*)(b + k + 3);
                    if (f.begin + (DWORD)k + 7 + d == c48rva) hasRef = true;
                }
                if (b[k] == 0xF2 && b[k + 1] == 0x0F && b[k + 2] == 0x10 &&
                    (b[k + 3] & 0xC7) == 0x05) {
                    int32_t d = *(int32_t*)(b + k + 4);
                    if (f.begin + (DWORD)k + 8 + d == c48rva) hasRef = true;
                }
            }
            if (hasCmp1 && hasCmp2) {
                if (nCmp < 12)
                    printf("P4cmp func RVA_0x%x..0x%x refC48=%d\n", f.begin,
                           f.end, (int)hasRef);
                nCmp++;
            }
            if (hasRef) {
                if (nRef < 12)
                    printf("P4ref func RVA_0x%x..0x%x cmp1=%d cmp2=%d\n",
                           f.begin, f.end, (int)hasCmp1, (int)hasCmp2);
                nRef++;
            }
            if (hasCmp1 && hasCmp2 && hasRef) {
                printf("P4 GetFrameSize candidate func RVA_0x%x..0x%x\n",
                       f.begin, f.end);
                if (++p4 > 8) break;
            }
        }
        printf("P4 cmp12=%d refC48=%d both=%d\n", nCmp, nRef, p4);
    }
    // P5: hexdump helper for named RVAs (manual decode).
    {
        DWORD targets[] = {0x2e7a0, 0x18bf34};
        for (DWORD t : targets) {
            for (auto& f : g_funcs)
                if (t >= f.begin && t < f.end) {
                    printf("--- dump func RVA_0x%x..0x%x ---\n", f.begin,
                           f.end);
                    uint8_t* b = g_text + (f.begin - g_textRva);
                    size_t len = f.end - f.begin;
                    if (len > 256) len = 256;
                    for (size_t k = 0; k < len; k += 16) {
                        printf("%05zx:", k);
                        for (size_t j = k; j < k + 16 && j < len; j++)
                            printf(" %02x", b[j]);
                        printf("\n");
                    }
                }
        }
    }
    return 0;
}
