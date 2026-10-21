// probe.cpp - inspect a WinUSB device (TASOLLER PLUS) without reverse engineering.
// Lists USB device interfaces, opens the matching one, dumps pipes and raw reports.
//
// build: zig c++ -target x86_64-windows-gnu -O2 -std=c++17 tools/probe.cpp -o build/probe.exe -lsetupapi -lole32
// run:   probe.exe --list
//        probe.exe --read --seconds=15
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <usb100.h>
#include <objbase.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

static const GUID kUsbDeviceGuid = {0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};
// TASOLLER PLUS vendor interface (WinUSB), taken from the device's DeviceInterfaceGUIDs
static const GUID kTasollerGuid = {0x1d4b2365, 0x4749, 0x48ea, {0xb3, 0x8a, 0x7c, 0x6f, 0xdd, 0xdd, 0x7e, 0x26}};

static std::atomic<bool> g_run{true};
static CRITICAL_SECTION g_logCs;
static FILE* g_log = nullptr;
static long long g_startMs = 0;

static void Log(const char* fmt, ...) {
    EnterCriticalSection(&g_logCs);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_logCs);
}

static long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Dev {
    std::string path;
};

static std::vector<Dev> ListInterfaces(const GUID& guid) {
    std::vector<Dev> out;
    HDEVINFO di = SetupDiGetClassDevsA(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) {
        Log("SetupDiGetClassDevs failed: %lu", GetLastError());
        return out;
    }
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
        Dev d;
        d.path = det->DevicePath;
        out.push_back(d);
    }
    SetupDiDestroyDeviceInfoList(di);
    return out;
}

struct Fn {
    decltype(&WinUsb_Initialize) Initialize = nullptr;
    decltype(&WinUsb_Free) Free = nullptr;
    decltype(&WinUsb_QueryInterfaceSettings) QueryInterfaceSettings = nullptr;
    decltype(&WinUsb_QueryPipe) QueryPipe = nullptr;
    decltype(&WinUsb_ReadPipe) ReadPipe = nullptr;
    decltype(&WinUsb_WritePipe) WritePipe = nullptr;
};
static Fn g_winusb;

static bool ResolveWinUsb() {
    HMODULE m = LoadLibraryA("winusb.dll");
    if (!m) return false;
    g_winusb.Initialize = (decltype(g_winusb.Initialize))GetProcAddress(m, "WinUsb_Initialize");
    g_winusb.Free = (decltype(g_winusb.Free))GetProcAddress(m, "WinUsb_Free");
    g_winusb.QueryInterfaceSettings = (decltype(g_winusb.QueryInterfaceSettings))GetProcAddress(m, "WinUsb_QueryInterfaceSettings");
    g_winusb.QueryPipe = (decltype(g_winusb.QueryPipe))GetProcAddress(m, "WinUsb_QueryPipe");
    g_winusb.ReadPipe = (decltype(g_winusb.ReadPipe))GetProcAddress(m, "WinUsb_ReadPipe");
    g_winusb.WritePipe = (decltype(g_winusb.WritePipe))GetProcAddress(m, "WinUsb_WritePipe");
    return g_winusb.Initialize && g_winusb.Free && g_winusb.QueryInterfaceSettings && g_winusb.QueryPipe && g_winusb.ReadPipe;
}

static void HexDump(const unsigned char* p, size_t n) {
    std::string s;
    char b[8];
    for (size_t i = 0; i < n && i < 48; i++) {
        sprintf(b, "%02X ", p[i]);
        s += b;
    }
    Log("    %s", s.c_str());
}

static void ReaderThread(WINUSB_INTERFACE_HANDLE iface, UCHAR pipeId, int maxPacket) {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<unsigned char> buf(maxPacket > 0 ? maxPacket * 4 : 256);
    unsigned char last[256] = {0};
    size_t lastLen = 0;
    int reports = 0, changes = 0;
    while (g_run.load()) {
        OVERLAPPED ov{};
        ov.hEvent = ev;
        ResetEvent(ev);
        ULONG read = 0;
        BOOL ok = g_winusb.ReadPipe(iface, pipeId, buf.data(), (ULONG)buf.size(), &read, &ov);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ev, 300) != WAIT_OBJECT_0) {
                    CancelIoEx((HANDLE)iface, &ov);
                    WaitForSingleObject(ev, 100);
                    continue;
                }
                if (!GetOverlappedResult((HANDLE)iface, &ov, &read, FALSE)) {
                    Log("pipe 0x%02X: overlapped read failed (%lu)", pipeId, GetLastError());
                    break;
                }
            } else {
                Log("pipe 0x%02X: read failed (%lu)", pipeId, e);
                break;
            }
        }
        if (!read) continue;
        reports++;
        bool changed = read != lastLen || memcmp(buf.data(), last, read) != 0;
        if (changed) {
            changes++;
            if (changes <= 6 || (changes % 20) == 0) {
                Log("pipe 0x%02X: report #%d len=%lu (+%lldms)", pipeId, reports, (unsigned long)read,
                    NowMs() - g_startMs);
                HexDump(buf.data(), read);
            }
            memcpy(last, buf.data(), read < sizeof(last) ? read : sizeof(last));
            lastLen = read;
        }
    }
    CloseHandle(ev);
    Log("pipe 0x%02X: done, %d reports, %d changes", pipeId, reports, changes);
}

static bool TryDevice(const std::string& path, int seconds) {
    Log("opening %s", path.c_str());
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log("  CreateFile failed: %lu", GetLastError());
        return false;
    }
    WINUSB_INTERFACE_HANDLE iface = nullptr;
    if (!g_winusb.Initialize(h, &iface)) {
        Log("  WinUsb_Initialize failed: %lu", GetLastError());
        CloseHandle(h);
        return false;
    }
    USB_INTERFACE_DESCRIPTOR desc{};
    if (!g_winusb.QueryInterfaceSettings(iface, 0, &desc)) {
        Log("  QueryInterfaceSettings failed: %lu", GetLastError());
        g_winusb.Free(iface);
        CloseHandle(h);
        return false;
    }
    Log("  interface: class=%u subclass=%u protocol=%u endpoints=%u", desc.bInterfaceClass,
        desc.bInterfaceSubClass, desc.bInterfaceProtocol, desc.bNumEndpoints);

    std::vector<std::pair<UCHAR, int>> inPipes;
    for (UCHAR i = 0; i < desc.bNumEndpoints; i++) {
        WINUSB_PIPE_INFORMATION pi{};
        if (!g_winusb.QueryPipe(iface, 0, i, &pi)) {
            Log("  QueryPipe %u failed: %lu", i, GetLastError());
            continue;
        }
        Log("  pipe 0x%02X type=%u interval=%u maxPacket=%u", pi.PipeId, pi.PipeType,
            pi.Interval, pi.MaximumPacketSize);
        if (pi.PipeId & 0x80) inPipes.push_back({pi.PipeId, pi.MaximumPacketSize});
    }
    if (inPipes.empty()) {
        Log("  no IN pipes");
        g_winusb.Free(iface);
        CloseHandle(h);
        return false;
    }

    g_startMs = NowMs();
    std::vector<std::thread> threads;
    for (auto& p : inPipes) threads.emplace_back(ReaderThread, iface, p.first, p.second);
    Log("listening %d seconds - touch / slide on the panel now", seconds);
    for (int i = 0; i < seconds * 10 && g_run.load(); i++) Sleep(100);
    g_run.store(false);
    for (auto& t : threads) t.join();
    g_winusb.Free(iface);
    CloseHandle(h);
    return true;
}


// ---- LED calibration -------------------------------------------------------
// Sends a 114-byte "DL 02" LED frame with a known colour so we can tell which
// wire byte drives which LED channel, and whether LED 0 is left or right.
static bool LedSession(const std::string& path, int secondsPerPhase) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log("led: CreateFile failed (%lu) - close the game first", GetLastError());
        return false;
    }
    WINUSB_INTERFACE_HANDLE iface = nullptr;
    if (!g_winusb.Initialize(h, &iface)) {
        Log("led: WinUsb_Initialize failed (%lu)", GetLastError());
        CloseHandle(h);
        return false;
    }
    USB_INTERFACE_DESCRIPTOR desc{};
    g_winusb.QueryInterfaceSettings(iface, 0, &desc);
    UCHAR outPipe = 0;
    for (UCHAR i = 0; i < desc.bNumEndpoints; i++) {
        WINUSB_PIPE_INFORMATION pi{};
        if (!g_winusb.QueryPipe(iface, 0, i, &pi)) continue;
        Log("led: pipe 0x%02X type=%u maxPacket=%u", pi.PipeId, pi.PipeType, pi.MaximumPacketSize);
        if (!(pi.PipeId & 0x80)) outPipe = pi.PipeId;
    }
    if (!outPipe) {
        Log("led: no OUT pipe");
        g_winusb.Free(iface);
        CloseHandle(h);
        return false;
    }
    Log("led: using OUT pipe 0x%02X", outPipe);

    auto sendFrame = [&](const uint8_t* rgb96) {
        uint8_t frame[114] = {0x44, 0x4c, 0x02};
        memcpy(frame + 3, rgb96, 96);
        ULONG written = 0;
        g_winusb.WritePipe(iface, outPipe, frame, sizeof(frame), &written, nullptr);
    };

    struct Phase { const char* name; int mode; };
    Phase phases[] = {
        {"A: byte0 = 255 (rest 0)", 0},
        {"B: byte1 = 255 (rest 0)", 1},
        {"C: byte2 = 255 (rest 0)", 2},
        {"D: all bytes 255 (white)", 3},
        {"E: only LED 0 = white", 4},
        {"F: only LED 1 = white", 5},
    };
    for (auto& ph : phases) {
        uint8_t rgb[96] = {0};
        if (ph.mode <= 2) {
            for (int i = 0; i < 32; i++) rgb[i * 3 + ph.mode] = 255;
        } else if (ph.mode == 3) {
            memset(rgb, 255, sizeof(rgb));
        } else {
            int idx = ph.mode == 4 ? 0 : 1;
            rgb[idx * 3 + 0] = rgb[idx * 3 + 1] = rgb[idx * 3 + 2] = 255;
        }
        Log("led: phase %s for %d s", ph.name, secondsPerPhase);
        DWORD end = GetTickCount() + secondsPerPhase * 1000;
        while (GetTickCount() < end) {
            sendFrame(rgb);
            Sleep(16);
        }
    }
    uint8_t off[96] = {0};
    sendFrame(off);
    g_winusb.Free(iface);
    CloseHandle(h);
    Log("led: done");
    return true;
}

int main(int argc, char** argv) {
    InitializeCriticalSection(&g_logCs);
    g_log = fopen("probe.log", "w");

    bool listOnly = false, readMode = false, ledMode = false;
    int seconds = 15;
    std::string filter = "vid_0e8f";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--list") listOnly = true;
        else if (a == "--read") readMode = true;
        else if (a == "--led-test") ledMode = true;
        else if (a.rfind("--seconds=", 0) == 0) seconds = atoi(a.c_str() + 10);
        else if (a.rfind("--filter=", 0) == 0) filter = a.c_str() + 9;
    }
    if (!listOnly && !readMode && !ledMode) {
        printf("usage: probe.exe --list | --read [--seconds=N] [--filter=vid_0e8f]\n");
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto devs = ListInterfaces(kUsbDeviceGuid);
    auto vendor = ListInterfaces(kTasollerGuid);
    devs.insert(devs.end(), vendor.begin(), vendor.end());
    Log("found %zu interfaces (%zu tasoller-vendor)", devs.size(), vendor.size());

    std::string lowFilter = filter;
    for (auto& c : lowFilter) c = (char)tolower((unsigned char)c);

    std::vector<Dev> matches;
    for (auto& d : devs) {
        std::string low = d.path;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (listOnly) Log("%s", d.path.c_str());
        if (low.find(lowFilter) != std::string::npos) matches.push_back(d);
    }

    if (listOnly) {
        Log("matches: %zu", matches.size());
        fclose(g_log);
        return 0;
    }
    if (matches.empty()) {
        Log("no device matching '%s' - is TASOLLER connected in TASOLLER mode?", lowFilter.c_str());
        fclose(g_log);
        return 2;
    }
    if (!ResolveWinUsb()) {
        Log("winusb.dll not available");
        fclose(g_log);
        return 3;
    }
    if (ledMode) {
        for (auto& d : matches) {
            if (LedSession(d.path, 3)) break;
        }
        fclose(g_log);
        return 0;
    }
    for (auto& d : matches) {
        if (TryDevice(d.path, seconds)) break;
    }
    fclose(g_log);
    return 0;
}
