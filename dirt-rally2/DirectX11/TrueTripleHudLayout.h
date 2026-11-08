#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

// DR2 1.18.130.9032. Change only the osd_scaler query, never the global
// Eyefinity state (which also shrinks menus and changes camera behavior).
namespace TrueTripleHudLayout {
using DisplayQuery = bool (__fastcall *)(int *, int *, bool *);
using EnabledQuery = bool (*)();
static DisplayQuery original = nullptr;
static EnabledQuery enabled = nullptr;
static uintptr_t osd_vtable = 0;
static volatile LONG corrections = 0;
static volatile LONG install_state = 0;

// 0/1 preserve the original bool result. 3 asks the relay to resume after
// the game's OSD enum getter with enum=middle already in EAX.
static int __fastcall Query(int *columns, int *rows, bool *landscape, void *layout)
{
    bool osd = false;
    __try { osd = layout && *reinterpret_cast<uintptr_t *>(layout) == osd_vtable; }
    __except (EXCEPTION_EXECUTE_HANDLER) { osd = false; }
    if (osd && enabled && enabled()) {
        *columns = 3;
        *rows = 1;
        *landscape = false;
        InterlockedIncrement(&corrections);
        return 3;
    }
    return original(columns, rows, landscape) ? 1 : 0;
}

static size_t MakeRelay(unsigned char *dst, void *target)
{
    // RBX is the layout object at this exact call site. Supply it as arg4.
    // Keep a normal Win64 shadow space/alignment while calling the C++ hook.
    const unsigned char code[] = {
        0x49,0x89,0xD9,                   // mov r9,rbx
        0x48,0xB8,0,0,0,0,0,0,0,0,      // mov rax,target
        0x48,0x83,0xEC,0x28,             // sub rsp,28h
        0xFF,0xD0,                       // call rax
        0x48,0x83,0xC4,0x28,             // add rsp,28h
        0x83,0xF8,0x03,0x75,0x05,        // cmp eax,3; jne normal_return
        0x48,0x83,0x04,0x24,0x0D,        // add qword [rsp],13
        0xC3                             // ret (D79E36 or D79E43)
    };
    memcpy(dst, code, sizeof(code));
    memcpy(dst + 5, &target, sizeof(target));
    return sizeof(code);
}

static bool Validate(unsigned char *base)
{
    __try {
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->FileHeader.TimeDateStamp != 0x605CBAE3 || nt->OptionalHeader.SizeOfImage != 0x2255000) return false;
        // Includes the instruction that supplies the object in RBX, the
        // argument registers, original CALL/test/branch/getter and continuation.
        const unsigned char site[] = {
            0x40,0x32,0xF6,0xE8,0xAA,0xA0,0xED,0xFF,0x84,0xC0,
            0x0F,0x84,0xCC,0x01,0x00,0x00,0xE8,0x8D,0xA0,0xED,0xFF,0x8B,0xF8
        };
        if (memcmp(base + 0xD79E2E, site, sizeof(site))) return false;
        const uintptr_t methods[] = {0xD61250,0xD77F30,0xD79D00,0xD6F070,0xFC0220,0xD6E600,0x50BB70};
        for (size_t i = 0; i < sizeof(methods)/sizeof(methods[0]); ++i)
            if (*reinterpret_cast<uintptr_t *>(base + 0x13DB880 + i*8) != reinterpret_cast<uintptr_t>(base) + methods[i]) return false;
        return memcmp(base + 0x13DB8C8, "osd_scale", 10) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool Install(EnabledQuery gate)
{
    if (InterlockedCompareExchange(&install_state, 1, 0)) return install_state == 2;
    auto base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
    if (!Validate(base)) return false;
    // Only the call displacement changes. Its containing eight-byte word is
    // aligned, so the transition cannot expose a partially written CALL.
    constexpr uintptr_t slot_rva = 0xD79E30;
    static_assert((slot_rva & 7) == 0, "atomic patch alignment");
    unsigned char *relay = nullptr;
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t origin = (reinterpret_cast<uintptr_t>(base) + 0x2255000 + si.dwAllocationGranularity - 1) & ~(uintptr_t(si.dwAllocationGranularity) - 1);
    for (uintptr_t delta = 0; delta < 0x60000000 && !relay; delta += si.dwAllocationGranularity)
        relay = reinterpret_cast<unsigned char *>(VirtualAlloc(reinterpret_cast<void *>(origin + delta), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!relay) return false;
    intptr_t distance = reinterpret_cast<intptr_t>(relay) - reinterpret_cast<intptr_t>(base + 0xD79E36);
    if (distance < INT32_MIN || distance > INT32_MAX) { VirtualFree(relay, 0, MEM_RELEASE); return false; }
    size_t n = MakeRelay(relay, reinterpret_cast<void *>(&Query));
    DWORD old;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old) || !FlushInstructionCache(GetCurrentProcess(), relay, n)) {
        VirtualFree(relay, 0, MEM_RELEASE); return false;
    }
    original = reinterpret_cast<DisplayQuery>(base + 0xC53EE0);
    osd_vtable = reinterpret_cast<uintptr_t>(base + 0x13DB880);
    enabled = gate;
    LONG64 before, after;
    memcpy(&before, base + slot_rva, 8);
    after = before;
    int32_t rel = static_cast<int32_t>(distance);
    memcpy(reinterpret_cast<unsigned char *>(&after) + 2, &rel, 4);
    auto slot = reinterpret_cast<volatile LONG64 *>(base + slot_rva);
    if (!VirtualProtect(base + slot_rva, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool changed = InterlockedCompareExchange64(slot, after, before) == before;
    bool flushed = changed && FlushInstructionCache(GetCurrentProcess(), base + slot_rva, 8);
    if (changed && !flushed) {
        InterlockedCompareExchange64(slot, before, after);
        FlushInstructionCache(GetCurrentProcess(), base + slot_rva, 8);
    }
    DWORD ignored;
    VirtualProtect(base + slot_rva, 8, old, &ignored);
    InterlockedExchange(&install_state, flushed ? 2 : -1);
    return flushed;
}
}
