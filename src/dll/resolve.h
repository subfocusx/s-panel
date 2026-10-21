#pragma once
#include <windows.h>
#include <cstdint>

// Offline resolver: derives hook addresses from the LOADED Taskbar.dll image
// (or a file buffer, for unit tests) using uniqueness rules. Any rule that
// does not yield EXACTLY ONE match -> refuse (return false). Never guess.
struct ResolvedAddrs {
    DWORD getFrameSizeRva = 0;  // sole referrer of .rdata 48.0
    DWORD getIconHeightRva = 0; // sole referrer of BOTH 16.0 and 24.0
    DWORD getMetricsRva = 0;    // sole caller of getIconHeight
    uint64_t imageHash = 0;     // FNV-1a over .text (fingerprint, logged)
};

// Works on a loaded module (base = HMODULE) or a raw file image.
bool ResolveTaskbarAddrs(const uint8_t* image, size_t size, bool isLoaded,
                         ResolvedAddrs* out);
