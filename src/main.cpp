// chunithm-handviewer
// Standalone CHUNITHM input viewer for keyboard + TASOLLER PLUS.
// C++17, Win32, no external dependencies. Serves a local web overlay.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <shellapi.h>
#include <mmsystem.h>

#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------- logging ---
static std::mutex g_logMu;
static std::wstring g_exeDir;
static std::wstring g_logPath;

static void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMu);
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath.c_str(), L"a") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fclose(f);
    }
}

// -------------------------------------------------------------- utilities ---
static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring p(buf, n);
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static bool ParseInt(const std::string& s, long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 0);
    if (end == s.c_str()) return false;
    out = v;
    return true;
}

// ------------------------------------------------------------------- sha1 ---
struct Sha1 {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t buflen;
    Sha1() { Reset(); }
    void Reset() {
        h[0] = 0x67452301; h[1] = 0xEFCDAB89; h[2] = 0x98BADCFE; h[3] = 0x10325476; h[4] = 0xC3D2E1F0;
        len = 0; buflen = 0;
    }
    static uint32_t Rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    void Block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    void Update(const void* data, size_t n) {
        const uint8_t* p = (const uint8_t*)data;
        len += n;
        while (n) {
            size_t take = std::min(n, 64 - buflen);
            memcpy(buf + buflen, p, take);
            buflen += take; p += take; n -= take;
            if (buflen == 64) { Block(buf); buflen = 0; }
        }
    }
    void Final(uint8_t out[20]) {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        Update(&pad, 1);
        uint8_t zero = 0;
        while (buflen != 56) Update(&zero, 1);
        uint8_t lb[8];
        for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - i * 8));
        Update(lb, 8);
        for (int i = 0; i < 5; i++) {
            out[i * 4] = (uint8_t)(h[i] >> 24);
            out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
            out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
            out[i * 4 + 3] = (uint8_t)h[i];
        }
    }
};

static std::string Base64(const uint8_t* d, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string r;
    r.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < n) v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < n) v |= d[i + 2];
        r += t[(v >> 18) & 63];
        r += t[(v >> 12) & 63];
        r += (i + 1 < n) ? t[(v >> 6) & 63] : '=';
        r += (i + 2 < n) ? t[v & 63] : '=';
    }
    return r;
}

// ----------------------------------------------------------------- config ---
struct Config {
    int port = 38085;
    bool openBrowser = true;
    bool keyboard = true;
    bool hid = true;
    bool probe = true;
    int vid = 0x1CCF;
    int pid = 0x2333;
    int threshold = 20;
    std::string theme = "arcade";
    std::string cells = "SSSSDDDDFFFFGGGGHHHHJJJJKKKKLLLL";
    std::string airKeys = "";
    std::string fnKeys = "";
};

static Config g_cfg;
static std::wstring g_iniPath;
static bool g_cliPort = false;
static int g_iniPort = 38085;

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// Virtual-key token: "S" | "0x53" | "83" | "SPACE" | "LEFT"
static int ParseVk(const std::string& tok) {
    if (tok.empty()) return 0;
    static const std::map<std::string, int> named = {
        {"SPACE", VK_SPACE}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"UP", VK_UP}, {"DOWN", VK_DOWN},
        {"SHIFT", VK_SHIFT}, {"CTRL", VK_CONTROL}, {"ALT", VK_MENU}, {"ENTER", VK_RETURN},
        {"TAB", VK_TAB}, {"ESC", VK_ESCAPE}, {"BACKSPACE", VK_BACK}, {"SEMI", VK_OEM_1},
        {"COMMA", VK_OEM_COMMA}, {"PERIOD", VK_OEM_PERIOD}, {"SLASH", VK_OEM_2},
        {"LBRACKET", VK_OEM_4}, {"RBRACKET", VK_OEM_6}, {"BACKSLASH", VK_OEM_5},
        {"MINUS", VK_OEM_MINUS}, {"EQUALS", VK_OEM_PLUS}, {"TILDE", VK_OEM_3},
    };
    std::string up = tok;
    for (auto& c : up) c = (char)toupper((unsigned char)c);
    auto it = named.find(up);
    if (it != named.end()) return it->second;
    if (up.size() == 1) {
        char c = up[0];
        if (c >= 'A' && c <= 'Z') return c;
        if (c >= '0' && c <= '9') return c;
    }
    long v;
    if (ParseInt(up, v) && v > 0 && v < 256) return (int)v;
    if (up.size() >= 2 && up[0] == 'F') {
        long f;
        if (ParseInt(up.substr(1), f) && f >= 1 && f <= 24) return VK_F1 + (int)f - 1;
    }
    return 0;
}

// "SSSS DDDD ..." or "SSSSDDDD..." -> list of vk. digits: "123456" means keys 1..6
static std::vector<int> ParseVkList(const std::string& s, size_t expect) {
    std::vector<int> out;
    std::vector<std::string> toks;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '	' || c == ',' || c == '-' || c == '_') {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) toks.push_back(cur);
    (void)expect;

    auto expand = [&](const std::string& t) {
        if (t.size() > 1 && (t[0] == 'F' || t[0] == 'f')) {
            long f = 0;
            if (ParseInt(t.substr(1), f) && f >= 1 && f <= 24) {
                out.push_back(VK_F1 + (int)f - 1);
                return;
            }
        }
        bool allAlnum = t.size() > 1;
        for (char c : t) if (!isalnum((unsigned char)c)) allAlnum = false;
        if (allAlnum) {
            for (char c : t) {
                int vk = ParseVk(std::string(1, c));
                if (vk) out.push_back(vk);
            }
            return;
        }
        int vk = ParseVk(t);
        if (vk) out.push_back(vk);
    };
    for (auto& t : toks) expand(t);
    return out;
}

static std::string FormatVkList(const std::vector<int>& vks, char sep = ' ') {
    std::string out;
    bool first = true;
    for (int vk : vks) {
        if (!vk) continue;
        if (!first) out += sep;
        first = false;
        if (vk >= 'A' && vk <= 'Z') out += (char)vk;
        else if (vk >= '0' && vk <= '9') out += (char)vk;
        else {
            char b[16];
            sprintf(b, "0x%02X", vk);
            out += b;
        }
    }
    return out;
}

static void LoadConfig() {
    g_iniPath = g_exeDir + L"\\handviewer.ini";
    std::ifstream f(g_iniPath);
    if (!f) {
        Log("config not found, using defaults");
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        std::string s = Trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#' || s[0] == '[') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Lower(Trim(s.substr(0, eq)));
        std::string v = Trim(s.substr(eq + 1));
        if (v.find(';') != std::string::npos) v = Trim(v.substr(0, v.find(';')));
        long n;
        if (k == "port" && ParseInt(v, n)) { g_cfg.port = (int)n; g_iniPort = (int)n; }
        else if (k == "open_browser") g_cfg.openBrowser = (v != "0");
        else if (k == "keyboard") g_cfg.keyboard = (v != "0");
        else if (k == "hid") g_cfg.hid = (v != "0");
        else if (k == "probe") g_cfg.probe = (v != "0");
        else if (k == "vid" && ParseInt(v, n)) g_cfg.vid = (int)n;
        else if (k == "pid" && ParseInt(v, n)) g_cfg.pid = (int)n;
        else if (k == "threshold" && ParseInt(v, n)) g_cfg.threshold = (int)n;
        else if (k == "theme") g_cfg.theme = v;
        else if (k == "cells") g_cfg.cells = v;
        else if (k == "air_keys") g_cfg.airKeys = v;
        else if (k == "fn_keys") g_cfg.fnKeys = v;
    }
}

static void SaveConfig() {
    std::ofstream f(g_iniPath);
    if (!f) { Log("cannot write config"); return; }
    f << "; chunithm-handviewer\n";
    f << "[general]\n";
    f << "port=" << (g_cliPort ? g_iniPort : g_cfg.port) << "\n";
    f << "open_browser=" << (g_cfg.openBrowser ? 1 : 0) << "\n";
    f << "theme=" << g_cfg.theme << "\n";
    f << "\n[input]\n";
    f << "keyboard=" << (g_cfg.keyboard ? 1 : 0) << "\n";
    f << "hid=" << (g_cfg.hid ? 1 : 0) << "\n";
    f << "probe=" << (g_cfg.probe ? 1 : 0) << "\n";
    f << "vid=0x" << std::hex << g_cfg.vid << std::dec << "\n";
    f << "pid=0x" << std::hex << g_cfg.pid << std::dec << "\n";
    f << "threshold=" << g_cfg.threshold << "\n";
    f << "\n[keys]\n";
    f << "; cells: left -> right, 32 cells (default segatools mapping)\n";
    f << "cells=" << g_cfg.cells << "\n";
    f << "air_keys=" << g_cfg.airKeys << "\n";
    f << "fn_keys=" << g_cfg.fnKeys << "\n";
}

// ------------------------------------------------------------------ input ---
static std::atomic<uint8_t> g_hidP[32];
// Last LED frame the driver sent to the panel (0xRRGGBB), so the viewer can
// mirror the real slider colours instead of a fixed palette.
static std::atomic<uint32_t> g_led[32];
static std::atomic<bool> g_ledSeen{false};
static std::atomic<bool> g_air[6];
static std::atomic<bool> g_hidFn[2];
static std::atomic<bool> g_kbCell[32];
static std::atomic<bool> g_kbAir[6];
static std::atomic<bool> g_kbFn[2];
static std::atomic<int> g_srcHid{0};  // 0 none, 1 hid, 2 shmem, 3 demo
static std::atomic<bool> g_kbAlive{false};
static std::mutex g_hidDevMu;
static std::string g_hidDevName;

static std::vector<int> g_vkCell;  // 32
static std::vector<int> g_vkAir;   // 6
static std::vector<int> g_vkFn;    // 2
static std::mutex g_vkMu;
static std::atomic<int> g_keyGen{0};

static std::atomic<bool> g_run{true};
static HANDLE g_quitEvent = nullptr;

static void SetHidName(const std::string& n) {
    std::lock_guard<std::mutex> lk(g_hidDevMu);
    g_hidDevName = n;
}
static std::string GetHidName() {
    std::lock_guard<std::mutex> lk(g_hidDevMu);
    return g_hidDevName;
}

static inline uint8_t Reverse8(uint8_t v) {
    v = (uint8_t)((v >> 4) | (v << 4));
    v = (uint8_t)(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
    v = (uint8_t)(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
    return v;
}

static void ApplyReport(const uint8_t* b, size_t n) {
    if (n < 36) return;
    int off = -1;
    bool official = false;
    for (int i = 0; i + 36 <= (int)n && i <= 2; i++) {
        if (b[i] == 0x68 && b[i + 1] == 0x66 && b[i + 2] == 0x84) { off = i; official = false; break; }
        if (b[i] == 0x44 && b[i + 1] == 0x42 && b[i + 2] == 0x54) { off = i; official = true; break; }
    }
    if (off < 0) return;
    uint8_t status = b[off + 3];
    if (official) {
        // Stock TASOLLER PLUS firmware sends "DBT" + a bit-reversed status byte.
        // Reversing it matches the segatools tasoller_v2 driver:
        //   bit0 -> TEST, bit1 -> SERVICE, bits2..7 -> beam 1..6
        uint8_t r = Reverse8(status);
        for (int i = 0; i < 6; i++) g_air[i].store((r >> (2 + i)) & 1, std::memory_order_relaxed);
        g_hidFn[0].store(r & 1, std::memory_order_relaxed);
        g_hidFn[1].store((r >> 1) & 1, std::memory_order_relaxed);
    } else {
        for (int i = 0; i < 6; i++) g_air[i].store((status >> i) & 1, std::memory_order_relaxed);
        g_hidFn[0].store((status >> 6) & 1, std::memory_order_relaxed);
        g_hidFn[1].store((status >> 7) & 1, std::memory_order_relaxed);
    }
    // The panel reports its 32 sensors right-to-left, and two adjacent sensors
    // are the upper/lower halves of one physical key. Flip by key so the
    // viewer reads left-to-right while keeping each pair together.
    for (int k = 0; k < 16; k++) {
        g_hidP[2 * k].store(b[off + 4 + 2 * (15 - k)], std::memory_order_relaxed);
        g_hidP[2 * k + 1].store(b[off + 4 + 2 * (15 - k) + 1], std::memory_order_relaxed);
    }
}
// "DL" + 0x02, then 32 x {G,B,R}; the driver writes this packet to pipe 0x03.
static void ApplyLed(const uint8_t* p, size_t n) {
    if (n < 99) return;
    if (p[0] != 0x44 || p[1] != 0x4c || p[2] != 0x02) return;
    bool any = false;
    for (int i = 0; i < 32; i++) {
        // LED frames are right-to-left like the touch sensors, and the driver
        // writes {G,R,B} from a BGR source; map both back to display order.
        int src = i;  // LED frames already come left-to-right
        uint8_t b0 = p[3 + src * 3 + 0];
        uint8_t b1 = p[3 + src * 3 + 1];
        uint8_t b2 = p[3 + src * 3 + 2];
        // Keep the wire order; the page maps bytes -> R/G/B (configurable).
        g_led[i].store(((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | b2, std::memory_order_relaxed);
        if (b0 || b1 || b2) any = true;
    }
    g_ledSeen.store(any);
}

// --- keyboard polling ---
static void KeyboardThread() {
    // initialise atomics
    for (int i = 0; i < 32; i++) g_kbCell[i].store(false);
    g_kbAlive.store(true);
    std::vector<int> cell, air, fn;
    int seenGen = -1;
    uint8_t lastCell[32] = {0};
    bool lastAir[6] = {0};
    bool lastFn[2] = {0};
    while (g_run.load()) {
        if (seenGen != g_keyGen.load()) {
            std::lock_guard<std::mutex> lk(g_vkMu);
            cell = g_vkCell; air = g_vkAir; fn = g_vkFn;
            cell.resize(32, 0); air.resize(6, 0); fn.resize(2, 0);
            seenGen = g_keyGen.load();
            for (int i = 0; i < 32; i++) { lastCell[i] = 0; g_kbCell[i].store(false); }
            for (int i = 0; i < 6; i++) { lastAir[i] = 0; g_kbAir[i].store(false); }
            for (int i = 0; i < 2; i++) { lastFn[i] = 0; g_kbFn[i].store(false); }
        }
        for (int i = 0; i < 32; i++) {
            bool down = cell[i] && (GetAsyncKeyState(cell[i]) & 0x8000) != 0;
            if (down != (bool)lastCell[i]) { lastCell[i] = down; g_kbCell[i].store(down, std::memory_order_relaxed); }
        }
        for (int i = 0; i < 6; i++) {
            bool down = air[i] && (GetAsyncKeyState(air[i]) & 0x8000) != 0;
            if (down != lastAir[i]) { lastAir[i] = down; g_kbAir[i].store(down, std::memory_order_relaxed); }
        }
        for (int i = 0; i < 2; i++) {
            bool down = fn[i] && (GetAsyncKeyState(fn[i]) & 0x8000) != 0;
            if (down != lastFn[i]) { lastFn[i] = down; g_kbFn[i].store(down, std::memory_order_relaxed); }
        }
        Sleep(1);
    }
}

// --- HID ---
static std::string NarrowPath(const wchar_t* p) { return WideToUtf8(p); }

struct HidDev {
    HANDLE h = INVALID_HANDLE_VALUE;
    USHORT inLen = 0;
    std::string path;
};
static bool ProbeMagic(HANDLE h, USHORT inLen) {
    if (inLen < 36) return false;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    std::vector<uint8_t> buf(inLen);
    DWORD rd = 0;
    BOOL ok = ReadFile(h, buf.data(), inLen, &rd, &ov);
    bool magic = false;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 150) == WAIT_OBJECT_0) {
            if (GetOverlappedResult(h, &ov, &rd, FALSE)) {
                for (int i = 0; i + 3 <= (int)rd && i <= 2; i++)
                    if (buf[i] == 0x68 && buf[i + 1] == 0x66 && buf[i + 2] == 0x84) magic = true;
            }
        } else {
            CancelIoEx(h, &ov);
            WaitForSingleObject(ov.hEvent, 100);
        }
    } else if (ok) {
        for (int i = 0; i + 3 <= (int)rd && i <= 2; i++)
            if (buf[i] == 0x68 && buf[i + 1] == 0x66 && buf[i + 2] == 0x84) magic = true;
    }
    CloseHandle(ov.hEvent);
    return magic;
}

static const GUID kHidInterfaceGuid = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};

static bool OpenFirstTasoller(HidDev& out, bool probe) {
    const GUID& guid = kHidInterfaceGuid;
    HDEVINFO di = SetupDiGetClassDevsA(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return false;
    std::vector<HidDev> candidates;
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
        HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at{};
        at.Size = sizeof(at);
        HIDP_CAPS caps{};
        PHIDP_PREPARSED_DATA pp = nullptr;
        bool got = HidD_GetAttributes(h, &at) && HidD_GetPreparsedData(h, &pp) && HidP_GetCaps(pp, &caps);
        if (pp) HidD_FreePreparsedData(pp);
        if (!got) { CloseHandle(h); continue; }
        HidDev d;
        d.h = h;
        d.inLen = caps.InputReportByteLength;
        d.path = det->DevicePath;
        if (d.inLen < 36) { CloseHandle(h); continue; }
        bool direct = (at.VendorID == g_cfg.vid && at.ProductID == g_cfg.pid);
        if (direct) {
            Log("hid: candidate %04X:%04X report=%u", at.VendorID, at.ProductID, (unsigned)d.inLen);
            candidates.insert(candidates.begin(), d);
        } else if (probe) {
            if (ProbeMagic(h, d.inLen)) {
                Log("hid: magic probe matched %04X:%04X report=%u", at.VendorID, at.ProductID, (unsigned)d.inLen);
                candidates.push_back(d);
            } else {
                CloseHandle(h);
            }
        } else {
            CloseHandle(h);
        }
    }
    SetupDiDestroyDeviceInfoList(di);
    if (candidates.empty()) return false;
    out = candidates.front();
    for (size_t i = 1; i < candidates.size(); i++) CloseHandle(candidates[i].h);
    return true;
}

static bool ShmemExists() {
    HANDLE hm = OpenFileMappingA(FILE_MAP_READ, FALSE, "tasoller_input");
    if (!hm) return false;
    CloseHandle(hm);
    return true;
}

// returns true when it wants to switch to the shared-memory source
static bool ReadHidLoop(HANDLE h, USHORT inLen) {
    std::vector<uint8_t> buf(inLen ? inLen : 64);
    int reports = 0, badReports = 0;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    while (g_run.load()) {
        ResetEvent(ov.hEvent);
        ov.Offset = 0;
        ov.OffsetHigh = 0;
        DWORD rd = 0;
        BOOL ok = ReadFile(h, buf.data(), (DWORD)buf.size(), &rd, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            DWORD w = WaitForSingleObject(ov.hEvent, 250);
            if (w == WAIT_TIMEOUT) {
                CancelIoEx(h, &ov);
                WaitForSingleObject(ov.hEvent, 100);
                if (ShmemExists()) { CloseHandle(ov.hEvent); return true; }
                continue;
            }
            if (!GetOverlappedResult(h, &ov, &rd, FALSE)) { CloseHandle(ov.hEvent); return false; }
        } else if (!ok) {
            CloseHandle(ov.hEvent);
            return false;
        }
        if (rd >= 36) {
            bool magic = (buf[0] == 0x68 && buf[1] == 0x66 && buf[2] == 0x84);
            if (reports == 0) {
                Log("hid: first report len=%u magic=%s bytes=%02X %02X %02X %02X %02X %02X", (unsigned)rd,
                    magic ? "yes" : "no", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
            }
            reports++;
            if (magic) badReports = 0;
            else if (++badReports > 2000) {
                Log("input: HID reports do not carry the Tasoller magic, closing");
                CloseHandle(ov.hEvent);
                return false;
            }
            ApplyReport(buf.data(), rd);
        }
    }
    CloseHandle(ov.hEvent);
    return false;
}

static bool TryShmem() {
    HANDLE hm = OpenFileMappingA(FILE_MAP_READ, FALSE, "tasoller_input");
    if (!hm) return false;
    void* p = MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 36);
    if (!p) { CloseHandle(hm); return false; }
    g_srcHid.store(2);
    SetHidName("tasoller_input (shared memory)");
    Log("input: reading tasoller_input shared memory");
    int idle = 0;
    while (g_run.load()) {
        uint8_t tmp[36];
        memcpy(tmp, p, 36);
        bool ok = tmp[0] == 0x68 && tmp[1] == 0x66 && tmp[2] == 0x84;
        if (ok) idle = 0;
        else if (++idle > 3000) {
            Log("input: shared memory present but has no expected report, falling back");
            break;
        }
        ApplyReport(tmp, 36);
        Sleep(1);
    }
    UnmapViewOfFile(p);
    CloseHandle(hm);
    return true;
}

#include "gameinput.h"
#include "winusb_input.h"

static void HidThread() {
    g_srcHid.store(0);
    while (g_run.load()) {
        if (TryGameInput()) continue;
        if (TryShmem()) continue;
        if (TryWinUsb()) continue;
        HidDev d;
        if (OpenFirstTasoller(d, g_cfg.probe)) {
            SetHidName("1CCF:2333 / HID");
            Log("input: HID opened, report length %u", (unsigned)d.inLen);
            g_srcHid.store(1);
            bool switchToShmem = ReadHidLoop(d.h, d.inLen);
            CloseHandle(d.h);
            g_srcHid.store(0);
            SetHidName("");
            Log(switchToShmem ? "input: released HID for shared memory" : "input: HID closed");
        }
        Sleep(800);
    }
}


static void DemoThread() {
    double t = 0;
    g_srcHid.store(3);
    SetHidName("demo");
    while (g_run.load()) {
        t += 0.05;
        for (int i = 0; i < 32; i++) {
            double phase = sin((t + i * 0.22)) * 0.5 + 0.5;
            int v = (int)(phase * 255);
            if (fmod(t, 6.0) > 3.0 && i >= 10 && i <= 17) v = 0;
            g_hidP[i].store((uint8_t)v);
        }
        for (int i = 0; i < 6; i++) g_air[i].store(((int)(t * 2) % 6) == i);
        Sleep(16);
    }
}

// ------------------------------------------------------------------ state ---
struct Snapshot {
    uint8_t cells[32];
    bool air[6];
    bool fn[2];
    int src;
    bool kb;
    bool ledValid;
    uint32_t led[32];
    std::string dev;
};

static Snapshot TakeSnapshot() {
    Snapshot s{};
    for (int i = 0; i < 32; i++) {
        s.cells[i] = g_hidP[i].load(std::memory_order_relaxed);
    }
    for (int i = 0; i < 6; i++) s.air[i] = g_air[i].load();
    for (int i = 0; i < 2; i++) s.fn[i] = g_hidFn[i].load();
    s.src = g_srcHid.load();
    s.kb = false;
    s.ledValid = g_ledSeen.load();
    for (int i = 0; i < 32; i++) s.led[i] = g_led[i].load(std::memory_order_relaxed);
    s.dev = GetHidName();
    return s;
}

static std::string JsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; sprintf(b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

static std::string SnapshotJson(const Snapshot& s, bool withKeys) {
    std::string j = "{\"v\":1,\"src\":";
    j += std::to_string(s.src);
    j += ",\"kb\":";
    j += s.kb ? "1" : "0";
    j += ",\"dev\":\"" + JsonEscape(s.dev) + "\",\"cells\":[";
    for (int i = 0; i < 32; i++) { if (i) j += ','; j += std::to_string(s.cells[i]); }
    j += "],\"air\":[";
    for (int i = 0; i < 6; i++) { if (i) j += ','; j += s.air[i] ? "1" : "0"; }
    j += "],\"fn\":[";
    for (int i = 0; i < 2; i++) { if (i) j += ','; j += s.fn[i] ? "1" : "0"; }
    j += "],\"ledValid\":";
    j += s.ledValid ? "1" : "0";
    j += ",\"led\":[";
    for (int i = 0; i < 32; i++) { if (i) j += ','; j += std::to_string(s.led[i]); }
    j += "]";
    if (withKeys) {
        j += ",\"keys\":{\"cells\":[";
        {
            std::lock_guard<std::mutex> lk(g_vkMu);
            for (size_t i = 0; i < g_vkCell.size(); i++) { if (i) j += ','; j += std::to_string(g_vkCell[i]); }
            j += "],\"air\":[";
            for (size_t i = 0; i < g_vkAir.size(); i++) { if (i) j += ','; j += std::to_string(g_vkAir[i]); }
            j += "],\"fn\":[";
            for (size_t i = 0; i < g_vkFn.size(); i++) { if (i) j += ','; j += std::to_string(g_vkFn[i]); }
            j += "]}";
        }
    }
    j += "}";
    return j;
}

// ----------------------------------------------------------------- server ---
struct WsClient {
    SOCKET s = INVALID_SOCKET;
    std::mutex w;
    std::atomic<bool> alive{true};
};

static std::mutex g_clientsMu;
static std::vector<std::shared_ptr<WsClient>> g_clients;
static SOCKET g_listen = INVALID_SOCKET;

static void SendAll(SOCKET s, const char* data, size_t n) {
    size_t off = 0;
    while (off < n) {
        int r = send(s, data + off, (int)(n - off), 0);
        if (r <= 0) return;
        off += (size_t)r;
    }
}

static std::string UrlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int a = hex(s[i + 1]), b = hex(s[i + 2]);
            if (a >= 0 && b >= 0) { o += (char)(a * 16 + b); i += 2; continue; }
        }
        if (s[i] == '+') o += ' ';
        else o += s[i];
    }
    return o;
}

static std::map<std::string, std::string> ParseQuery(const std::string& q) {
    std::map<std::string, std::string> m;
    size_t i = 0;
    while (i < q.size()) {
        size_t amp = q.find('&', i);
        std::string kv = q.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) m[UrlDecode(kv.substr(0, eq))] = UrlDecode(kv.substr(eq + 1));
        i = (amp == std::string::npos) ? q.size() : amp + 1;
    }
    return m;
}

static bool ReadFileBytes(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static const char* MimeFor(const std::string& path) {
    auto ends = [&](const char* e) {
        size_t n = strlen(e);
        return path.size() >= n && _stricmp(path.c_str() + path.size() - n, e) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".css")) return "text/css; charset=utf-8";
    if (ends(".js")) return "application/javascript; charset=utf-8";
    if (ends(".json")) return "application/json; charset=utf-8";
    if (ends(".png")) return "image/png";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".ico")) return "image/x-icon";
    if (ends(".woff2")) return "font/woff2";
    return "application/octet-stream";
}

static void HttpError(SOCKET c, int code, const char* msg) {
    char body[256];
    sprintf(body, "{\"error\":\"%s\"}", msg);
    char hdr[512];
    sprintf(hdr,
            "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
            code, code == 404 ? "Not Found" : "Error", (int)strlen(body));
    SendAll(c, hdr, strlen(hdr));
    SendAll(c, body, strlen(body));
}

static void ServeStatic(SOCKET c, const std::string& urlPath) {
    std::string rel = urlPath == "/" ? "/index.html" : urlPath;
    if (rel.find("..") != std::string::npos) { HttpError(c, 404, "not found"); return; }
    std::string body;
    if (!ReadFileBytes(g_exeDir + L"\\web" + Utf8ToWide(rel), body)) {
        // fall back to <exe>/web built-in? no, report clearly
        HttpError(c, 404, "not found");
        return;
    }
    char hdr[512];
    sprintf(hdr,
            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
            "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
            MimeFor(rel), (int)body.size());
    SendAll(c, hdr, strlen(hdr));
    SendAll(c, body.data(), body.size());
}

static void HandleWs(SOCKET c, const std::string& key) {
    std::string src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[20];
    Sha1 sh;
    sh.Update(src.data(), src.size());
    sh.Final(digest);
    std::string accept = Base64(digest, 20);
    std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
    if (send(c, resp.data(), (int)resp.size(), 0) <= 0) return;

    // The HTTP handler left a 5 s receive timeout on this socket; websocket
    // clients stay silent until they close, so drop that timeout or every
    // connection would be torn down after 5 seconds.
    DWORD noTimeout = 0;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&noTimeout, sizeof(noTimeout));

    auto cl = std::make_shared<WsClient>();
    cl->s = c;
    {
        std::lock_guard<std::mutex> lk(g_clientsMu);
        g_clients.push_back(cl);
    }
    // push one immediate frame
    auto snap = TakeSnapshot();
    std::string js = SnapshotJson(snap, true);
    std::string frame;
    frame += (char)0x81;
    if (js.size() < 126) frame += (char)js.size();
    else { frame += (char)126; frame += (char)((js.size() >> 8) & 0xFF); frame += (char)(js.size() & 0xFF); }
    frame += js;
    { std::lock_guard<std::mutex> lk(cl->w); SendAll(c, frame.data(), frame.size()); }

    char buf[256];
    while (g_run.load() && cl->alive.load()) {
        int r = recv(c, buf, sizeof(buf), 0);
        if (r <= 0) break;
    }
    cl->alive.store(false);
    {
        std::lock_guard<std::mutex> lk(g_clientsMu);
        g_clients.erase(std::remove(g_clients.begin(), g_clients.end(), cl), g_clients.end());
    }
    closesocket(c);
}

static void HandleConn(SOCKET c) {
    DWORD tv = 5000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    DWORD sv = 200;
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sv, sizeof(sv));

    std::string req;
    char buf[2048];
    while (req.size() < 16384) {
        int r = recv(c, buf, sizeof(buf), 0);
        if (r <= 0) break;
        req.append(buf, r);
        if (req.find("\r\n\r\n") != std::string::npos) break;
    }
    if (req.empty()) { closesocket(c); return; }
    size_t lineEnd = req.find("\r\n");
    std::string line = req.substr(0, lineEnd);
    size_t sp1 = line.find(' ');
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) { closesocket(c); return; }
    std::string method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string path = target, query;
    size_t qm = target.find('?');
    if (qm != std::string::npos) { path = target.substr(0, qm); query = target.substr(qm + 1); }

    std::string key;
    {
        std::string lowerReq = Lower(req);
        size_t kp = lowerReq.find("\r\nsec-websocket-key:");
        if (kp != std::string::npos) {
            size_t vs = kp + strlen("\r\nsec-websocket-key:");
            size_t ve = req.find("\r\n", vs);
            key = Trim(req.substr(vs, ve - vs));
        }
    }

    if (path == "/ws" && !key.empty()) { HandleWs(c, key); return; }

    if (path == "/state") {
        auto snap = TakeSnapshot();
        std::string js = SnapshotJson(snap, true);
        char hdr[512];
        sprintf(hdr,
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n"
                "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                (int)js.size());
        SendAll(c, hdr, strlen(hdr));
        SendAll(c, js.data(), js.size());
        closesocket(c);
        return;
    }
    if (path == "/quit") {
        const char* body = "{\"ok\":true}";
        char hdr[256];
        sprintf(hdr, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", (int)strlen(body));
        SendAll(c, hdr, strlen(hdr));
        SendAll(c, body, strlen(body));
        closesocket(c);
        Log("quit requested via http");
        SetEvent(g_quitEvent);
        return;
    }
    if (path == "/bind") {
        auto q = ParseQuery(query);
        std::string slot = q["slot"];
        long vk = 0;
        bool ok = ParseInt(q["vk"], vk) && vk > 0 && vk < 256;
        int idx = -1, kind = 0;
        if (ok) {
            if (slot.rfind("cell", 0) == 0) { kind = 0; idx = atoi(slot.c_str() + 4); }
            else if (slot.rfind("air", 0) == 0) { kind = 1; idx = atoi(slot.c_str() + 3); }
            else if (slot.rfind("fn", 0) == 0) { kind = 2; idx = atoi(slot.c_str() + 2); }
            ok = idx >= 0 && idx < (kind == 0 ? 32 : kind == 1 ? 6 : 2) && !q["vk"].empty();
        }
        if (ok) {
            {
                std::lock_guard<std::mutex> lk(g_vkMu);
                if (kind == 0) g_vkCell[idx] = (int)vk;
                else if (kind == 1) g_vkAir[idx] = (int)vk;
                else g_vkFn[idx] = (int)vk;
                g_cfg.cells = FormatVkList(g_vkCell, ' ');
                g_cfg.airKeys = FormatVkList(g_vkAir, ' ');
                g_cfg.fnKeys = FormatVkList(g_vkFn, ' ');
                g_keyGen.fetch_add(1);
            }
            SaveConfig();
        }
        const char* body = ok ? "{\"ok\":true}" : "{\"ok\":false}";
        char hdr[256];
        sprintf(hdr, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", (int)strlen(body));
        SendAll(c, hdr, strlen(hdr));
        SendAll(c, body, strlen(body));
        closesocket(c);
        return;
    }

    if (method != "GET") { HttpError(c, 404, "not found"); closesocket(c); return; }
    ServeStatic(c, path);
    // ServeStatic closes? no
    closesocket(c);
}

static void AcceptThread() {
    while (g_run.load()) {
        sockaddr_in a{};
        int alen = sizeof(a);
        SOCKET c = accept(g_listen, (sockaddr*)&a, &alen);
        if (c == INVALID_SOCKET) {
            if (!g_run.load()) break;
            Sleep(20);
            continue;
        }
        std::thread(HandleConn, c).detach();
    }
}

static void BroadcastThread() {
    while (g_run.load()) {
        auto snap = TakeSnapshot();
        std::string js = SnapshotJson(snap, false);
        std::string frame;
        frame += (char)0x81;
        if (js.size() < 126) frame += (char)js.size();
        else if (js.size() < 65536) {
            frame += (char)126;
            frame += (char)((js.size() >> 8) & 0xFF);
            frame += (char)(js.size() & 0xFF);
        } else {
            frame += (char)127;
            for (int i = 7; i >= 0; i--) frame += (char)((uint64_t)js.size() >> (i * 8));
        }
        frame += js;

        std::vector<std::shared_ptr<WsClient>> alive;
        {
            std::lock_guard<std::mutex> lk(g_clientsMu);
            alive = g_clients;
        }
        for (auto& cl : alive) {
            if (!cl->alive.load()) continue;
            std::lock_guard<std::mutex> lk(cl->w);
            if (send(cl->s, frame.data(), (int)frame.size(), 0) <= 0) cl->alive.store(false);
        }
        Sleep(8);
    }
}

static bool StartServer() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) return false;
    BOOL yes = TRUE;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_cfg.port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(g_listen, (sockaddr*)&addr, sizeof(addr)) != 0) return false;
    if (listen(g_listen, 16) != 0) return false;
    std::thread(AcceptThread).detach();
    std::thread(BroadcastThread).detach();
    return true;
}

// ---------------------------------------------------------------- browser ---
static std::wstring FindChromium() {
    const wchar_t* names[] = {L"\\Microsoft\\Edge\\Application\\msedge.exe", L"\\Google\\Chrome\\Application\\chrome.exe"};
    const wchar_t* roots[] = {L"ProgramFiles(x86)", L"ProgramFiles", L"LOCALAPPDATA"};
    for (auto root : roots) {
        wchar_t base[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(root, base, MAX_PATH * 2);
        if (!n) continue;
        for (auto nm : names) {
            std::wstring p = std::wstring(base) + nm;
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        }
    }
    return L"";
}

static bool LaunchViewer(const std::wstring& url) {
    std::wstring exe = FindChromium();
    if (!exe.empty()) {
        // A dedicated profile keeps these flags effective even when the user already has
        // Edge/Chrome running, and the throttling flags stop Chromium from freezing the
        // renderer when the game window covers the viewer.
        std::wstring cmd = L"\"" + exe + L"\" --app=" + url +
                           L" --window-size=1360,420 --window-position=180,140 --new-window" +
                           L" --user-data-dir=\"" + g_exeDir + L"\browser-profile\"" +
                           L" --no-first-run --no-default-browser-check" +
                           L" --disable-background-timer-throttling" +
                           L" --disable-backgrounding-occluded-windows" +
                           L" --disable-renderer-backgrounding" +
                           L" --disable-features=CalculateNativeWinOcclusion";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return true;
        }
    }
    HINSTANCE r = ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

// ------------------------------------------------------------------- main ---
static void ParseArgs(int& argc, wchar_t** argv, bool& demo, bool& noBrowser, bool& console) {
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--demo") demo = true;
        else if (a == L"--no-browser") noBrowser = true;
        else if (a == L"--console") console = true;
        else if (a.find(L"--port=") == 0) { g_cfg.port = _wtoi(a.c_str() + 7); g_cliPort = true; }
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    g_exeDir = ExeDir();
    g_logPath = g_exeDir + L"\\handviewer.log";

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool demo = false, noBrowser = false, console = false;
    LoadConfig();
    ParseArgs(argc, argv, demo, noBrowser, console);
    LocalFree(argv);
    if (console) {
        AllocConsole();
        FILE* f;
        f = freopen("CONOUT$", "w", stdout);
        f = freopen("CONOUT$", "w", stderr);
    }
    Log("---- start (pid %lu) ----", GetCurrentProcessId());

    // Name the guard after the port: an instance left running on another port must
    // never stop a new one from starting.
    wchar_t mutexName[128];
    swprintf(mutexName, 128, L"Global\\ChunithmHandViewer_%d", g_cfg.port);
    HANDLE once = CreateMutexW(nullptr, FALSE, mutexName);
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        Log("another instance already serves port %d, opening it", g_cfg.port);
        wchar_t url[256];
        swprintf(url, 256, L"http://127.0.0.1:%d/?v=193", g_cfg.port);
        LaunchViewer(url);
        return 0;
    }

    g_quitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (int i = 0; i < 32; i++) { g_hidP[i].store(0); g_kbCell[i].store(false); }
    for (int i = 0; i < 6; i++) { g_air[i].store(false); g_kbAir[i].store(false); }
    for (int i = 0; i < 2; i++) { g_hidFn[i].store(false); g_kbFn[i].store(false); }

    {
        std::lock_guard<std::mutex> lk(g_vkMu);
        g_vkCell = ParseVkList(g_cfg.cells, 32);
        g_vkAir = ParseVkList(g_cfg.airKeys, 6);
        g_vkFn = ParseVkList(g_cfg.fnKeys, 2);
        g_vkCell.resize(32, 0);
        int nonzero = 0;
        for (int v : g_vkCell) if (v) nonzero++;
        if (nonzero < 32) {
            Log("keymap invalid (%d/32 keys), falling back to defaults", nonzero);
            static const int def[32] = {83,83,83,83,68,68,68,68,70,70,70,70,71,71,71,71,
                                        72,72,72,72,74,74,74,74,75,75,75,75,76,76,76,76};
            for (int i = 0; i < 32; i++) g_vkCell[i] = def[i];
            g_cfg.cells = "SSSS DDDD FFFF GGGG HHHH JJJJ KKKK LLLL";
        }
        g_vkAir.resize(6, 0);
        g_vkFn.resize(2, 0);
        Log("keymap: %zu cell keys, %zu air keys", g_vkCell.size(), g_vkAir.size());
    }

    timeBeginPeriod(1);  // 1 ms timer so the 60 Hz broadcast is not quantised to 15.6 ms

    if (!StartServer()) {
        Log("failed to bind port %d", g_cfg.port);
        MessageBoxW(nullptr, L"端口被占用，无法启动。请修改 handviewer.ini 里的 port。", L"chunithm-handviewer", MB_ICONERROR);
        return 1;
    }
    Log("http://127.0.0.1:%d/", g_cfg.port);

    if (demo) std::thread(DemoThread).detach();
    else {
        if (g_cfg.hid) std::thread(HidThread).detach();
    }

    wchar_t url[256];
    swprintf(url, 256, L"http://127.0.0.1:%d/?v=193", g_cfg.port);
    if (g_cfg.openBrowser && !noBrowser && !demo) {
        if (!LaunchViewer(url)) {
            std::wstring msg = L"无法自动打开浏览器。请手动打开：";
            msg += (wchar_t)10;
            msg += (wchar_t)10;
            msg += url;
            MessageBoxW(nullptr, msg.c_str(), L"chunithm-handviewer", MB_ICONINFORMATION);
        }
    }

    WaitForSingleObject(g_quitEvent, INFINITE);
    g_run.store(false);
    Log("shutting down");
    if (g_listen != INVALID_SOCKET) closesocket(g_listen);
    Sleep(150);
    timeEndPeriod(1);
    return 0;
}
