// mdstack: minimal minidump reader. Prints exception record + faulting thread
// registers + top of stack (return addresses) + module list. Read-only.
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdint>
#include <vector>

static const uint8_t* g_base = nullptr;

template <typename T>
static const T* At(RVA rva) {
    return (const T*)(g_base + rva);
}

static const MINIDUMP_MODULE* FindModule(uint64_t) {
    return nullptr;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: mdstack.exe <dump>\n");
        return 2;
    }
    HANDLE f = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 3;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    std::vector<uint8_t> img((size_t)sz.QuadPart);
    DWORD rd = 0;
    ReadFile(f, img.data(), (DWORD)img.size(), &rd, nullptr);
    CloseHandle(f);
    g_base = img.data();

    auto hdr = At<MINIDUMP_HEADER>(0);
    if (hdr->Signature != MINIDUMP_SIGNATURE) {
        printf("bad signature\n");
        return 4;
    }
    auto dir = At<MINIDUMP_DIRECTORY>(hdr->StreamDirectoryRva);
    const MINIDUMP_DIRECTORY* excDir = nullptr;
    const MINIDUMP_DIRECTORY* modDir = nullptr;
    const MINIDUMP_DIRECTORY* thdDir = nullptr;
    for (ULONG i = 0; i < hdr->NumberOfStreams; i++) {
        if (dir[i].StreamType == ExceptionStream) excDir = &dir[i];
        if (dir[i].StreamType == ModuleListStream) modDir = &dir[i];
        if (dir[i].StreamType == ThreadListStream) thdDir = &dir[i];
    }
    if (!excDir || !modDir || !thdDir) {
        printf("missing streams\n");
        return 5;
    }
    auto exs = At<MINIDUMP_EXCEPTION_STREAM>(excDir->Location.Rva);
    auto* exc = &exs->ExceptionRecord;
    printf("ExceptionCode=0x%08lX Address=%p ThreadId=%lu\n",
           (unsigned long)exc->ExceptionCode, (void*)(uintptr_t)exc->ExceptionAddress,
           (unsigned long)exs->ThreadId);
    printf("params=%lu:", (unsigned long)exc->NumberParameters);
    for (ULONG i = 0; i < exc->NumberParameters && i < 15; i++)
        printf(" %llx", (unsigned long long)exc->ExceptionInformation[i]);
    printf("\n");
    ULONG excThread = exs->ThreadId;

    // Modules.
    struct Mod {
        uint64_t base, size;
        char name[260];
    };
    std::vector<Mod> mods;
    auto* mlbase = At<uint8_t>(modDir->Location.Rva);
    ULONG nMods = *(const ULONG*)mlbase;
    for (ULONG i = 0; i < nMods; i++) {
        auto* m = (const MINIDUMP_MODULE*)(mlbase + sizeof(ULONG) +
                                           i * sizeof(MINIDUMP_MODULE));
        Mod md{};
        md.base = m->BaseOfImage;
        md.size = m->SizeOfImage;
        const uint8_t* sp = g_base + m->ModuleNameRva;
        ULONG slen = *(const ULONG*)sp;
        const wchar_t* wname = (const wchar_t*)(sp + sizeof(ULONG));
        size_t wchars = slen / sizeof(wchar_t);
        size_t wi = 0;
        for (; wi + 1 < sizeof(md.name) && wi < wchars; wi++)
            md.name[wi] = (char)(wname[wi] & 0xFF);
        md.name[wi] = 0;
        const char* bn = strrchr(md.name, '\\');
        if (bn) memmove(md.name, bn + 1, strlen(bn + 1) + 1);
        mods.push_back(md);
    }
    auto sym = [&](uint64_t a, char* out, size_t n) {
        for (auto& m : mods) {
            if (a >= m.base && a < m.base + m.size) {
                snprintf(out, n, "%s+0x%llx", m.name,
                         (unsigned long long)(a - m.base));
                return;
            }
        }
        snprintf(out, n, "0x%llx (?)", (unsigned long long)a);
    };

    // Faulting thread: context + stack (manual walk: no array member in SDK struct).
    auto* tlbase = At<uint8_t>(thdDir->Location.Rva);
    ULONG nThreads = *(const ULONG*)tlbase;
    for (ULONG i = 0; i < nThreads; i++) {
        auto* t = (const MINIDUMP_THREAD*)(tlbase + sizeof(ULONG) +
                                          i * sizeof(MINIDUMP_THREAD));
        if (t->ThreadId != excThread) continue;
        auto* ctx = At<CONTEXT>(t->ThreadContext.Rva);
        printf("Thread %lu: Rip=%p Rsp=%p Rbp=%p\n", t->ThreadId, (void*)ctx->Rip,
               (void*)ctx->Rsp, (void*)ctx->Rbp);
        // Stack memory: find in MemoryList? Thread stack is in Memory64List or
        // MemoryList. Search all memory descriptors for Rsp.
        // Simpler: scan ThreadList memory? MINIDUMP_THREAD has Stack field!
        uint64_t rsp = ctx->Rsp;
        auto* st = &t->Stack;
        uint64_t saddr = st->StartOfMemoryRange;
        const uint8_t* sdata = g_base + st->Memory.Rva;
        size_t ssize = (size_t)st->Memory.DataSize;
        printf("stack range %p size 0x%zx, rsp offset 0x%llx\n", (void*)saddr, ssize,
               (unsigned long long)(rsp - saddr));
        for (int k = 0; k < 24; k++) {
            uint64_t off = (rsp - saddr) + (uint64_t)k * 8;
            if (off + 8 > ssize) break;
            uint64_t v = 0;
            memcpy(&v, sdata + off, 8);
            char name[300]{};
            sym(v, name, sizeof(name));
            printf("  rsp+%02x: %s\n", k * 8, name);
        }
    }
    return 0;
}
