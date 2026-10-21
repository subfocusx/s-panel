// featcheck: query Velocity feature 29785184 (DynamicIconScaling) state.
// Read-only. Usage: featcheck.exe [featureId]
#include <windows.h>
#include <cstdio>
#include <cstdlib>

#pragma pack(push, 1)
struct RTL_FEATURE_CONFIGURATION {
    unsigned int featureId;
    unsigned __int32 group : 4;
    unsigned __int32 enabledState : 2;
    unsigned __int32 enabledStateOptions : 1;
    unsigned __int32 unused1 : 1;
    unsigned __int32 variant : 6;
    unsigned __int32 variantPayloadKind : 2;
    unsigned __int32 unused2 : 16;
    unsigned int payload;
};
#pragma pack(pop)

int wmain(int argc, wchar_t** argv) {
    UINT32 id = argc > 1 ? _wtoi(argv[1]) : 29785184;
    auto fn = (int(NTAPI*)(UINT32, int, INT64*, RTL_FEATURE_CONFIGURATION*))
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                       "RtlQueryFeatureConfiguration");
    if (!fn) {
        printf("no RtlQueryFeatureConfiguration\n");
        return 3;
    }
    RTL_FEATURE_CONFIGURATION f = {};
    INT64 stamp = 0;
    HRESULT hr = fn(id, 1, &stamp, &f);
    // enabledState: 0=default 1=disabled 2=enabled
    printf("feature %u hr=0x%08x state=%u variant=%u\n", id, (unsigned)hr,
           (unsigned)(f.enabledState /*bitfield read*/), (unsigned)f.variant);
    return 0;
}
