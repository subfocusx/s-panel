// pdbrva: offline RVA lookup from a PDB. Read-only: loads no module, injects nothing.
// Usage:
//   pdbrva.exe <file.pdb> [--pe <image.dll>] [--contains] <name> [name...]
//   --pe        PE image on disk whose section table maps PUB32 seg:off -> RVA
//               (default: %SystemRoot%\System32\Taskbar.dll)
//   --contains  treat each name as a substring and print every match
// Prints "<RVA hex>  <name>" per hit, or "NOT FOUND  <name>".
#include <windows.h>
#include <MSPDBX/PDB.h>
#include <MSPDBX/PEUtility.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

static bool ReadFileAll(const std::wstring& path, std::vector<std::uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
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

// Build segment-index -> section RVA map from a PE file on disk (no loading).
static bool LoadSectionRVAs(const std::vector<std::uint8_t>& image,
                            std::unordered_map<size_t, DWORD>& outRva) {
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew < 0 || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS) > image.size())
        return false;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image.data() + dos->e_lfanew);
    auto segs = MSPDBX::PEGetAllSegments(nt);
    if (!segs) return false;
    outRva = std::move(*segs);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fwprintf(stderr,
                 L"usage: pdbrva <file.pdb> [--pe <image.dll>] [--contains] <name> [name...]\n");
        return 2;
    }

    std::wstring pdbPath = argv[1];
    std::wstring pePath;
    bool contains = false;
    std::vector<std::string> names;

    for (int i = 2; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--pe" && i + 1 < argc) {
            pePath = argv[++i];
        } else if (a == L"--contains") {
            contains = true;
        } else {
            int n = WideCharToMultiByte(CP_UTF8, 0, a.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 0 ? n - 1 : 0, '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, a.c_str(), -1, s.data(), n, nullptr, nullptr);
            names.push_back(s);
        }
    }
    if (names.empty()) {
        fwprintf(stderr, L"pdbrva: no symbol names given\n");
        return 2;
    }

    if (pePath.empty()) {
        wchar_t sysdir[MAX_PATH]{};
        GetSystemDirectoryW(sysdir, MAX_PATH);
        pePath = std::wstring(sysdir) + L"\\Taskbar.dll";
    }

    std::vector<std::uint8_t> pdbBytes;
    if (!ReadFileAll(pdbPath, pdbBytes)) {
        fwprintf(stderr, L"pdbrva: cannot read PDB: %s\n", pdbPath.c_str());
        return 3;
    }
    std::vector<std::uint8_t> peBytes;
    if (!ReadFileAll(pePath, peBytes)) {
        fwprintf(stderr, L"pdbrva: cannot read PE: %s\n", pePath.c_str());
        return 3;
    }
    std::unordered_map<size_t, DWORD> sectionRva;
    if (!LoadSectionRVAs(peBytes, sectionRva)) {
        fwprintf(stderr, L"pdbrva: not a valid PE: %s\n", pePath.c_str());
        return 3;
    }

    // All PUB32 symbols -> RVA. Key = decorated name, value = RVA.
    std::vector<std::pair<std::string, DWORD>> all;
    try {
        MSPDBX::PDB pdb(pdbBytes.data());
        pdb.ForEachSymbol([&](const MSPDBX::PDB::PUBSYM32& sym) {
            auto it = sectionRva.find(sym.seg - 1);
            if (it == sectionRva.end()) return true;
            all.emplace_back(std::string(sym.name), it->second + sym.off);
            return true;
        });
    } catch (const std::exception&) {
        fwprintf(stderr, L"pdbrva: not a valid MSF PDB: %s\n", pdbPath.c_str());
        return 3;
    }

    for (const auto& want : names) {
        bool found = false;
        for (const auto& [name, rva] : all) {
            bool hit = contains ? (name.find(want) != std::string::npos) : (name == want);
            if (!hit) continue;
            found = true;
            printf("%08lX  %s\n", static_cast<unsigned long>(rva), name.c_str());
        }
        if (!found) printf("NOT FOUND  %s\n", want.c_str());
    }
    return 0;
}
