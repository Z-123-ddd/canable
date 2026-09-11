// nsi_dfu_flash.c — 32-bit helper: DFU-flash a .bin via vendor NsMcuCmdDll.dll
//
// The vendor DLL (NsMcuCmdDll.dll, 32-bit) talks to the factory DFU bootloader
// through the NationstechDFU.sys driver; it computes all the protocol framing /
// CRCs internally, so the host never needs to. This helper is a thin CLI wrapper
// the (64-bit) Qt host app launches via QProcess after the device is in DFU.
//
//   NS_CMD_ENUM_DEV(type=1 USB-DFU, wchar_t* nameBuf, int bufSize)
//   NS_CMD_CONNECT_DEV(unsigned* outHandle, const wchar_t* name, int flags)  -> 0, *outHandle = h
//   NS_CMD_GET_INF(handle, out)                                              -> 0xA000
//   NS_CMD_FLASH_ERASE(handle, startAddr, endAddr, 0)                        -> 0xA000
//   NS_CMD_FLASH_DWNLD(handle, NS_Dl_File*, NULL, NULL, NULL, NULL)          -> 0xA000
//       NS_Dl_File: +0x04 addr, +0x08 DATA PTR, +0x0C len(16-aln), +0x10 byte flag, +0x14 extra ptr
//   NS_CMD_APP_GO(handle, 0, 0, 0)                                           -> 0xA000  (run new fw)
//
// Build (x86):  cl /nologo /O2 /Fe:nsi_dfu_flash.exe nsi_dfu_flash.c ole32.lib
// Usage:        nsi_dfu_flash.exe <dllDir> <bin> [startAddrHex]
// Exit:         0 ok ; non-zero = failure.

#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*fp_enum)(unsigned char type, wchar_t *nameBuf, int bufSize);
typedef int (*fp_conn)(void *outHandle, const wchar_t *name, int flags);
typedef int (*fp_getinf)(void *hdl, void *out);
typedef int (*fp_erase)(void *hdl, unsigned start, unsigned end, void *ctx);
typedef int (*fp_dwnld6)(void *hdl, void *dlfile, void *a2, void *a3, void *a4, void *a5);
typedef int (*fp_appgo)(void *hdl, void *a1, void *a2, void *a3);

// 宽字符入口：路径可为含中文的任意 Unicode（wmain 自动被 MSVC 链接器选用）
int wmain(int argc, wchar_t **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { printf("usage: nsi_dfu_flash.exe <dllDir> <bin> [startAddrHex]\n"); return 2; }
    const wchar_t *dllDir = argv[1], *binPath = argv[2];
    unsigned startAddr = (argc >= 4) ? (unsigned)wcstoul(argv[3], 0, 16) : 0x08000000;

    CoInitialize(NULL);
    SetCurrentDirectoryW(dllDir);
    SetDllDirectoryW(dllDir);
    wchar_t dllpath[MAX_PATH];
    _snwprintf(dllpath, MAX_PATH, L"%ls\\NsMcuCmdDll.dll", dllDir);
    HMODULE h = LoadLibraryExW(dllpath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { printf("ERROR: load DLL failed (%lu)\n", GetLastError()); return 3; }

    fp_enum ENUM = (fp_enum) GetProcAddress(h, "NS_CMD_ENUM_DEV");
    fp_conn CONN = (fp_conn) GetProcAddress(h, "NS_CMD_CONNECT_DEV");
    fp_getinf GETIN = (fp_getinf) GetProcAddress(h, "NS_CMD_GET_INF");
    fp_erase ERASE = (fp_erase) GetProcAddress(h, "NS_CMD_FLASH_ERASE");
    fp_dwnld6 DWN = (fp_dwnld6) GetProcAddress(h, "NS_CMD_FLASH_DWNLD");
    fp_appgo GO = (fp_appgo) GetProcAddress(h, "NS_CMD_APP_GO");
    if (!ENUM || !CONN || !GETIN || !ERASE || !DWN || !GO) { printf("ERROR: missing exports\n"); return 4; }

    wchar_t wname[0x400]; memset(wname, 0, sizeof(wname));
    if (ENUM(1, wname, 0x400) != 1 || !wname[0]) { printf("ERROR: no USB-DFU device found\n"); return 5; }
    { size_t n = wcslen(wname); while (n && wname[n-1] == L';') wname[--n] = 0; }
    wprintf(L"device: %ls\n", wname);

    unsigned hdl = 0;
    CONN(&hdl, wname, 0);
    if (!hdl) { printf("ERROR: connect failed\n"); return 6; }
    unsigned char inf[0x200]; memset(inf, 0, sizeof(inf));
    if ((GETIN((void *)(size_t)hdl, inf) & 0xffffffff) != 0xA000) { printf("ERROR: read info failed\n"); return 7; }
    printf("chip: model=%d\n", inf[4]);

    FILE *f = _wfopen(binPath, L"rb");
    if (!f) { printf("ERROR: open bin failed\n"); return 8; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *fw = (unsigned char *)VirtualAlloc(NULL, sz + 16, MEM_COMMIT, PAGE_READWRITE);
    if (!fw) { fclose(f); return 9; }
    fread(fw, 1, sz, f); fclose(f);
    unsigned len = (unsigned)((sz + 15) & ~15);
    for (long i = sz; i < (long)len; i++) fw[i] = 0;
    printf("bin: %ld bytes -> erase/download %u bytes @ 0x%08X\n", sz, len, startAddr);

    if ((ERASE((void *)(size_t)hdl, startAddr, startAddr + len - 1, 0) & 0xffffffff) != 0xA000) {
        printf("ERROR: erase failed\n"); return 10;
    }
    printf("erase: OK\n");

    unsigned char *dlf = (unsigned char *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    memset(dlf, 0, 0x1000);
    *(unsigned *)(dlf + 0x04) = startAddr;
    *(unsigned *)(dlf + 0x08) = (unsigned)(size_t)fw;   // DATA POINTER
    *(unsigned *)(dlf + 0x0C) = len;
    *(unsigned *)(dlf + 0x10) = 0;
    *(unsigned *)(dlf + 0x14) = (unsigned)(size_t)fw;

    int dr = -1;
    __try { dr = DWN((void *)(size_t)hdl, dlf, NULL, NULL, NULL, NULL); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("ERROR: download raised 0x%08X\n", GetExceptionCode()); return 11; }
    if ((dr & 0xffffffff) != 0xA000) { printf("ERROR: download failed (0x%X)\n", dr & 0xffffffff); return 12; }
    printf("download: OK\n");

    if ((GO((void *)(size_t)hdl, 0, 0, 0) & 0xffffffff) != 0xA000) { printf("ERROR: app_go failed\n"); return 13; }
    printf("app_go: OK\n");
    printf("RESULT: OK\n");
    return 0;
}
