// test_resolve: resolver unit test on the REAL Taskbar.dll FILE bytes.
// No LoadLibrary (its DllMain needs explorer), no explorer contact.
// PASS = unique resolution to the validated RVAs (file 41326fff…).
#include "../src/dll/resolve.h"
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <wincrypt.h>
#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")

static const char kValidatedSha256[] =
    "41326fffd57a2c6e08d1884a698bd9e9dfe90de0614de98733edaebacb5f5403";

// The FixedFileInfo (VS_FIXEDFILEINFO) of System32\Taskbar.dll reports a stale
// 6.2 product major/minor even though the file is 10.0.26100.8457 (its string
// FileVersion). Use the StringFileInfo FileVersion as the source of truth.
static bool IsKnownBuild(const wchar_t* path) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (size == 0) return false;
    std::vector<uint8_t> ver(size);
    if (!GetFileVersionInfoW(path, 0, size, ver.data())) return false;
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (VerQueryValueW(ver.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) && ffi) {
        printf("fixed file info %u.%u.%u.%u\n",
               HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
               HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    }
    wchar_t* str = nullptr;
    UINT strLen = 0;
    if (!VerQueryValueW(ver.data(), L"\\StringFileInfo\\040904B0\\FileVersion",
                        reinterpret_cast<void**>(&str), &strLen) || !str) {
        printf("FAIL no StringFileInfo FileVersion\n");
        return false;
    }
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (swscanf_s(str, L"%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        printf("FAIL cannot parse FileVersion\n");
        return false;
    }
    printf("file version %u.%u.%u.%u\n", a, b, c, d);
    return a == 10 && b == 0 && c == 26100;
}

static bool FileSha256Hex(const wchar_t* path, char out[65]) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool ok = false;
    if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES,
                             CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
        std::vector<uint8_t> chunk(1 << 16);
        DWORD rd = 0;
        ok = true;
        while (ReadFile(h, chunk.data(), (DWORD)chunk.size(), &rd, nullptr) && rd > 0) {
            if (!CryptHashData(hash, chunk.data(), rd, 0)) { ok = false; break; }
        }
        BYTE digest[32];
        DWORD dlen = sizeof(digest);
        if (ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &dlen, 0) && dlen == 32) {
            for (int i = 0; i < 32; ++i) snprintf(out + i * 2, 3, "%02x", digest[i]);
            out[64] = '\0';
        } else {
            ok = false;
        }
    }
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    CloseHandle(h);
    return ok;
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
    char sha[65] = {};
    if (!FileSha256Hex(path, sha)) {
        printf("FAIL sha256 computation\n");
        return 1;
    }
    printf("sha256 %s\n", sha);
    bool knownHash = strcmp(sha, kValidatedSha256) == 0;
    if (!knownHash) printf("FAIL sha256 does not match validated 41326fff...\n");
    if (!IsKnownBuild(path)) {
        printf("SKIP: build is not 10.0.26100.*, RVA check not applicable\n");
        return knownHash ? 0 : 1;
    }
    int fail = knownHash ? 0 : 1;
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
