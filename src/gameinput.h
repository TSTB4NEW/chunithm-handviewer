// gameinput.h - read the TASOLLER input buffer out of a running chusanApp.exe.
//
// The stock chuniio driver (tasoller_plus.dll) owns the WinUSB device while the
// game runs, so a second process cannot open it. But the driver keeps the last
// 36-byte report in a global buffer (taso_v2_input_buffer), so we can simply
// read that buffer with ReadProcessMemory. This is exactly the same data the
// driver hands to the game.
#pragma once
#include <tlhelp32.h>

static DWORD FindProcessByName(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    static bool dbg = false;
    if (snap == INVALID_HANDLE_VALUE) {
        if (!dbg) { dbg = true; Log("game: process snapshot failed (%lu)", GetLastError()); }
        return 0;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    int count = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            count++;
            if (!dbg && count <= 200) {
                char n[64];
                WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, n, sizeof(n), nullptr, nullptr);
                if (strstr(n, "chusan")) Log("game: saw process '%s' pid %lu", n, pe.th32ProcessID);
            }
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    if (!dbg) { dbg = true; Log("game: scanned %d processes, pid=%lu", count, pid); }
    CloseHandle(snap);
    return pid;
}

static bool FindModuleBase(DWORD pid, const wchar_t* dllName, uint64_t& base, uint32_t& size) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, dllName) == 0) {
                base = (uint64_t)(uintptr_t)me.modBaseAddr;
                size = me.modBaseSize;
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static bool ReadProc(HANDLE proc, uint64_t va, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(proc, (LPCVOID)(uintptr_t)va, out, n, &got) && got == n;
}

// Look for the ReadPipe setup code:
//   mov dword [esp+0Ch], 24h
//   mov dword [esp+08h], <buffer>   <- absolute VA of taso_v2_input_buffer
//   mov dword [esp+04h], 84h
static bool ScanForBufferVa(HANDLE proc, uint64_t base, uint32_t size, uint64_t& va) {
    static const uint8_t prefix[] = {0xC7, 0x44, 0x24, 0x0C, 0x24, 0x00, 0x00, 0x00, 0xC7, 0x44, 0x24, 0x08};
    static const uint8_t suffix[] = {0xC7, 0x44, 0x24, 0x04, 0x84, 0x00, 0x00, 0x00, 0x89, 0x04, 0x24, 0xE8};
    const size_t chunk = 0x1000;
    std::vector<uint8_t> buf(chunk + 64);
    for (uint64_t off = 0; off + chunk < size; off += chunk) {
        SIZE_T got = 0;
        if (!ReadProcessMemory(proc, (LPCVOID)(uintptr_t)(base + off), buf.data(), chunk, &got) || got < 32) continue;
        for (size_t i = 0; i + 28 < got; i++) {
            if (memcmp(buf.data() + i, prefix, sizeof(prefix)) != 0) continue;
            if (memcmp(buf.data() + i + 16, suffix, sizeof(suffix)) != 0) continue;
            uint32_t addr = 0;
            memcpy(&addr, buf.data() + i + 12, 4);
            va = addr;
            return true;
        }
    }
    return false;
}

// Returns true when a game session was attached (function blocks while it lasts).
static bool TryGameInput() {
    DWORD pid = FindProcessByName(L"chusanApp.exe");
    if (!pid) return false;
    HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) {
        static DWORD lastLog = 0;
        DWORD now = GetTickCount();
        if (now - lastLog > 15000) {
            Log("game: chusanApp.exe found but OpenProcess failed (%lu) - run the viewer as administrator", GetLastError());
            lastLog = now;
        }
        Sleep(1000);
        return true;  // game owns the device, do not fall through to WinUSB
    }
    uint64_t base = 0;
    uint32_t size = 0;
    if (!FindModuleBase(pid, L"tasoller_plus.dll", base, size)) {
        Sleep(600);
        CloseHandle(proc);
        return true;
    }
    uint64_t va = base + 0x8A40;  // taso_v2_input_buffer for this build
    uint8_t probe[3] = {0};
    if (!ReadProc(proc, va, probe, 3) || probe[0] != 'D' || probe[1] != 'B' || probe[2] != 'T') {
        if (!ScanForBufferVa(proc, base, size, va)) {
            static DWORD lastWait = 0;
            DWORD now = GetTickCount();
            if (now - lastWait > 5000) { Log("game: waiting for tasoller_plus.dll input buffer"); lastWait = now; }
            Sleep(600);
            CloseHandle(proc);
            return true;
        }
        Log("game: located input buffer by pattern at %llx", (unsigned long long)va);
    }
    Log("game: attached to chusanApp.exe (pid %lu), buffer at %llx", pid, (unsigned long long)va);
    g_srcHid.store(5);
    SetHidName("chusanApp / tasoller_plus.dll");
    // One read covers both globals: input report at +0x00 and the LED packet
    // at +0x40 (0x8A40 -> 0x8A80 in this build).
    uint8_t buf[178];
    int fails = 0;
    while (g_run.load()) {
        if (!ReadProc(proc, va, buf, sizeof(buf))) {
            if (++fails > 20) break;
            Sleep(5);
            continue;
        }
        fails = 0;
        ApplyReport(buf, 36);
        ApplyLed(buf + 0x40, 114);
        Sleep(1);
    }
    for (int i = 0; i < 32; i++) g_led[i].store(0);
    g_ledSeen.store(false);
    g_srcHid.store(0);
    SetHidName("");
    CloseHandle(proc);
    Log("game: detached");
    return true;
}
