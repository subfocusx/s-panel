// callgraph: HDE-based linear sweep over .pdata functions of Taskbar.dll.
// Read-only. Reports: tiny movsd getters, cmp ecx,1&2 funcs, UCOMISD sites.
#include <windows.h>
#include <wincrypt.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include "../vendor/minhook/src/hde/hde64.h"

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
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
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

template <typename Cb>
static void WalkFunc(const Func& f, Cb cb) {
    if (f.begin < g_textRva || f.end > g_textRva + (DWORD)g_textSz) return;
    uint8_t* b = g_text + (f.begin - g_textRva);
    size_t len = f.end - f.begin;
    size_t k = 0;
    while (k < len) {
        hde64s hs{};
        unsigned l = hde64_disasm(b + k, &hs);
        if (!l || (hs.flags & 0x0001F000) || k + l > len) break;
        bool isRet = (hs.opcode == 0xC3 || hs.opcode == 0xC2);
        cb(b + k, f.begin + (DWORD)k, hs);
        if (isRet) break;
        k += l;
        if (k > 0x4000) break;
    }
}

int wmain(int argc, wchar_t** argv) {
    std::wstring path =
        argc > 1 ? argv[1] : L"C:\\Windows\\System32\\Taskbar.dll";
    if (!LoadFile(path.c_str())) {
        printf("load failed\n");
        return 1;
    }
    auto dos = (IMAGE_DOS_HEADER*)g_img;
    auto nt = (IMAGE_NT_HEADERS64*)(g_img + dos->e_lfanew);
    auto secs = IMAGE_FIRST_SECTION(nt);
    int nsec = nt->FileHeader.NumberOfSections;
    auto& exc =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    DWORD pdataRaw = 0;
    for (int i = 0; i < nsec; i++)
        if (secs[i].VirtualAddress <= exc.VirtualAddress &&
            exc.VirtualAddress <
                secs[i].VirtualAddress + secs[i].SizeOfRawData)
            pdataRaw = secs[i].PointerToRawData +
                       (exc.VirtualAddress - secs[i].VirtualAddress);
    auto pe = (IMAGE_RUNTIME_FUNCTION_ENTRY*)(g_img + pdataRaw);
    size_t nfunc = exc.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
    for (size_t i = 0; i < nfunc; i++)
        g_funcs.push_back({pe[i].BeginAddress, pe[i].EndAddress});
    for (int i = 0; i < nsec; i++)
        if (!memcmp(secs[i].Name, ".text", 5)) {
            g_text = g_img + secs[i].PointerToRawData;
            g_textSz = secs[i].SizeOfRawData;
            g_textRva = secs[i].VirtualAddress;
        }

    printf("=== tiny movsd getters (movsd xmm0,[rcx+disp]; ret) ===\n");
    {
        int n = 0;
        for (auto& f : g_funcs) {
            if (f.end - f.begin > 64 || f.end - f.begin < 3) continue;
            uint8_t* b = g_text + (f.begin - g_textRva);
            if (f.begin < g_textRva ||
                f.end > g_textRva + (DWORD)g_textSz)
                continue;
            bool hit = false;
            int32_t disp = 0;
            // walk max 4 insns
            size_t k = 0;
            int steps = 0;
            bool ok = false;
            while (k + 1 < (size_t)(f.end - f.begin) && steps < 4) {
                hde64s hs{};
                unsigned l = hde64_disasm(b + k, &hs);
                if (!l || (hs.flags & 0x1F000)) break;
                if (steps == 0 && hs.opcode == 0x0F && hs.opcode2 == 0x10 &&
                    (hs.flags & F_PREFIX_66) && hs.modrm_reg == 0 &&
                    hs.modrm_rm == 1 && hs.modrm_mod != 3) {
                    hit = true;
                    disp = (hs.flags & F_DISP32)
                               ? (int32_t)hs.disp.disp32
                               : ((hs.flags & F_DISP8) ? (int8_t)hs.disp.disp8
                                                       : 0);
                } else if (steps == 1 && hit &&
                           (hs.opcode == 0xC3 || hs.opcode == 0xC2)) {
                    ok = true;
                }
                steps++;
                k += l;
            }
            if (hit && ok) {
                printf("getter RVA_0x%x len=%u disp=0x%x\n", f.begin,
                       f.end - f.begin, disp);
                if (++n > 40) break;
            }
        }
        printf("getters: %d\n", n);
    }

    printf("=== cmp ecx,1 & cmp ecx,2 in same func ===\n");
    {
        int n = 0;
        for (auto& f : g_funcs) {
            bool c1 = false, c2 = false;
            WalkFunc(f, [&](uint8_t*, DWORD, hde64s& hs) {
                if (hs.opcode == 0x83 && hs.modrm_reg == 7 &&
                    hs.modrm_rm == 1 && (hs.flags & F_IMM8)) {
                    if (hs.imm.imm8 == 1) c1 = true;
                    if (hs.imm.imm8 == 2) c2 = true;
                }
            });
            if (c1 && c2) {
                printf("cmp12 func RVA_0x%x..0x%x\n", f.begin, f.end);
                if (++n > 15) break;
            }
        }
        printf("cmp12: %d\n", n);
    }

    printf("=== UCOMISD + JP + Jcc chains ===\n");
    {
        int n = 0;
        for (auto& f : g_funcs) {
            // collect insns
            struct Ins {
                DWORD rva;
                hde64s hs;
            };
            std::vector<Ins> ins;
            WalkFunc(f, [&](uint8_t*, DWORD rva, hde64s& hs) {
                ins.push_back({rva, hs});
            });
            for (size_t i = 0; i + 2 < ins.size(); i++) {
                auto& a = ins[i];
                auto& b = ins[i + 1];
                auto& c = ins[i + 2];
                if (a.hs.opcode == 0x0F && a.hs.opcode2 == 0x2E &&
                    b.hs.opcode == 0x7A &&
                    ((c.hs.opcode == 0x74 || c.hs.opcode == 0x75) ||
                     (c.hs.opcode == 0x0F &&
                      (c.hs.opcode2 == 0x84 || c.hs.opcode2 == 0x85)))) {
                    int32_t disp = (a.hs.flags & F_DISP32)
                                       ? (int32_t)a.hs.disp.disp32
                                       : 0;
                    printf("ucomisd RVA_0x%x disp=0x%x modrm=%02x(rex=%02x) func RVA_0x%x\n",
                           a.rva, disp, a.hs.modrm, a.hs.rex, f.begin);
                    if (++n > 20) break;
                }
            }
            if (n > 20) break;
        }
        printf("ucomisd chains: %d\n", n);
    }
    printf("=== .rdata doubles + referencing funcs ===\n");
    {
        struct Dbl {
            const char* name;
            uint8_t b[8];
        };
        Dbl dbls[] = {
            {"16.0", {0, 0, 0, 0, 0, 0, 0x30, 0x40}},
            {"24.0", {0, 0, 0, 0, 0, 0, 0x38, 0x40}},
            {"28.0", {0, 0, 0, 0, 0, 0, 0x3C, 0x40}},
            {"32.0", {0, 0, 0, 0, 0, 0, 0x40, 0x40}},
            {"44.0", {0, 0, 0, 0, 0, 0, 0x46, 0x40}},
            {"48.0", {0, 0, 0, 0, 0, 0, 0x48, 0x40}},
        };
        for (int i = 0; i < nsec; i++) {
            if (memcmp(secs[i].Name, ".rdata", 6)) continue;
            uint8_t* r = g_img + secs[i].PointerToRawData;
            for (DWORD j = 0; j + 8 < secs[i].SizeOfRawData; j++) {
                for (auto& d : dbls) {
                    if (memcmp(r + j, d.b, 8)) continue;
                    DWORD cRva = secs[i].VirtualAddress + j;
                    int nRef = 0;
                    for (auto& f : g_funcs) {
                        bool ref = false;
                        WalkFunc(f, [&](uint8_t* p, DWORD rva,
                                        hde64s& hs) {
                            if (ref) return;
                            // MOVSD xmm,[rip+d] or MOV r64,[rip+d] or LEA
                            DWORD tgt = 0;
                            bool rip = false;
                            if (hs.opcode == 0x0F && hs.opcode2 == 0x10 &&
                                hs.modrm_rm == 5 && hs.modrm_mod == 0 &&
                                (hs.flags & F_DISP32)) {
                                tgt = rva + hs.len + hs.disp.disp32;
                                rip = true;
                            } else if ((hs.opcode == 0x8B ||
                                        hs.opcode == 0x8D) &&
                                       hs.modrm_rm == 5 &&
                                       hs.modrm_mod == 0 &&
                                       (hs.flags & F_DISP32)) {
                                tgt = rva + hs.len + hs.disp.disp32;
                                rip = true;
                            }
                            if (rip && tgt == cRva) ref = true;
                        });
                        if (ref) {
                            if (nRef < 6)
                                printf("%s RVA_0x%x <- func RVA_0x%x\n",
                                       d.name, cRva, f.begin);
                            nRef++;
                        }
                    }
                    printf("%s RVA_0x%x refs=%d\n", d.name, cRva, nRef);
                }
            }
        }
    }
    printf("=== callers of pinned candidates ===\n");
    {
        DWORD targets[] = {0x2e7a0, 0x1a19dc, 0x85ca4, 0x2e604, 0x2eacc};
        for (DWORD t : targets) {
            int n = 0;
            for (auto& f : g_funcs) {
                bool calls = false;
                DWORD site = 0;
                WalkFunc(f, [&](uint8_t*, DWORD rva, hde64s& hs) {
                    if (calls) return;
                    if (hs.opcode == 0xE8 && (hs.flags & F_IMM32)) {
                        DWORD tgt = rva + hs.len + hs.imm.imm32;
                        if (tgt == t) {
                            calls = true;
                            site = rva;
                        }
                    }
                });
                if (calls) {
                    if (n < 8)
                        printf("caller RVA_0x%x -> 0x%x @site 0x%x\n", f.begin,
                               t, site);
                    n++;
                }
            }
            printf("0x%x callers=%d\n", t, n);
        }
    }
    printf("=== movabs doubles (48 B8 imm64) ===\n");
    {
        struct Dbl {
            const char* name;
            uint64_t v;
        };
        Dbl dbls[] = {{"16.0", 0x4030000000000000ULL},
                      {"24.0", 0x4038000000000000ULL},
                      {"32.0", 0x4040000000000000ULL},
                      {"48.0", 0x4048000000000000ULL}};
        for (auto& f : g_funcs) {
            WalkFunc(f, [&](uint8_t*, DWORD rva, hde64s& hs) {
                if (hs.opcode >= 0xB8 && hs.opcode <= 0xBF && hs.rex_w &&
                    (hs.flags & F_IMM64)) {
                    for (auto& d : dbls)
                        if (hs.imm.imm64 == d.v)
                            printf("movabs %s in func RVA_0x%x @0x%x\n",
                                   d.name, f.begin, rva);
                }
            });
        }
    }
    printf("=== callers of 0x1a057c (GetMetrics?) ===\n");
    {
        int n = 0;
        for (auto& f : g_funcs) {
            bool calls = false;
            DWORD site = 0;
            WalkFunc(f, [&](uint8_t*, DWORD rva, hde64s& hs) {
                if (calls) return;
                if (hs.opcode == 0xE8 && (hs.flags & F_IMM32)) {
                    if (rva + hs.len + hs.imm.imm32 == 0x1a057c) {
                        calls = true;
                        site = rva;
                    }
                }
            });
            if (calls) {
                if (n < 8)
                    printf("caller RVA_0x%x -> GetMetrics? @site 0x%x\n",
                           f.begin, site);
                n++;
            }
        }
        printf("GetMetrics? callers=%d\n", n);
    }
    printf("=== SHA256(Taskbar.dll) ===\n");
    {
        HCRYPTPROV hP = 0;
        HCRYPTHASH hH = 0;
        BYTE h[32];
        DWORD hl = 32;
        if (CryptAcquireContextW(&hP, nullptr, nullptr, PROV_RSA_AES,
                                 CRYPT_VERIFYCONTEXT) &&
            CryptCreateHash(hP, CALG_SHA_256, 0, 0, &hH)) {
            CryptHashData(hH, g_img, (DWORD)g_size, 0);
            CryptGetHashParam(hH, HP_HASHVAL, h, &hl, 0);
            printf("sha256: ");
            for (DWORD i = 0; i < hl; i++) printf("%02x", h[i]);
            printf("\n");
            CryptDestroyHash(hH);
            CryptReleaseContext(hP, 0);
        }
    }
    printf("=== range-check (e in {1,2}) funcs ===\n");
    {
        int n = 0;
        for (auto& f : g_funcs) {
            bool hit = false;
            DWORD site = 0;
            WalkFunc(f, [&](uint8_t* p, DWORD rva, hde64s& hs) {
                if (hit) return;
                // sub ecx,1 (83 E9 01) then cmp ecx,1 (83 F9 01) nearby
                if (hs.opcode == 0x83 && hs.modrm_reg == 5 &&
                    hs.modrm_rm == 1 && (hs.flags & F_IMM8) &&
                    hs.imm.imm8 == 1) {
                    // look ahead up to 24 bytes for cmp ecx,1 + ja
                    uint8_t* q = p + hs.len;
                    for (int k = 0; k < 24; k++) {
                        if (q[k] == 0x83 && q[k + 1] == 0xF9 &&
                            q[k + 2] == 0x01) {
                            hit = true;
                            site = rva;
                            return;
                        }
                    }
                }
            });
            if (hit) {
                if (n < 12)
                    printf("range12 func RVA_0x%x..0x%x @0x%x\n", f.begin,
                           f.end, site);
                n++;
            }
        }
        printf("range12 total(shown<=12): %d\n", n);
    }
    printf("=== 0x4e220 head + callees ===\n");
    {
        for (auto& f : g_funcs) {
            if (f.begin != 0x4e220) continue;
            uint8_t* b = g_text + (f.begin - g_textRva);
            size_t len = f.end - f.begin > 64 ? 64 : f.end - f.begin;
            printf("head:");
            for (size_t k = 0; k < len; k++) printf(" %02x", b[k]);
            printf("\n");
            WalkFunc(f, [&](uint8_t*, DWORD rva, hde64s& hs) {
                if (hs.opcode == 0xE8 && (hs.flags & F_IMM32))
                    printf("call @0x%x -> 0x%x\n", rva,
                           rva + hs.len + hs.imm.imm32);
            });
        }
    }
    return 0;
}
