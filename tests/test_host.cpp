// Dry test 2: load taskbar.dll in a dummy host (NOT explorer).
// PASS = loads, DllMain notices Taskbar.dll absent / build unknown, unloads
// cleanly, exit 0. Proves the DLL cannot crash a host by itself.
#include <windows.h>
#include <cstdio>

struct FreeCtx {
    HMODULE module;
    BOOL result;
    DWORD gle;
};

static DWORD WINAPI FreeThread(LPVOID p) {
    FreeCtx* ctx = static_cast<FreeCtx*>(p);
    ctx->result = FreeLibrary(ctx->module);
    ctx->gle = ctx->result ? 0 : GetLastError();
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: test_host.exe <taskbar.dll>\n");
        return 3;
    }
    HMODULE m = LoadLibraryW(argv[1]);
    if (!m) {
        printf("FAIL LoadLibrary GLE=%lu\n", GetLastError());
        return 1;
    }
    printf("loaded, sleeping 1s for InitThread...\n");
    Sleep(1000);

    FreeCtx ctx{m, FALSE, 0};
    HANDLE th = CreateThread(nullptr, 0, FreeThread, &ctx, 0, nullptr);
    if (!th) {
        printf("FAIL CreateThread GLE=%lu\n", GetLastError());
        return 1;
    }
    if (WaitForSingleObject(th, 5000) != WAIT_OBJECT_0) {
        printf("FAIL FreeLibrary timed out (>5s)\n");
        return 1;
    }
    CloseHandle(th);
    if (!ctx.result) {
        printf("FAIL FreeLibrary GLE=%lu\n", ctx.gle);
        return 1;
    }
    printf("host test PASS (no crash, clean unload within 5s)\n");
    return 0;
}
