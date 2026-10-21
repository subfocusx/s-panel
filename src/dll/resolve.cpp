// resolve.cpp — offline, uniqueness-gated discovery (see resolve.h).
// Rules (validated on Taskbar.dll 10.0.26100, sha256 41326fff...):
//  R1: .rdata contains exactly one 48.0 double; exactly one .text function
//      references it via RIP-relative MOV/LEA/MOVSD -> GetFrameSize.
//  R2: exactly one function references BOTH 16.0 and 24.0 -> GetIconHeight.
//  R3: exactly one function calls GetIconHeight -> GetMetrics.
#include "resolve.h"
#include <cstring>

static uint64_t Fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

bool ResolveTaskbarAddrs(const uint8_t* image, size_t size, bool isLoaded,
                         ResolvedAddrs* out) {
    if (!image || size < 0x1000 || !out) return false;
    *out = ResolvedAddrs{};
    auto dos = (const IMAGE_DOS_HEADER*)image;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if ((size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > size) return false;
    auto nt = (const IMAGE_NT_HEADERS64*)(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;

    auto secs = (const IMAGE_SECTION_HEADER*)((const uint8_t*)&nt->OptionalHeader +
                                              nt->FileHeader.SizeOfOptionalHeader);
    int nsec = nt->FileHeader.NumberOfSections;
    const uint8_t* text = nullptr;
    size_t textSz = 0;
    DWORD textRva = 0;
    const uint8_t* rdata = nullptr;
    size_t rdataSz = 0;
    DWORD rdataRva = 0;
    for (int i = 0; i < nsec; i++) {
        char n[9] = {};
        memcpy(n, secs[i].Name, 8);
        if (!strcmp(n, ".text")) {
            textRva = secs[i].VirtualAddress;
            textSz = secs[i].Misc.VirtualSize;
            text = isLoaded ? image + textRva
                            : image + secs[i].PointerToRawData;
        } else if (!strcmp(n, ".rdata")) {
            rdataRva = secs[i].VirtualAddress;
            rdataSz = secs[i].Misc.VirtualSize;
            rdata = isLoaded ? image + rdataRva
                             : image + secs[i].PointerToRawData;
        }
    }
    if (!text || !rdata) return false;

    // Find .rdata doubles (exact byte match).
    auto findDbl = [&](uint64_t bits) -> DWORD {
        DWORD found = 0;
        int cnt = 0;
        for (size_t j = 0; j + 8 <= rdataSz; j += 1) {
            uint64_t v;
            memcpy(&v, rdata + j, 8);
            if (v == bits) {
                found = rdataRva + (DWORD)j;
                if (++cnt > 1) return 0; // not unique
            }
        }
        return cnt == 1 ? found : 0;
    };
    const DWORD r48 = findDbl(0x4048000000000000ULL);
    const DWORD r16 = findDbl(0x4030000000000000ULL);
    const DWORD r24 = findDbl(0x4038000000000000ULL);
    if (!r48 || !r16 || !r24) return false;

    // Walk .text linearly; HDE-free minimal decoder for the patterns we need:
    //  E8 rel32 (call), 83 /7 ib with modrm rm==1 (cmp ecx,imm8),
    //  8B/8D modrm rm==5 disp32 (MOV/LEA rip-rel), F2 0F 10 modrm rm==5
    //  (MOVSD rip-rel). Everything else: skip via tiny length table is
    //  overkill — instead scan byte windows and VERIFY with a second pass.
    //  Simpler robust approach: candidate functions from .pdata, byte-scan
    //  inside each range (same as validated tools).
    auto& exc =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    // In a loaded image RVAs are direct; in a file find the raw offset.
    DWORD pdataOff = 0;
    if (isLoaded) {
        pdataOff = exc.VirtualAddress;
    } else {
        for (int i = 0; i < nsec; i++) {
            DWORD va = secs[i].VirtualAddress, vsz = secs[i].Misc.VirtualSize;
            if (exc.VirtualAddress >= va && exc.VirtualAddress < va + vsz) {
                pdataOff = secs[i].PointerToRawData +
                           (exc.VirtualAddress - va);
                break;
            }
        }
    }
    if (!pdataOff) return false;
    auto pe = (const IMAGE_RUNTIME_FUNCTION_ENTRY*)(image + pdataOff);
    size_t nfunc = exc.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);

    auto fnBytes = [&](DWORD begin, DWORD end, const uint8_t** b,
                       size_t* n) -> bool {
        if (begin < textRva || end > textRva + (DWORD)textSz || end <= begin ||
            end - begin > 0x4000)
            return false;
        *b = text + (begin - textRva);
        *n = end - begin;
        return true;
    };
    auto refsRva = [&](const uint8_t* b, size_t n, DWORD funcRva,
                       DWORD target) -> bool {
        for (size_t k = 0; k + 8 <= n; k++) {
            // MOV r64,[rip+d]: 48 8B /0 (modrm&0xC7==05)
            if (b[k] == 0x48 && b[k + 1] == 0x8B &&
                (b[k + 2] & 0xC7) == 0x05) {
                int32_t d;
                memcpy(&d, b + k + 3, 4);
                if (funcRva + (DWORD)k + 7 + d == target) return true;
            }
            // LEA r64,[rip+d]: 48 8D /0
            if (b[k] == 0x48 && b[k + 1] == 0x8D &&
                (b[k + 2] & 0xC7) == 0x05) {
                int32_t d;
                memcpy(&d, b + k + 3, 4);
                if (funcRva + (DWORD)k + 7 + d == target) return true;
            }
            // MOVSD xmm,[rip+d]: F2 0F 10 /0 (modrm&0xC7==05)
            if (b[k] == 0xF2 && b[k + 1] == 0x0F && b[k + 2] == 0x10 &&
                (b[k + 3] & 0xC7) == 0x05) {
                int32_t d;
                memcpy(&d, b + k + 4, 4);
                if (funcRva + (DWORD)k + 8 + d == target) return true;
            }
        }
        return false;
    };

    const uint8_t* bb;
    size_t nn;
    DWORD frame = 0;
    int nFrame = 0;
    DWORD icon = 0;
    int nIcon = 0;
    for (size_t i = 0; i < nfunc; i++) {
        DWORD b = pe[i].BeginAddress, e = pe[i].EndAddress;
        if (!fnBytes(b, e, &bb, &nn)) continue;
        if (refsRva(bb, nn, b, r48)) {
            frame = b;
            nFrame++;
        }
        if (refsRva(bb, nn, b, r16) && refsRva(bb, nn, b, r24)) {
            icon = b;
            nIcon++;
        }
    }
    if (nFrame != 1 || nIcon != 1) return false;

    // R3: sole caller of icon (E8 rel32 scan).
    auto callsTarget = [&](DWORD callerB, DWORD callerE,
                           DWORD target) -> bool {
        const uint8_t* cb;
        size_t cn;
        if (!fnBytes(callerB, callerE, &cb, &cn)) return false;
        for (size_t k = 0; k + 5 <= cn; k++) {
            if (cb[k] == 0xE8) {
                int32_t d;
                memcpy(&d, cb + k + 1, 4);
                if (callerB + (DWORD)k + 5 + d == target) return true;
            }
        }
        return false;
    };
    DWORD metrics = 0;
    int nMet = 0;
    for (size_t i = 0; i < nfunc; i++) {
        DWORD b = pe[i].BeginAddress, e = pe[i].EndAddress;
        if (callsTarget(b, e, icon)) {
            metrics = b;
            nMet++;
        }
    }
    if (nMet != 1) return false;

    out->getFrameSizeRva = frame;
    out->getIconHeightRva = icon;
    out->getMetricsRva = metrics;
    out->imageHash = Fnv1a(text, textSz);
    return true;
}
