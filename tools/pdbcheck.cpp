// pdbcheck: print the PDB GUID/age/name a PE file was linked against (CodeView RSDS).
// Read-only: reads the file from disk into a buffer, loads no module.
// Usage: pdbcheck.exe <image.dll|exe>
// Output: "guid=<32 hex upper> age=<n> pdb=<name>"
// Exit:   0 ok, 1 cannot read file, 2 not a PE, 3 no RSDS record.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdint>

static bool ReadFileAll(const wchar_t* path, std::vector<std::uint8_t>& out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return false; }
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    bool ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr)
              && got == out.size();
    CloseHandle(h);
    return ok;
}

// Translate an RVA into a file offset using the section table. Returns false if
// the RVA is not backed by file data.
static bool RvaToFileOffset(const IMAGE_NT_HEADERS64* nt, DWORD rva, size_t fileSize, size_t& outOff) {
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        DWORD span = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (rva < sec->VirtualAddress || rva - sec->VirtualAddress >= span) continue;
        DWORD delta = rva - sec->VirtualAddress;
        if (delta >= sec->SizeOfRawData) return false;
        size_t off = static_cast<size_t>(sec->PointerToRawData) + delta;
        if (off >= fileSize) return false;
        outOff = off;
        return true;
    }
    return false;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: pdbcheck <image.dll|exe>\n");
        return 1;
    }

    std::vector<std::uint8_t> img;
    if (!ReadFileAll(argv[1], img)) {
        fwprintf(stderr, L"pdbcheck: cannot read file: %s\n", argv[1]);
        return 1;
    }

    if (img.size() < sizeof(IMAGE_DOS_HEADER)) {
        fwprintf(stderr, L"pdbcheck: not a PE: %s\n", argv[1]);
        return 2;
    }
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > img.size()) {
        fwprintf(stderr, L"pdbcheck: not a PE: %s\n", argv[1]);
        return 2;
    }
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(img.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        fwprintf(stderr, L"pdbcheck: not an x64 PE: %s\n", argv[1]);
        return 2;
    }
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG) {
        fwprintf(stderr, L"pdbcheck: no debug directory: %s\n", argv[1]);
        return 3;
    }

    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (dir.VirtualAddress == 0 || dir.Size < sizeof(IMAGE_DEBUG_DIRECTORY)) {
        fwprintf(stderr, L"pdbcheck: no RSDS record: %s\n", argv[1]);
        return 3;
    }

    size_t dirOff = 0;
    if (!RvaToFileOffset(nt, dir.VirtualAddress, img.size(), dirOff) ||
        dirOff + dir.Size > img.size()) {
        fwprintf(stderr, L"pdbcheck: bad debug directory RVA: %s\n", argv[1]);
        return 2;
    }

    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(img.data() + dirOff);
    size_t count = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (size_t i = 0; i < count; ++i) {
        const IMAGE_DEBUG_DIRECTORY& e = entries[i];
        if (e.Type != IMAGE_DEBUG_TYPE_CODEVIEW || e.SizeOfData < 24 || e.AddressOfRawData == 0)
            continue;
        size_t off = 0;
        if (!RvaToFileOffset(nt, e.AddressOfRawData, img.size(), off)) continue;
        if (off + e.SizeOfData > img.size()) continue;

        const std::uint8_t* p = img.data() + off;
        if (std::memcmp(p, "RSDS", 4) != 0) continue;

        const std::uint8_t* g = p + 4;
        std::uint32_t d1;
        std::uint16_t d2, d3;
        std::memcpy(&d1, g, 4);
        std::memcpy(&d2, g + 4, 2);
        std::memcpy(&d3, g + 6, 2);
        std::uint32_t age;
        std::memcpy(&age, p + 20, 4);

        const char* name = reinterpret_cast<const char*>(p + 24);
        size_t maxName = e.SizeOfData - 24;
        size_t nameLen = strnlen(name, maxName);

        printf("guid=%08lX%04X%04X", static_cast<unsigned long>(d1), d2, d3);
        for (int b = 0; b < 8; ++b) printf("%02X", g[8 + b]);
        printf(" age=%lu pdb=%.*s\n", static_cast<unsigned long>(age),
               static_cast<int>(nameLen), name);
        return 0;
    }

    fwprintf(stderr, L"pdbcheck: no RSDS record: %s\n", argv[1]);
    return 3;
}
