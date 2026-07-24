// NVCloak - process-local NVIDIA adapter cloaking proxy
// Shared core for DXGI and forwarding-proxy injection variants.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <tlhelp32.h>
#include <guiddef.h>
#include <string.h>
#include <wchar.h>

// Real dxgi.dll handle
static HMODULE g_realDxgi = NULL;

// Type defs for real CreateDXGIFactory functions
typedef HRESULT (WINAPI *CreateDXGIFactory_t)(REFIID riid, void** ppFactory);
typedef HRESULT (WINAPI *CreateDXGIFactory1_t)(REFIID riid, void** ppFactory);
typedef HRESULT (WINAPI *CreateDXGIFactory2_t)(UINT Flags, REFIID riid, void** ppFactory);

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory);
HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory);
HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory);

// Forward declarations
typedef struct _FilterFactory FilterFactory;
struct _FilterFactory {
    const struct IDXGIFactory7Vtbl* lpVtbl;
    IDXGIFactory7* inner;
    LONG refcount;
};

// Get a function from the real dxgi.dll (not ourselves)
static FARPROC getRealProc(const char* name) {
    if (!g_realDxgi) {
        g_realDxgi = LoadLibraryW(L"C:\\Windows\\System32\\dxgi.dll");
        if (!g_realDxgi)
            g_realDxgi = LoadLibraryW(L"dxgi.dll");
    }
    return GetProcAddress(g_realDxgi, name);
}

// Check if an adapter is NVIDIA (PCI vendor ID 0x10DE).
static BOOL isNvidiaAdapter(IDXGIAdapter1* pAdapter) {
    DXGI_ADAPTER_DESC1 desc;
    return pAdapter && SUCCEEDED(pAdapter->lpVtbl->GetDesc1(pAdapter, &desc))
        && desc.VendorId == 0x10DE;
}

static BOOL isFactoryIid(REFIID riid) {
    return IsEqualGUID(riid, &IID_IUnknown)
        || IsEqualGUID(riid, &IID_IDXGIObject)
        || IsEqualGUID(riid, &IID_IDXGIFactory)
        || IsEqualGUID(riid, &IID_IDXGIFactory1)
        || IsEqualGUID(riid, &IID_IDXGIFactory2)
        || IsEqualGUID(riid, &IID_IDXGIFactory3)
        || IsEqualGUID(riid, &IID_IDXGIFactory4)
        || IsEqualGUID(riid, &IID_IDXGIFactory5)
        || IsEqualGUID(riid, &IID_IDXGIFactory6)
        || IsEqualGUID(riid, &IID_IDXGIFactory7);
}

// Translate a visible adapter index into the matching real DXGI index.
// This prevents the AMD adapter from being returned twice after NVIDIA is skipped.
static HRESULT getVisibleAdapter(FilterFactory* self, UINT visibleIndex,
    IDXGIAdapter1** ppAdapter) {
    UINT actualIndex = 0;
    UINT visibleCount = 0;
    *ppAdapter = NULL;

    for (;;) {
        IDXGIAdapter1* candidate = NULL;
        HRESULT hr = self->inner->lpVtbl->EnumAdapters1(
            self->inner, actualIndex++, &candidate);
        if (FAILED(hr))
            return hr;
        if (isNvidiaAdapter(candidate)) {
            candidate->lpVtbl->Release(candidate);
            continue;
        }
        if (visibleCount++ == visibleIndex) {
            *ppAdapter = candidate;
            return S_OK;
        }
        candidate->lpVtbl->Release(candidate);
    }
}

//-----------------------------------------------------------------------------
// AGS uses EnumDisplayDevicesW/A independently of DXGI. Patch only the IAT
// entries inside the real AMD AGS module; no AGS ABI or data layout is touched.
//-----------------------------------------------------------------------------
typedef BOOL (WINAPI *EnumDisplayDevicesW_t)(LPCWSTR, DWORD, PDISPLAY_DEVICEW, DWORD);
typedef BOOL (WINAPI *EnumDisplayDevicesA_t)(LPCSTR, DWORD, PDISPLAY_DEVICEA, DWORD);
static EnumDisplayDevicesW_t g_realEnumDisplayDevicesW = NULL;
static EnumDisplayDevicesA_t g_realEnumDisplayDevicesA = NULL;
static LONG g_agsEnumHookState = 0;

static BOOL isNvidiaDisplayDeviceW(const DISPLAY_DEVICEW* device) {
    return wcsstr(device->DeviceID, L"VEN_10DE") != NULL
        || wcsstr(device->DeviceString, L"NVIDIA") != NULL;
}

static BOOL isNvidiaDisplayDeviceA(const DISPLAY_DEVICEA* device) {
    return strstr(device->DeviceID, "VEN_10DE") != NULL
        || strstr(device->DeviceString, "NVIDIA") != NULL;
}

static BOOL WINAPI HookEnumDisplayDevicesW(LPCWSTR lpDevice, DWORD iDevNum,
    PDISPLAY_DEVICEW output, DWORD flags) {
    if (!g_realEnumDisplayDevicesW || !output || lpDevice)
        return g_realEnumDisplayDevicesW
            ? g_realEnumDisplayDevicesW(lpDevice, iDevNum, output, flags) : FALSE;

    DWORD rawIndex = 0, visibleIndex = 0;
    DISPLAY_DEVICEW candidate;
    const DWORD outputSize = output->cb;
    while (rawIndex < 64) {
        ZeroMemory(&candidate, sizeof(candidate));
        candidate.cb = sizeof(candidate);
        if (!g_realEnumDisplayDevicesW(NULL, rawIndex++, &candidate, flags))
            break;
        if (isNvidiaDisplayDeviceW(&candidate))
            continue;
        if (visibleIndex++ == iDevNum) {
            CopyMemory(output, &candidate,
                outputSize < sizeof(candidate) ? outputSize : sizeof(candidate));
            output->cb = outputSize;
            return TRUE;
        }
    }
    SetLastError(ERROR_NOT_FOUND);
    return FALSE;
}

static BOOL WINAPI HookEnumDisplayDevicesA(LPCSTR lpDevice, DWORD iDevNum,
    PDISPLAY_DEVICEA output, DWORD flags) {
    if (!g_realEnumDisplayDevicesA || !output || lpDevice)
        return g_realEnumDisplayDevicesA
            ? g_realEnumDisplayDevicesA(lpDevice, iDevNum, output, flags) : FALSE;

    DWORD rawIndex = 0, visibleIndex = 0;
    DISPLAY_DEVICEA candidate;
    const DWORD outputSize = output->cb;
    while (rawIndex < 64) {
        ZeroMemory(&candidate, sizeof(candidate));
        candidate.cb = sizeof(candidate);
        if (!g_realEnumDisplayDevicesA(NULL, rawIndex++, &candidate, flags))
            break;
        if (isNvidiaDisplayDeviceA(&candidate))
            continue;
        if (visibleIndex++ == iDevNum) {
            CopyMemory(output, &candidate,
                outputSize < sizeof(candidate) ? outputSize : sizeof(candidate));
            output->cb = outputSize;
            return TRUE;
        }
    }
    SetLastError(ERROR_NOT_FOUND);
    return FALSE;
}

static void patchAgsEnumDisplayDevicesIat(void) {
    if (InterlockedCompareExchange(&g_agsEnumHookState, 1, 0) != 0)
        return;

    HMODULE ags = GetModuleHandleW(L"amd_ags_x64.dll");
    if (!ags) {
        InterlockedExchange(&g_agsEnumHookState, 0);
        return;
    }

    BYTE* base = (BYTE*)ags;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    IMAGE_DATA_DIRECTORY imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return;

    IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + imports.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* dllName = (const char*)(base + desc->Name);
        if (strcmp(dllName, "USER32.dll") != 0)
            continue;

        IMAGE_THUNK_DATA64* names = (IMAGE_THUNK_DATA64*)(base +
            (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        IMAGE_THUNK_DATA64* iat = (IMAGE_THUNK_DATA64*)(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            IMAGE_IMPORT_BY_NAME* import = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            FARPROC replacement = NULL;
            if (strcmp((const char*)import->Name, "EnumDisplayDevicesW") == 0) {
                if (!g_realEnumDisplayDevicesW)
                    g_realEnumDisplayDevicesW = (EnumDisplayDevicesW_t)(ULONG_PTR)iat->u1.Function;
                replacement = (FARPROC)HookEnumDisplayDevicesW;
            } else if (strcmp((const char*)import->Name, "EnumDisplayDevicesA") == 0) {
                if (!g_realEnumDisplayDevicesA)
                    g_realEnumDisplayDevicesA = (EnumDisplayDevicesA_t)(ULONG_PTR)iat->u1.Function;
                replacement = (FARPROC)HookEnumDisplayDevicesA;
            }
            if (replacement) {
                DWORD oldProtect;
                if (VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), PAGE_READWRITE, &oldProtect)) {
                    iat->u1.Function = (ULONGLONG)(ULONG_PTR)replacement;
                    VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), oldProtect, &oldProtect);
                }
            }
        }
        break;
    }
}

//-----------------------------------------------------------------------------
// Broad process-local interception. This emulates the useful parts of an
// absent NVIDIA display device without changing system device state.
//-----------------------------------------------------------------------------
typedef HRESULT (WINAPI *D3D11CreateDevice_t)(IDXGIAdapter*, D3D_DRIVER_TYPE,
    HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
    D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT (WINAPI *D3D11CreateDeviceAndSwapChain_t)(IDXGIAdapter*,
    D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
    D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

typedef HMODULE (WINAPI *LoadLibraryW_t)(LPCWSTR);
typedef HMODULE (WINAPI *LoadLibraryA_t)(LPCSTR);
typedef HMODULE (WINAPI *LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);
typedef HMODULE (WINAPI *LoadLibraryExA_t)(LPCSTR, HANDLE, DWORD);
typedef FARPROC (WINAPI *GetProcAddress_t)(HMODULE, LPCSTR);
typedef BOOL (WINAPI *SetupDiEnumDeviceInfo_t)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *SetupDiGetDeviceRegistryPropertyW_t)(HDEVINFO,
    PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
typedef BOOL (WINAPI *SetupDiGetDeviceRegistryPropertyA_t)(HDEVINFO,
    PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
typedef CONFIGRET (WINAPI *CM_Get_Device_IDW_t)(DEVINST, PWSTR, ULONG, ULONG);
typedef CONFIGRET (WINAPI *CM_Get_Device_IDA_t)(DEVINST, PSTR, ULONG, ULONG);

static D3D11CreateDevice_t g_realD3D11CreateDevice = NULL;
static D3D11CreateDeviceAndSwapChain_t g_realD3D11CreateDeviceAndSwapChain = NULL;
static LoadLibraryW_t g_realLoadLibraryW = NULL;
static LoadLibraryA_t g_realLoadLibraryA = NULL;
static LoadLibraryExW_t g_realLoadLibraryExW = NULL;
static LoadLibraryExA_t g_realLoadLibraryExA = NULL;
static GetProcAddress_t g_realGetProcAddress = NULL;
static SetupDiEnumDeviceInfo_t g_realSetupDiEnumDeviceInfo = NULL;
static SetupDiGetDeviceRegistryPropertyW_t g_realSetupDiGetDeviceRegistryPropertyW = NULL;
static SetupDiGetDeviceRegistryPropertyA_t g_realSetupDiGetDeviceRegistryPropertyA = NULL;
static CM_Get_Device_IDW_t g_realCMGetDeviceIDW = NULL;
static CM_Get_Device_IDA_t g_realCMGetDeviceIDA = NULL;
static HMODULE g_selfModule = NULL;
static LONG g_installingHooks = 0;

#ifdef PROXY_VERSION
FARPROC g_proxyExports[17];
static const char* const g_proxyExportNames[17] = {
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle",
    "GetFileVersionInfoExA", "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW", "VerFindFileA", "VerFindFileW",
    "VerInstallFileA", "VerInstallFileW", "VerLanguageNameA",
    "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW"
};

static BOOL initializeVersionProxy(void) {
    wchar_t path[MAX_PATH];
    UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (!length || length + 13 >= MAX_PATH) return FALSE;
    lstrcatW(path, L"\\version.dll");
    HMODULE realVersion = LoadLibraryW(path);
    if (!realVersion) return FALSE;
    for (UINT i = 0; i < 17; ++i) {
        g_proxyExports[i] = GetProcAddress(realVersion, g_proxyExportNames[i]);
        if (!g_proxyExports[i]) return FALSE;
    }
    return TRUE;
}
#endif

static void logLine(const char* text) {
    char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(g_selfModule, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    while (length && path[length - 1] != '\\' && path[length - 1] != '/') --length;
    lstrcpyA(path + length, "nvcloak.log");
    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, text, lstrlenA(text), &written, NULL);
        WriteFile(file, "\r\n", 2, &written, NULL);
        CloseHandle(file);
    }
}

static BOOL containsInsensitiveA(const char* text, const char* needle) {
    if (!text || !needle) return FALSE;
    int n = lstrlenA(needle);
    for (; *text; ++text)
        if (CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, text, n,
                needle, n) == CSTR_EQUAL)
            return TRUE;
    return FALSE;
}

static BOOL containsInsensitiveW(const wchar_t* text, const wchar_t* needle) {
    if (!text || !needle) return FALSE;
    int n = lstrlenW(needle);
    for (; *text; ++text)
        if (CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, text, n,
                needle, n) == CSTR_EQUAL)
            return TRUE;
    return FALSE;
}

static BOOL isBlockedNvidiaLibraryA(LPCSTR name) {
    return name && (containsInsensitiveA(name, "nvapi.dll")
        || containsInsensitiveA(name, "nvapi64.dll"));
}

static BOOL isBlockedNvidiaLibraryW(LPCWSTR name) {
    return name && (containsInsensitiveW(name, L"nvapi.dll")
        || containsInsensitiveW(name, L"nvapi64.dll"));
}

static BOOL moduleIsNvapi(HMODULE module) {
    char path[MAX_PATH];
    return module && GetModuleFileNameA(module, path, MAX_PATH)
        && isBlockedNvidiaLibraryA(path);
}

static IDXGIAdapter1* findAmdAdapter(void) {
    CreateDXGIFactory1_t createFactory =
        (CreateDXGIFactory1_t)getRealProc("CreateDXGIFactory1");
    IDXGIFactory1* factory = NULL;
    if (!createFactory || FAILED(createFactory(&IID_IDXGIFactory1, (void**)&factory)))
        return NULL;

    IDXGIAdapter1* result = NULL;
    for (UINT index = 0; ; ++index) {
        IDXGIAdapter1* adapter = NULL;
        if (FAILED(factory->lpVtbl->EnumAdapters1(factory, index, &adapter)))
            break;
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->lpVtbl->GetDesc1(adapter, &desc))
                && desc.VendorId == 0x1002
                && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            result = adapter;
            break;
        }
        adapter->lpVtbl->Release(adapter);
    }
    factory->lpVtbl->Release(factory);
    return result;
}

static BOOL shouldReplaceAdapter(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type) {
    if (!adapter)
        return type == D3D_DRIVER_TYPE_HARDWARE;
    IDXGIAdapter1* adapter1 = NULL;
    BOOL replace = FALSE;
    if (SUCCEEDED(adapter->lpVtbl->QueryInterface(adapter,
            &IID_IDXGIAdapter1, (void**)&adapter1))) {
        replace = isNvidiaAdapter(adapter1);
        adapter1->lpVtbl->Release(adapter1);
    }
    return replace;
}

static HRESULT WINAPI HookD3D11CreateDevice(IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion,
    ID3D11Device** device, D3D_FEATURE_LEVEL* selectedLevel,
    ID3D11DeviceContext** context) {
    IDXGIAdapter1* amd = NULL;
    if (shouldReplaceAdapter(adapter, type)) {
        amd = findAmdAdapter();
        if (amd) {
            adapter = (IDXGIAdapter*)amd;
            type = D3D_DRIVER_TYPE_UNKNOWN;
            software = NULL;
            logLine("D3D11CreateDevice: forced AMD adapter");
        }
    }
    HRESULT hr = g_realD3D11CreateDevice
        ? g_realD3D11CreateDevice(adapter, type, software, flags, levels,
            levelCount, sdkVersion, device, selectedLevel, context) : E_FAIL;
    if (amd) amd->lpVtbl->Release(amd);
    return hr;
}

static HRESULT WINAPI HookD3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain,
    ID3D11Device** device, D3D_FEATURE_LEVEL* selectedLevel,
    ID3D11DeviceContext** context) {
    IDXGIAdapter1* amd = NULL;
    if (shouldReplaceAdapter(adapter, type)) {
        amd = findAmdAdapter();
        if (amd) {
            adapter = (IDXGIAdapter*)amd;
            type = D3D_DRIVER_TYPE_UNKNOWN;
            software = NULL;
            logLine("D3D11CreateDeviceAndSwapChain: forced AMD adapter");
        }
    }
    HRESULT hr = g_realD3D11CreateDeviceAndSwapChain
        ? g_realD3D11CreateDeviceAndSwapChain(adapter, type, software, flags,
            levels, levelCount, sdkVersion, desc, swapChain, device,
            selectedLevel, context) : E_FAIL;
    if (amd) amd->lpVtbl->Release(amd);
    return hr;
}

static void patchModuleImports(HMODULE module);
static BOOL isGameLocalModule(HMODULE module);

static HMODULE WINAPI HookLoadLibraryW(LPCWSTR name) {
    if (isBlockedNvidiaLibraryW(name)) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        logLine("Blocked LoadLibraryW for NVAPI");
        return NULL;
    }
    HMODULE result = g_realLoadLibraryW ? g_realLoadLibraryW(name) : NULL;
    if (result && isGameLocalModule(result)) patchModuleImports(result);
    return result;
}
static HMODULE WINAPI HookLoadLibraryA(LPCSTR name) {
    if (isBlockedNvidiaLibraryA(name)) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        logLine("Blocked LoadLibraryA for NVAPI");
        return NULL;
    }
    HMODULE result = g_realLoadLibraryA ? g_realLoadLibraryA(name) : NULL;
    if (result && isGameLocalModule(result)) patchModuleImports(result);
    return result;
}
static HMODULE WINAPI HookLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    if (isBlockedNvidiaLibraryW(name)) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        logLine("Blocked LoadLibraryExW for NVAPI");
        return NULL;
    }
    HMODULE result = g_realLoadLibraryExW ? g_realLoadLibraryExW(name, file, flags) : NULL;
    if (result && isGameLocalModule(result)) patchModuleImports(result);
    return result;
}
static HMODULE WINAPI HookLoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
    if (isBlockedNvidiaLibraryA(name)) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        logLine("Blocked LoadLibraryExA for NVAPI");
        return NULL;
    }
    HMODULE result = g_realLoadLibraryExA ? g_realLoadLibraryExA(name, file, flags) : NULL;
    if (result && isGameLocalModule(result)) patchModuleImports(result);
    return result;
}

static BOOL bufferHasNvidia(const BYTE* buffer, DWORD size) {
    if (!buffer || !size) return FALSE;
    // Device IDs are normally uppercase MULTI_SZ strings. Check both ANSI and
    // UTF-16 forms without assuming that the caller supplied a terminator.
    static const char ansiVendor[] = "VEN_10DE";
    static const wchar_t wideVendor[] = L"VEN_10DE";
    static const char ansiName[] = "NVIDIA";
    static const wchar_t wideName[] = L"NVIDIA";
    for (DWORD i = 0; i + sizeof(ansiVendor) - 1 <= size; ++i)
        if (memcmp(buffer + i, ansiVendor, sizeof(ansiVendor) - 1) == 0
                || (i + sizeof(ansiName) - 1 <= size
                    && memcmp(buffer + i, ansiName, sizeof(ansiName) - 1) == 0))
            return TRUE;
    for (DWORD i = 0; i + sizeof(wideVendor) - sizeof(wchar_t) <= size; i += sizeof(wchar_t))
        if (memcmp(buffer + i, wideVendor, sizeof(wideVendor) - sizeof(wchar_t)) == 0
                || (i + sizeof(wideName) - sizeof(wchar_t) <= size
                    && memcmp(buffer + i, wideName, sizeof(wideName) - sizeof(wchar_t)) == 0))
            return TRUE;
    return FALSE;
}

static BOOL deviceInfoIsNvidia(HDEVINFO set, PSP_DEVINFO_DATA data) {
    BYTE value[4096];
    DWORD type = 0, needed = 0;
    ZeroMemory(value, sizeof(value));
    if (g_realSetupDiGetDeviceRegistryPropertyW
            && g_realSetupDiGetDeviceRegistryPropertyW(set, data, SPDRP_HARDWAREID,
                &type, value, sizeof(value), &needed)
            && bufferHasNvidia(value, needed < sizeof(value) ? needed : sizeof(value)))
        return TRUE;
    ZeroMemory(value, sizeof(value));
    if (g_realSetupDiGetDeviceRegistryPropertyW
            && g_realSetupDiGetDeviceRegistryPropertyW(set, data, SPDRP_DEVICEDESC,
                &type, value, sizeof(value), &needed)
            && bufferHasNvidia(value, needed < sizeof(value) ? needed : sizeof(value)))
        return TRUE;
    return FALSE;
}

static BOOL WINAPI HookSetupDiEnumDeviceInfo(HDEVINFO set, DWORD visibleIndex,
    PSP_DEVINFO_DATA output) {
    if (!g_realSetupDiEnumDeviceInfo || !output) return FALSE;
    DWORD visible = 0;
    for (DWORD actual = 0; actual < 4096; ++actual) {
        SP_DEVINFO_DATA candidate;
        ZeroMemory(&candidate, sizeof(candidate));
        candidate.cbSize = sizeof(candidate);
        if (!g_realSetupDiEnumDeviceInfo(set, actual, &candidate)) return FALSE;
        if (deviceInfoIsNvidia(set, &candidate)) {
            logLine("SetupAPI: skipped NVIDIA devnode");
            continue;
        }
        if (visible++ == visibleIndex) {
            *output = candidate;
            return TRUE;
        }
    }
    SetLastError(ERROR_NO_MORE_ITEMS);
    return FALSE;
}

static BOOL WINAPI HookSetupDiGetDeviceRegistryPropertyW(HDEVINFO set,
    PSP_DEVINFO_DATA data, DWORD property, PDWORD type, PBYTE buffer,
    DWORD size, PDWORD required) {
    if (!g_realSetupDiGetDeviceRegistryPropertyW) return FALSE;
    BOOL ok = g_realSetupDiGetDeviceRegistryPropertyW(set, data, property,
        type, buffer, size, required);
    if (ok && bufferHasNvidia(buffer, required && *required < size ? *required : size)) {
        SetLastError(ERROR_INVALID_DATA);
        logLine("SetupAPI W: hid NVIDIA property");
        return FALSE;
    }
    return ok;
}

static BOOL WINAPI HookSetupDiGetDeviceRegistryPropertyA(HDEVINFO set,
    PSP_DEVINFO_DATA data, DWORD property, PDWORD type, PBYTE buffer,
    DWORD size, PDWORD required) {
    if (!g_realSetupDiGetDeviceRegistryPropertyA) return FALSE;
    BOOL ok = g_realSetupDiGetDeviceRegistryPropertyA(set, data, property,
        type, buffer, size, required);
    if (ok && bufferHasNvidia(buffer, required && *required < size ? *required : size)) {
        SetLastError(ERROR_INVALID_DATA);
        logLine("SetupAPI A: hid NVIDIA property");
        return FALSE;
    }
    return ok;
}

static CONFIGRET WINAPI HookCMGetDeviceIDW(DEVINST device, PWSTR buffer,
    ULONG length, ULONG flags) {
    if (!g_realCMGetDeviceIDW) return CR_FAILURE;
    CONFIGRET result = g_realCMGetDeviceIDW(device, buffer, length, flags);
    if (result == CR_SUCCESS && buffer
            && (containsInsensitiveW(buffer, L"VEN_10DE")
                || containsInsensitiveW(buffer, L"NVIDIA"))) {
        if (length) buffer[0] = 0;
        logLine("CfgMgr W: hid NVIDIA devnode");
        return CR_NO_SUCH_DEVNODE;
    }
    return result;
}

static CONFIGRET WINAPI HookCMGetDeviceIDA(DEVINST device, PSTR buffer,
    ULONG length, ULONG flags) {
    if (!g_realCMGetDeviceIDA) return CR_FAILURE;
    CONFIGRET result = g_realCMGetDeviceIDA(device, buffer, length, flags);
    if (result == CR_SUCCESS && buffer
            && (containsInsensitiveA(buffer, "VEN_10DE")
                || containsInsensitiveA(buffer, "NVIDIA"))) {
        if (length) buffer[0] = 0;
        logLine("CfgMgr A: hid NVIDIA devnode");
        return CR_NO_SUCH_DEVNODE;
    }
    return result;
}

static FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR name) {
    if (!name || ((ULONG_PTR)name >> 16) == 0)
        return g_realGetProcAddress ? g_realGetProcAddress(module, name) : NULL;
    if (moduleIsNvapi(module)) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        logLine("Blocked GetProcAddress for NVAPI");
        return NULL;
    }
    if (lstrcmpA(name, "CreateDXGIFactory") == 0)
        return (FARPROC)CreateDXGIFactory;
    if (lstrcmpA(name, "CreateDXGIFactory1") == 0)
        return (FARPROC)CreateDXGIFactory1;
    if (lstrcmpA(name, "CreateDXGIFactory2") == 0)
        return (FARPROC)CreateDXGIFactory2;
    if (lstrcmpA(name, "D3D11CreateDevice") == 0)
        return (FARPROC)HookD3D11CreateDevice;
    if (lstrcmpA(name, "D3D11CreateDeviceAndSwapChain") == 0)
        return (FARPROC)HookD3D11CreateDeviceAndSwapChain;
    if (lstrcmpA(name, "EnumDisplayDevicesW") == 0)
        return (FARPROC)HookEnumDisplayDevicesW;
    if (lstrcmpA(name, "EnumDisplayDevicesA") == 0)
        return (FARPROC)HookEnumDisplayDevicesA;
    if (lstrcmpA(name, "SetupDiEnumDeviceInfo") == 0)
        return (FARPROC)HookSetupDiEnumDeviceInfo;
    if (lstrcmpA(name, "SetupDiGetDeviceRegistryPropertyW") == 0)
        return (FARPROC)HookSetupDiGetDeviceRegistryPropertyW;
    if (lstrcmpA(name, "SetupDiGetDeviceRegistryPropertyA") == 0)
        return (FARPROC)HookSetupDiGetDeviceRegistryPropertyA;
    if (lstrcmpA(name, "CM_Get_Device_IDW") == 0)
        return (FARPROC)HookCMGetDeviceIDW;
    if (lstrcmpA(name, "CM_Get_Device_IDA") == 0)
        return (FARPROC)HookCMGetDeviceIDA;
    return g_realGetProcAddress ? g_realGetProcAddress(module, name) : NULL;
}

static FARPROC replacementForImport(const char* name, FARPROC current) {
    FARPROC replacement = NULL;
    if (lstrcmpA(name, "CreateDXGIFactory") == 0) {
        replacement = (FARPROC)CreateDXGIFactory;
    } else if (lstrcmpA(name, "CreateDXGIFactory1") == 0) {
        replacement = (FARPROC)CreateDXGIFactory1;
    } else if (lstrcmpA(name, "CreateDXGIFactory2") == 0) {
        replacement = (FARPROC)CreateDXGIFactory2;
    } else if (lstrcmpA(name, "EnumDisplayDevicesW") == 0) {
        if (!g_realEnumDisplayDevicesW) g_realEnumDisplayDevicesW = (EnumDisplayDevicesW_t)current;
        replacement = (FARPROC)HookEnumDisplayDevicesW;
    } else if (lstrcmpA(name, "EnumDisplayDevicesA") == 0) {
        if (!g_realEnumDisplayDevicesA) g_realEnumDisplayDevicesA = (EnumDisplayDevicesA_t)current;
        replacement = (FARPROC)HookEnumDisplayDevicesA;
    } else if (lstrcmpA(name, "D3D11CreateDevice") == 0) {
        if (!g_realD3D11CreateDevice) g_realD3D11CreateDevice = (D3D11CreateDevice_t)current;
        replacement = (FARPROC)HookD3D11CreateDevice;
    } else if (lstrcmpA(name, "D3D11CreateDeviceAndSwapChain") == 0) {
        if (!g_realD3D11CreateDeviceAndSwapChain)
            g_realD3D11CreateDeviceAndSwapChain = (D3D11CreateDeviceAndSwapChain_t)current;
        replacement = (FARPROC)HookD3D11CreateDeviceAndSwapChain;
    } else if (lstrcmpA(name, "SetupDiEnumDeviceInfo") == 0) {
        if (!g_realSetupDiEnumDeviceInfo) g_realSetupDiEnumDeviceInfo = (SetupDiEnumDeviceInfo_t)current;
        replacement = (FARPROC)HookSetupDiEnumDeviceInfo;
    } else if (lstrcmpA(name, "SetupDiGetDeviceRegistryPropertyW") == 0) {
        if (!g_realSetupDiGetDeviceRegistryPropertyW)
            g_realSetupDiGetDeviceRegistryPropertyW = (SetupDiGetDeviceRegistryPropertyW_t)current;
        replacement = (FARPROC)HookSetupDiGetDeviceRegistryPropertyW;
    } else if (lstrcmpA(name, "SetupDiGetDeviceRegistryPropertyA") == 0) {
        if (!g_realSetupDiGetDeviceRegistryPropertyA)
            g_realSetupDiGetDeviceRegistryPropertyA = (SetupDiGetDeviceRegistryPropertyA_t)current;
        replacement = (FARPROC)HookSetupDiGetDeviceRegistryPropertyA;
    } else if (lstrcmpA(name, "CM_Get_Device_IDW") == 0) {
        if (!g_realCMGetDeviceIDW) g_realCMGetDeviceIDW = (CM_Get_Device_IDW_t)current;
        replacement = (FARPROC)HookCMGetDeviceIDW;
    } else if (lstrcmpA(name, "CM_Get_Device_IDA") == 0) {
        if (!g_realCMGetDeviceIDA) g_realCMGetDeviceIDA = (CM_Get_Device_IDA_t)current;
        replacement = (FARPROC)HookCMGetDeviceIDA;
    } else if (lstrcmpA(name, "LoadLibraryW") == 0) replacement = (FARPROC)HookLoadLibraryW;
    else if (lstrcmpA(name, "LoadLibraryA") == 0) replacement = (FARPROC)HookLoadLibraryA;
    else if (lstrcmpA(name, "LoadLibraryExW") == 0) replacement = (FARPROC)HookLoadLibraryExW;
    else if (lstrcmpA(name, "LoadLibraryExA") == 0) replacement = (FARPROC)HookLoadLibraryExA;
    else if (lstrcmpA(name, "GetProcAddress") == 0) replacement = (FARPROC)HookGetProcAddress;
    return replacement;
}

static void patchModuleImports(HMODULE module) {
    if (!module || module == g_selfModule || module == g_realDxgi) return;
    BYTE* base = (BYTE*)module;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    IMAGE_DATA_DIRECTORY imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return;

    IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + imports.VirtualAddress);
    for (; desc->Name; ++desc) {
        if (!desc->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA64* names = (IMAGE_THUNK_DATA64*)(base + desc->OriginalFirstThunk);
        IMAGE_THUNK_DATA64* iat = (IMAGE_THUNK_DATA64*)(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* import = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            FARPROC current = (FARPROC)(ULONG_PTR)iat->u1.Function;
            FARPROC replacement = replacementForImport((const char*)import->Name, current);
            if (replacement && current != replacement) {
                DWORD oldProtect;
                if (VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function),
                        PAGE_READWRITE, &oldProtect)) {
                    iat->u1.Function = (ULONGLONG)(ULONG_PTR)replacement;
                    VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function),
                        oldProtect, &oldProtect);
                    FlushInstructionCache(GetCurrentProcess(), &iat->u1.Function,
                        sizeof(iat->u1.Function));
                }
            }
        }
    }
}

static BOOL isGameLocalModule(HMODULE module) {
    wchar_t exePath[MAX_PATH], modulePath[MAX_PATH];
    DWORD exeLength = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    DWORD moduleLength = GetModuleFileNameW(module, modulePath, MAX_PATH);
    if (!exeLength || !moduleLength) return FALSE;
    while (exeLength && exePath[exeLength - 1] != L'\\' && exePath[exeLength - 1] != L'/')
        --exeLength;
    return exeLength && moduleLength >= exeLength
        && CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, exePath, exeLength,
            modulePath, exeLength) == CSTR_EQUAL;
}

static void initializeRealFunctions(void) {
    if (!g_realGetProcAddress) g_realGetProcAddress = GetProcAddress;
    if (!g_realLoadLibraryW) g_realLoadLibraryW = LoadLibraryW;
    if (!g_realLoadLibraryA) g_realLoadLibraryA = LoadLibraryA;
    if (!g_realLoadLibraryExW) g_realLoadLibraryExW = LoadLibraryExW;
    if (!g_realLoadLibraryExA) g_realLoadLibraryExA = LoadLibraryExA;

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        if (!g_realEnumDisplayDevicesW)
            g_realEnumDisplayDevicesW = (EnumDisplayDevicesW_t)
                g_realGetProcAddress(user32, "EnumDisplayDevicesW");
        if (!g_realEnumDisplayDevicesA)
            g_realEnumDisplayDevicesA = (EnumDisplayDevicesA_t)
                g_realGetProcAddress(user32, "EnumDisplayDevicesA");
    }
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (d3d11) {
        if (!g_realD3D11CreateDevice)
            g_realD3D11CreateDevice = (D3D11CreateDevice_t)
                g_realGetProcAddress(d3d11, "D3D11CreateDevice");
        if (!g_realD3D11CreateDeviceAndSwapChain)
            g_realD3D11CreateDeviceAndSwapChain = (D3D11CreateDeviceAndSwapChain_t)
                g_realGetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
    }
    HMODULE setupapi = GetModuleHandleW(L"setupapi.dll");
    if (setupapi) {
        if (!g_realSetupDiEnumDeviceInfo)
            g_realSetupDiEnumDeviceInfo = (SetupDiEnumDeviceInfo_t)
                g_realGetProcAddress(setupapi, "SetupDiEnumDeviceInfo");
        if (!g_realSetupDiGetDeviceRegistryPropertyW)
            g_realSetupDiGetDeviceRegistryPropertyW = (SetupDiGetDeviceRegistryPropertyW_t)
                g_realGetProcAddress(setupapi, "SetupDiGetDeviceRegistryPropertyW");
        if (!g_realSetupDiGetDeviceRegistryPropertyA)
            g_realSetupDiGetDeviceRegistryPropertyA = (SetupDiGetDeviceRegistryPropertyA_t)
                g_realGetProcAddress(setupapi, "SetupDiGetDeviceRegistryPropertyA");
    }
    HMODULE cfgmgr = GetModuleHandleW(L"cfgmgr32.dll");
    if (cfgmgr) {
        if (!g_realCMGetDeviceIDW)
            g_realCMGetDeviceIDW = (CM_Get_Device_IDW_t)
                g_realGetProcAddress(cfgmgr, "CM_Get_Device_IDW");
        if (!g_realCMGetDeviceIDA)
            g_realCMGetDeviceIDA = (CM_Get_Device_IDA_t)
                g_realGetProcAddress(cfgmgr, "CM_Get_Device_IDA");
    }
}

static void installBroadHooks(BOOL allModules) {
    if (InterlockedCompareExchange(&g_installingHooks, 1, 0) != 0) return;
    initializeRealFunctions();
    patchModuleImports(GetModuleHandleW(NULL));
    patchModuleImports(GetModuleHandleW(L"amd_ags_x64.dll"));
    if (allModules) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snapshot != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W entry;
            ZeroMemory(&entry, sizeof(entry));
            entry.dwSize = sizeof(entry);
            if (Module32FirstW(snapshot, &entry)) {
                do {
                    if (isGameLocalModule(entry.hModule))
                        patchModuleImports(entry.hModule);
                } while (Module32NextW(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }
    }
    InterlockedExchange(&g_installingHooks, 0);
}

//-----------------------------------------------------------------------------
// FilterFactory implementations - wraps IDXGIFactory7 and filters NVIDIA
//-----------------------------------------------------------------------------

// IUnknown
static HRESULT STDMETHODCALLTYPE FF_QueryInterface(
    IDXGIFactory7* This, REFIID riid, void** ppvObj) {
    FilterFactory* self = (FilterFactory*)This;
    if (!ppvObj)
        return E_POINTER;
    *ppvObj = NULL;

    // Never hand the real factory back for a factory interface, otherwise
    // callers would bypass the adapter filter after QueryInterface.
    if (isFactoryIid(riid)) {
        *ppvObj = This;
        InterlockedIncrement(&self->refcount);
        return S_OK;
    }
    return self->inner->lpVtbl->QueryInterface(self->inner, riid, ppvObj);
}

static ULONG STDMETHODCALLTYPE FF_AddRef(IDXGIFactory7* This) {
    FilterFactory* self = (FilterFactory*)This;
    return InterlockedIncrement(&self->refcount);
}

static ULONG STDMETHODCALLTYPE FF_Release(IDXGIFactory7* This) {
    FilterFactory* self = (FilterFactory*)This;
    ULONG refs = InterlockedDecrement(&self->refcount);
    if (refs == 0) {
        self->inner->lpVtbl->Release(self->inner);
        HeapFree(GetProcessHeap(), 0, self);
    }
    return refs;
}

// IDXGIObject
static HRESULT STDMETHODCALLTYPE FF_SetPrivateData(
    IDXGIFactory7* This, REFGUID Name, UINT DataSize, const void* pData) {
    return ((FilterFactory*)This)->inner->lpVtbl->SetPrivateData(
        ((FilterFactory*)This)->inner, Name, DataSize, pData);
}

static HRESULT STDMETHODCALLTYPE FF_SetPrivateDataInterface(
    IDXGIFactory7* This, REFGUID Name, const IUnknown* pUnknown) {
    return ((FilterFactory*)This)->inner->lpVtbl->SetPrivateDataInterface(
        ((FilterFactory*)This)->inner, Name, pUnknown);
}

static HRESULT STDMETHODCALLTYPE FF_GetPrivateData(
    IDXGIFactory7* This, REFGUID Name, UINT* pDataSize, void* pData) {
    return ((FilterFactory*)This)->inner->lpVtbl->GetPrivateData(
        ((FilterFactory*)This)->inner, Name, pDataSize, pData);
}

static HRESULT STDMETHODCALLTYPE FF_GetParent(
    IDXGIFactory7* This, REFIID riid, void** ppParent) {
    return ((FilterFactory*)This)->inner->lpVtbl->GetParent(
        ((FilterFactory*)This)->inner, riid, ppParent);
}

// IDXGIFactory
static HRESULT STDMETHODCALLTYPE FF_EnumAdapters(
    IDXGIFactory7* This, UINT Adapter, IDXGIAdapter** ppAdapter) {
    if (!ppAdapter)
        return E_POINTER;
    IDXGIAdapter1* adapter1 = NULL;
    HRESULT hr = getVisibleAdapter((FilterFactory*)This, Adapter, &adapter1);
    if (FAILED(hr)) {
        *ppAdapter = NULL;
        return hr;
    }
    *ppAdapter = (IDXGIAdapter*)adapter1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FF_MakeWindowAssociation(
    IDXGIFactory7* This, HWND WindowHandle, UINT Flags) {
    return ((FilterFactory*)This)->inner->lpVtbl->MakeWindowAssociation(
        ((FilterFactory*)This)->inner, WindowHandle, Flags);
}

static HRESULT STDMETHODCALLTYPE FF_GetWindowAssociation(
    IDXGIFactory7* This, HWND* pWindowHandle) {
    return ((FilterFactory*)This)->inner->lpVtbl->GetWindowAssociation(
        ((FilterFactory*)This)->inner, pWindowHandle);
}

static HRESULT STDMETHODCALLTYPE FF_CreateSwapChain(
    IDXGIFactory7* This, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
    IDXGISwapChain** ppSwapChain) {
    return ((FilterFactory*)This)->inner->lpVtbl->CreateSwapChain(
        ((FilterFactory*)This)->inner, pDevice, pDesc, ppSwapChain);
}

static HRESULT STDMETHODCALLTYPE FF_CreateSoftwareAdapter(
    IDXGIFactory7* This, HMODULE Module, IDXGIAdapter** ppAdapter) {
    return ((FilterFactory*)This)->inner->lpVtbl->CreateSoftwareAdapter(
        ((FilterFactory*)This)->inner, Module, ppAdapter);
}

// IDXGIFactory1
static HRESULT STDMETHODCALLTYPE FF_EnumAdapters1(
    IDXGIFactory7* This, UINT Adapter, IDXGIAdapter1** ppAdapter) {
    if (!ppAdapter)
        return E_POINTER;
    HRESULT hr = getVisibleAdapter((FilterFactory*)This, Adapter, ppAdapter);
    if (FAILED(hr))
        *ppAdapter = NULL;
    return hr;
}

static BOOL STDMETHODCALLTYPE FF_IsCurrent(IDXGIFactory7* This) {
    return ((FilterFactory*)This)->inner->lpVtbl->IsCurrent(
        ((FilterFactory*)This)->inner);
}

// IDXGIFactory2
static BOOL STDMETHODCALLTYPE FF_IsWindowedStereoEnabled(IDXGIFactory7* This) {
    return ((FilterFactory*)This)->inner->lpVtbl->IsWindowedStereoEnabled(
        ((FilterFactory*)This)->inner);
}

static HRESULT STDMETHODCALLTYPE FF_CreateSwapChainForHwnd(
    IDXGIFactory7* This, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    return ((FilterFactory*)This)->inner->lpVtbl->CreateSwapChainForHwnd(
        ((FilterFactory*)This)->inner, pDevice, hWnd, pDesc,
        pFullscreenDesc, pRestrictToOutput, ppSwapChain);
}

static HRESULT STDMETHODCALLTYPE FF_CreateSwapChainForCoreWindow(
    IDXGIFactory7* This, IUnknown* pDevice, IUnknown* pWindow,
    const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) {
    return ((FilterFactory*)This)->inner->lpVtbl->CreateSwapChainForCoreWindow(
        ((FilterFactory*)This)->inner, pDevice, pWindow, pDesc,
        pRestrictToOutput, ppSwapChain);
}

static HRESULT STDMETHODCALLTYPE FF_GetSharedResourceAdapterLuid(
    IDXGIFactory7* This, HANDLE hResource, LUID* pLuid) {
    return ((FilterFactory*)This)->inner->lpVtbl->GetSharedResourceAdapterLuid(
        ((FilterFactory*)This)->inner, hResource, pLuid);
}

static HRESULT STDMETHODCALLTYPE FF_RegisterStereoStatusWindow(
    IDXGIFactory7* This, HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) {
    return ((FilterFactory*)This)->inner->lpVtbl->RegisterStereoStatusWindow(
        ((FilterFactory*)This)->inner, WindowHandle, wMsg, pdwCookie);
}

static HRESULT STDMETHODCALLTYPE FF_RegisterStereoStatusEvent(
    IDXGIFactory7* This, HANDLE hEvent, DWORD* pdwCookie) {
    return ((FilterFactory*)This)->inner->lpVtbl->RegisterStereoStatusEvent(
        ((FilterFactory*)This)->inner, hEvent, pdwCookie);
}

static void STDMETHODCALLTYPE FF_UnregisterStereoStatus(
    IDXGIFactory7* This, DWORD dwCookie) {
    ((FilterFactory*)This)->inner->lpVtbl->UnregisterStereoStatus(
        ((FilterFactory*)This)->inner, dwCookie);
}

static HRESULT STDMETHODCALLTYPE FF_RegisterOcclusionStatusWindow(
    IDXGIFactory7* This, HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) {
    return ((FilterFactory*)This)->inner->lpVtbl->RegisterOcclusionStatusWindow(
        ((FilterFactory*)This)->inner, WindowHandle, wMsg, pdwCookie);
}

static HRESULT STDMETHODCALLTYPE FF_RegisterOcclusionStatusEvent(
    IDXGIFactory7* This, HANDLE hEvent, DWORD* pdwCookie) {
    return ((FilterFactory*)This)->inner->lpVtbl->RegisterOcclusionStatusEvent(
        ((FilterFactory*)This)->inner, hEvent, pdwCookie);
}

static void STDMETHODCALLTYPE FF_UnregisterOcclusionStatus(
    IDXGIFactory7* This, DWORD dwCookie) {
    ((FilterFactory*)This)->inner->lpVtbl->UnregisterOcclusionStatus(
        ((FilterFactory*)This)->inner, dwCookie);
}

static HRESULT STDMETHODCALLTYPE FF_CreateSwapChainForComposition(
    IDXGIFactory7* This, IUnknown* pDevice,
    const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) {
    return ((FilterFactory*)This)->inner->lpVtbl->CreateSwapChainForComposition(
        ((FilterFactory*)This)->inner, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
}

// IDXGIFactory3
static UINT STDMETHODCALLTYPE FF_GetCreationFlags(IDXGIFactory7* This) {
    return ((FilterFactory*)This)->inner->lpVtbl->GetCreationFlags(
        ((FilterFactory*)This)->inner);
}

// IDXGIFactory4
static HRESULT STDMETHODCALLTYPE FF_EnumAdapterByLuid(
    IDXGIFactory7* This, LUID AdapterLuid, REFIID riid, void** ppvAdapter) {
    if (!ppvAdapter)
        return E_POINTER;
    *ppvAdapter = NULL;

    FilterFactory* self = (FilterFactory*)This;
    IDXGIAdapter1* adapter1 = NULL;
    HRESULT hr = self->inner->lpVtbl->EnumAdapterByLuid(
        self->inner, AdapterLuid, &IID_IDXGIAdapter1, (void**)&adapter1);
    if (FAILED(hr))
        return hr;
    if (isNvidiaAdapter(adapter1)) {
        adapter1->lpVtbl->Release(adapter1);
        return DXGI_ERROR_NOT_FOUND;
    }
    hr = adapter1->lpVtbl->QueryInterface(adapter1, riid, ppvAdapter);
    adapter1->lpVtbl->Release(adapter1);
    return hr;
}

static HRESULT STDMETHODCALLTYPE FF_EnumWarpAdapter(
    IDXGIFactory7* This, REFIID riid, void** ppvAdapter) {
    return ((FilterFactory*)This)->inner->lpVtbl->EnumWarpAdapter(
        ((FilterFactory*)This)->inner, riid, ppvAdapter);
}

// IDXGIFactory5
static HRESULT STDMETHODCALLTYPE FF_CheckFeatureSupport(
    IDXGIFactory7* This, DXGI_FEATURE Feature, void* pFeatureSupportData,
    UINT FeatureSupportDataSize) {
    return ((FilterFactory*)This)->inner->lpVtbl->CheckFeatureSupport(
        ((FilterFactory*)This)->inner, Feature, pFeatureSupportData,
        FeatureSupportDataSize);
}

// IDXGIFactory6
static HRESULT STDMETHODCALLTYPE FF_EnumAdapterByGpuPreference(
    IDXGIFactory7* This, UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference,
    REFIID riid, void** ppvAdapter) {
    if (!ppvAdapter)
        return E_POINTER;
    *ppvAdapter = NULL;

    FilterFactory* self = (FilterFactory*)This;
    UINT actualIndex = 0;
    UINT visibleCount = 0;
    for (;;) {
        IDXGIAdapter1* candidate = NULL;
        HRESULT hr = self->inner->lpVtbl->EnumAdapterByGpuPreference(
            self->inner, actualIndex++, GpuPreference, &IID_IDXGIAdapter1,
            (void**)&candidate);
        if (FAILED(hr))
            return hr;
        if (isNvidiaAdapter(candidate)) {
            candidate->lpVtbl->Release(candidate);
            continue;
        }
        if (visibleCount++ == Adapter) {
            hr = candidate->lpVtbl->QueryInterface(candidate, riid, ppvAdapter);
            candidate->lpVtbl->Release(candidate);
            return hr;
        }
        candidate->lpVtbl->Release(candidate);
    }
}

// IDXGIFactory7
static HRESULT STDMETHODCALLTYPE FF_RegisterAdaptersChangedEvent(
    IDXGIFactory7* This, HANDLE hEvent, DWORD* pdwCookie) {
    return ((FilterFactory*)This)->inner->lpVtbl->RegisterAdaptersChangedEvent(
        ((FilterFactory*)This)->inner, hEvent, pdwCookie);
}

static HRESULT STDMETHODCALLTYPE FF_UnregisterAdaptersChangedEvent(
    IDXGIFactory7* This, DWORD dwCookie) {
    return ((FilterFactory*)This)->inner->lpVtbl->UnregisterAdaptersChangedEvent(
        ((FilterFactory*)This)->inner, dwCookie);
}

// Vtable - must match SDK's IDXGIFactory7Vtbl exactly
static const IDXGIFactory7Vtbl g_filterVtbl = {
    /* IUnknown */
    FF_QueryInterface,
    FF_AddRef,
    FF_Release,
    /* IDXGIObject */
    FF_SetPrivateData,
    FF_SetPrivateDataInterface,
    FF_GetPrivateData,
    FF_GetParent,
    /* IDXGIFactory */
    FF_EnumAdapters,
    FF_MakeWindowAssociation,
    FF_GetWindowAssociation,
    FF_CreateSwapChain,
    FF_CreateSoftwareAdapter,
    /* IDXGIFactory1 */
    FF_EnumAdapters1,
    FF_IsCurrent,
    /* IDXGIFactory2 */
    FF_IsWindowedStereoEnabled,
    FF_CreateSwapChainForHwnd,
    FF_CreateSwapChainForCoreWindow,
    FF_GetSharedResourceAdapterLuid,
    FF_RegisterStereoStatusWindow,
    FF_RegisterStereoStatusEvent,
    FF_UnregisterStereoStatus,
    FF_RegisterOcclusionStatusWindow,
    FF_RegisterOcclusionStatusEvent,
    FF_UnregisterOcclusionStatus,
    FF_CreateSwapChainForComposition,
    /* IDXGIFactory3 */
    FF_GetCreationFlags,
    /* IDXGIFactory4 */
    FF_EnumAdapterByLuid,
    FF_EnumWarpAdapter,
    /* IDXGIFactory5 */
    FF_CheckFeatureSupport,
    /* IDXGIFactory6 */
    FF_EnumAdapterByGpuPreference,
    /* IDXGIFactory7 */
    FF_RegisterAdaptersChangedEvent,
    FF_UnregisterAdaptersChangedEvent,
};

//-----------------------------------------------------------------------------
// Create a filtered factory wrapper
//-----------------------------------------------------------------------------
static HRESULT createFilteredFactory(IDXGIFactory7* factory7, void** ppFactory) {
    if (!factory7 || !ppFactory)
        return E_INVALIDARG;

    FilterFactory* wrapper = (FilterFactory*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FilterFactory));
    if (!wrapper) {
        factory7->lpVtbl->Release(factory7);
        return E_OUTOFMEMORY;
    }

    wrapper->lpVtbl = &g_filterVtbl;
    wrapper->inner = factory7; // Real Factory7 reference is owned by wrapper.
    wrapper->refcount = 1;
    *ppFactory = wrapper;
    return S_OK;
}

// ============================================================================
// Intercepted CreateDXGIFactory functions
// ============================================================================

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
    logLine("CreateDXGIFactory: returning filtered factory");
    patchAgsEnumDisplayDevicesIat();
    installBroadHooks(TRUE);
    if (!ppFactory || !isFactoryIid(riid))
        return E_NOINTERFACE;
    CreateDXGIFactory_t real = (CreateDXGIFactory_t)getRealProc("CreateDXGIFactory");
    if (!real) return E_FAIL;
    IDXGIFactory7* factory7 = NULL;
    HRESULT hr = real(&IID_IDXGIFactory7, (void**)&factory7);
    return FAILED(hr) ? hr : createFilteredFactory(factory7, ppFactory);
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    logLine("CreateDXGIFactory1: returning filtered factory");
    patchAgsEnumDisplayDevicesIat();
    installBroadHooks(TRUE);
    if (!ppFactory || !isFactoryIid(riid))
        return E_NOINTERFACE;
    CreateDXGIFactory1_t real = (CreateDXGIFactory1_t)getRealProc("CreateDXGIFactory1");
    if (!real) return E_FAIL;
    IDXGIFactory7* factory7 = NULL;
    HRESULT hr = real(&IID_IDXGIFactory7, (void**)&factory7);
    return FAILED(hr) ? hr : createFilteredFactory(factory7, ppFactory);
}

HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    logLine("CreateDXGIFactory2: returning filtered factory");
    patchAgsEnumDisplayDevicesIat();
    installBroadHooks(TRUE);
    if (!ppFactory || !isFactoryIid(riid))
        return E_NOINTERFACE;
    CreateDXGIFactory2_t real = (CreateDXGIFactory2_t)getRealProc("CreateDXGIFactory2");
    if (!real) return E_FAIL;
    IDXGIFactory7* factory7 = NULL;
    HRESULT hr = real(Flags, &IID_IDXGIFactory7, (void**)&factory7);
    return FAILED(hr) ? hr : createFilteredFactory(factory7, ppFactory);
}

//-----------------------------------------------------------------------------
// DLL entry point
//-----------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_selfModule = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
#ifdef PROXY_VERSION
        if (!initializeVersionProxy()) return FALSE;
#endif
#ifdef GENERIC_PROXY
        extern BOOL initializeGenericProxy(void);
        if (!initializeGenericProxy()) return FALSE;
#endif
        initializeRealFunctions();
        patchAgsEnumDisplayDevicesIat();
        installBroadHooks(FALSE);
        logLine("NVCloak shim loaded");
    }
    return TRUE;
}
