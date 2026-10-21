// winusb_input.h - TASOLLER PLUS "WINUSB mode" reader.
// Included from main.cpp after logging / state helpers are defined.
//
// The stock segatools driver (tasoller_plus.dll) talks to vendor interface
// MI_00 of VID_0E8F:PID_1231 through WinUSB. We only read from it, so the
// viewer can show the panel while no game is running.
#pragma once
#include <winusb.h>
#include <usb100.h>
#include <objbase.h>
#include <cfgmgr32.h>

// From the device's DeviceInterfaceGUIDs registry value. The segatools driver
// reads the same value; this is just the known value for TASOLLER PLUS.
static const GUID kTasollerVendorGuid = {0x1d4b2365, 0x4749, 0x48ea, {0xb3, 0x8a, 0x7c, 0x6f, 0xdd, 0xdd, 0x7e, 0x26}};

struct WinUsbApi {
    HMODULE mod = nullptr;
    BOOL(WINAPI* Initialize)(HANDLE, PWINUSB_INTERFACE_HANDLE) = nullptr;
    BOOL(WINAPI* Free)(WINUSB_INTERFACE_HANDLE) = nullptr;
    BOOL(WINAPI* QueryInterfaceSettings)(WINUSB_INTERFACE_HANDLE, UCHAR, PUSB_INTERFACE_DESCRIPTOR) = nullptr;
    BOOL(WINAPI* QueryPipe)(WINUSB_INTERFACE_HANDLE, UCHAR, UCHAR, PWINUSB_PIPE_INFORMATION) = nullptr;
    BOOL(WINAPI* ReadPipe)(WINUSB_INTERFACE_HANDLE, UCHAR, PUCHAR, ULONG, PULONG, LPOVERLAPPED) = nullptr;
};
static WinUsbApi g_wu;

static bool LoadWinUsb() {
    if (g_wu.mod) return true;
    HMODULE m = LoadLibraryA("winusb.dll");
    if (!m) return false;
    g_wu.Initialize = (decltype(g_wu.Initialize))GetProcAddress(m, "WinUsb_Initialize");
    g_wu.Free = (decltype(g_wu.Free))GetProcAddress(m, "WinUsb_Free");
    g_wu.QueryInterfaceSettings = (decltype(g_wu.QueryInterfaceSettings))GetProcAddress(m, "WinUsb_QueryInterfaceSettings");
    g_wu.QueryPipe = (decltype(g_wu.QueryPipe))GetProcAddress(m, "WinUsb_QueryPipe");
    g_wu.ReadPipe = (decltype(g_wu.ReadPipe))GetProcAddress(m, "WinUsb_ReadPipe");
    if (!g_wu.Initialize || !g_wu.Free || !g_wu.QueryInterfaceSettings || !g_wu.QueryPipe || !g_wu.ReadPipe) {
        g_wu.mod = nullptr;
        return false;
    }
    g_wu.mod = m;
    return true;
}

static std::vector<std::string> EnumInterfacePaths(const GUID& guid) {
    std::vector<std::string> out;
    HDEVINFO di = SetupDiGetClassDevsA(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA did{};
    did.cbSize = sizeof(did);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, nullptr, &guid, i, &did); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(di, &did, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<char> buf(need);
        auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*)buf.data();
        det->cbSize = sizeof(*det);
        if (!SetupDiGetDeviceInterfaceDetailA(di, &did, det, need, nullptr, nullptr)) continue;
        out.emplace_back(det->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(di);
    return out;
}

static bool PathMatchesTasoller(const std::string& p) {
    std::string low = Lower(p);
    struct IdPair { unsigned vid, pid; };
    const IdPair ids[] = {{0x0E8F, 0x1231}, {0x1CCF, 0x2333}, {(unsigned)g_cfg.vid, (unsigned)g_cfg.pid}};
    for (auto& id : ids) {
        char vid[16], pid[16];
        sprintf(vid, "vid_%04x", id.vid);
        sprintf(pid, "pid_%04x", id.pid);
        if (low.find(vid) != std::string::npos && low.find(pid) != std::string::npos) return true;
    }
    return false;
}

// Fallback discovery: find the USB device node, read its DeviceInterfaceGUIDs
// registry value and enumerate that interface, exactly like the driver does.
static std::vector<std::string> DiscoverViaRegistry() {
    std::vector<std::string> out;
    static const GUID usbDev = {0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};
    HDEVINFO di = SetupDiGetClassDevsA(&usbDev, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA did{};
    did.cbSize = sizeof(did);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, nullptr, &usbDev, i, &did); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(di, &did, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<char> buf(need);
        auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*)buf.data();
        det->cbSize = sizeof(*det);
        SP_DEVINFO_DATA info{};
        info.cbSize = sizeof(info);
        if (!SetupDiGetDeviceInterfaceDetailA(di, &did, det, need, nullptr, &info)) continue;
        if (!PathMatchesTasoller(det->DevicePath)) continue;
        HKEY key = SetupDiOpenDevRegKey(di, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        wchar_t val[512];
        DWORD size = sizeof(val), type = 0;
        LONG rc = RegQueryValueExW(key, L"DeviceInterfaceGUIDs", nullptr, &type, (LPBYTE)val, &size);
        RegCloseKey(key);
        if (rc != ERROR_SUCCESS || type != REG_MULTI_SZ) continue;
        for (wchar_t* s = val; *s; s += wcslen(s) + 1) {
            GUID g;
            if (FAILED(IIDFromString(s, &g))) continue;
            for (auto& p : EnumInterfacePaths(g)) out.push_back(p);
        }
    }
    SetupDiDestroyDeviceInfoList(di);
    return out;
}

static bool WinUsbOpen(const std::string& path, HANDLE& file, WINUSB_INTERFACE_HANDLE& iface,
                       std::vector<std::pair<UCHAR, int>>& pipes) {
    file = INVALID_HANDLE_VALUE;
    iface = nullptr;
    file = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        static DWORD lastLog = 0;
        DWORD now = GetTickCount();
        if (now - lastLog > 10000) { Log("winusb: CreateFile failed (%lu)", e); lastLog = now; }
        return false;
    }
    if (!g_wu.Initialize(file, &iface)) {
        Log("winusb: WinUsb_Initialize failed (%lu)", GetLastError());
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        return false;
    }
    USB_INTERFACE_DESCRIPTOR desc{};
    if (!g_wu.QueryInterfaceSettings(iface, 0, &desc)) {
        Log("winusb: QueryInterfaceSettings failed (%lu)", GetLastError());
        g_wu.Free(iface);
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        return false;
    }
    Log("winusb: interface class=%u endpoints=%u", desc.bInterfaceClass, desc.bNumEndpoints);
    for (UCHAR i = 0; i < desc.bNumEndpoints; i++) {
        WINUSB_PIPE_INFORMATION pi{};
        if (!g_wu.QueryPipe(iface, 0, i, &pi)) continue;
        Log("winusb: pipe 0x%02X type=%u maxPacket=%u", pi.PipeId, pi.PipeType, pi.MaximumPacketSize);
        if (pi.PipeId & 0x80) pipes.push_back({pi.PipeId, pi.MaximumPacketSize});
    }
    return true;
}

static bool TryReadOnce(WINUSB_INTERFACE_HANDLE iface, UCHAR pipeId, int maxPacket, ULONG& got,
                        std::vector<uint8_t>& out, DWORD timeoutMs) {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return false;
    out.resize(maxPacket > 0 ? maxPacket * 4 : 256);
    OVERLAPPED ov{};
    ov.hEvent = ev;
    got = 0;
    BOOL ok = g_wu.ReadPipe(iface, pipeId, out.data(), (ULONG)out.size(), &got, &ov);
    bool have = false;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ev, timeoutMs) == WAIT_OBJECT_0 &&
            GetOverlappedResult((HANDLE)iface, &ov, &got, FALSE)) {
            have = true;
        } else {
            CancelIoEx((HANDLE)iface, &ov);
            WaitForSingleObject(ev, 100);
        }
    } else if (ok) {
        have = true;
    }
    CloseHandle(ev);
    return have;
}

static void WinUsbReader(WINUSB_INTERFACE_HANDLE iface, UCHAR pipeId, int maxPacket) {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return;
    std::vector<uint8_t> buf(maxPacket > 0 ? maxPacket * 4 : 256);
    DWORD nextGameCheck = 0;
    while (g_run.load()) {
        DWORD now = GetTickCount();
        if (now >= nextGameCheck) {
            nextGameCheck = now + 700;
            if (FindProcessByName(L"chusanApp.exe")) {
                Log("winusb: chusanApp.exe started, releasing device");
                break;
            }
        }
        OVERLAPPED ov{};
        ov.hEvent = ev;
        ResetEvent(ev);
        ULONG rd = 0;
        BOOL ok = g_wu.ReadPipe(iface, pipeId, buf.data(), (ULONG)buf.size(), &rd, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) break;
            if (WaitForSingleObject(ev, 250) != WAIT_OBJECT_0) {
                CancelIoEx((HANDLE)iface, &ov);
                WaitForSingleObject(ev, 100);
                continue;
            }
            if (!GetOverlappedResult((HANDLE)iface, &ov, &rd, FALSE)) break;
        }
        if (rd >= 36) ApplyReport(buf.data(), rd);
    }
    CloseHandle(ev);
}

// Runs one WinUSB session. Returns false when no device was found.
static bool TryWinUsb() {
    if (!LoadWinUsb()) return false;
    // Never grab the device while the game is running: its driver owns it and a
    // second reader would silently split the input reports.
    if (FindProcessByName(L"chusanApp.exe")) return false;
    std::vector<std::string> paths;
    for (auto& p : EnumInterfacePaths(kTasollerVendorGuid)) {
        if (PathMatchesTasoller(p)) paths.push_back(p);
    }
    if (paths.empty()) paths = DiscoverViaRegistry();
    if (paths.empty()) return false;

    for (auto& path : paths) {
        HANDLE file = INVALID_HANDLE_VALUE;
        WINUSB_INTERFACE_HANDLE iface = nullptr;
        std::vector<std::pair<UCHAR, int>> pipes;
        if (!WinUsbOpen(path, file, iface, pipes)) continue;
        Log("winusb: opened %s", path.c_str());

        std::vector<std::pair<UCHAR, int>> live;
        for (auto& p : pipes) {
            ULONG got = 0;
            std::vector<uint8_t> first;
            if (TryReadOnce(iface, p.first, p.second, got, first, 400) && got >= 36) {
                Log("winusb: pipe 0x%02X carries %u bytes", p.first, (unsigned)got);
                std::string hex;
                char b[8];
                for (unsigned i = 0; i < got && i < 12; i++) { sprintf(b, "%02X ", first[i]); hex += b; }
                Log("winusb: first bytes %s", hex.c_str());
                live.push_back(p);
                ApplyReport(first.data(), got);
            } else {
                Log("winusb: pipe 0x%02X idle", p.first);
            }
        }
        if (live.empty()) {
            g_wu.Free(iface);
            CloseHandle(file);
            continue;
        }
        g_srcHid.store(4);
        SetHidName("TASOLLER PLUS (WinUSB)");
        std::vector<std::thread> readers;
        for (auto& p : live) readers.emplace_back(WinUsbReader, iface, p.first, p.second);
        for (auto& t : readers) t.join();
        g_srcHid.store(0);
        SetHidName("");
        g_wu.Free(iface);
        CloseHandle(file);
        Log("winusb: session ended");
        return true;
    }
    return false;
}
