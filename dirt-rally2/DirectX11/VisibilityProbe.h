// Experimental passive visibility probe. No query/result/plane writes.
// B2A610 ABI verified: uint32 __fastcall(this, query, result); no stack args.
#pragma once
#include <Windows.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

namespace TTVisibilityProbe {
typedef unsigned (__fastcall *QueryFn)(void *, void *, void *);
static QueryFn original = NULL;
static uintptr_t base = 0;
static volatile LONG state = 0, total = 0, last_tick = 0, untracked = 0;
struct Slot {
    PVOID volatile query;
    volatile LONG calls, sampled;
};
static Slot slots[64] = {};

static bool context(uintptr_t *ctx, int *mode)
{
    __try {
        uintptr_t renderer = *(uintptr_t *)(base + 0x159DD90);
        *ctx = renderer ? *(uintptr_t *)(renderer + 0x2050) : 0;
        *mode = *ctx ? *(int *)(*ctx + 0x1E98) : -1;
        return *ctx != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { *ctx = 0; *mode = -2; return false; }
}

struct Sample {
    float planes[24], eye[4], radius;
    int pvs, extra;
    unsigned long long items, secondary;
    unsigned raw[10];
    uintptr_t ctx, camera;
    int mode, equals_main;
};

static bool read_sample(void *query, void *output, Sample *s)
{
    __try {
        memcpy(s->planes, query, sizeof(s->planes));
        memcpy(s->eye, (char *)query + 0xE0, sizeof(s->eye));
        s->radius = *(float *)((char *)query + 0xEC);
        s->pvs = *(int *)((char *)query + 0xF4);
        s->extra = *(int *)((char *)query + 0x124);
        s->items = s->secondary = 0;
        for (int i = 0; i < 16; ++i) {
            s->items += *(unsigned *)((char *)output + i * 0x18);
            s->secondary += *(unsigned *)((char *)output + 0x180 + i * 0x18);
        }
        memcpy(s->raw, (char *)output + 0x320, sizeof(s->raw));
        context(&s->ctx, &s->mode);
        s->camera = s->ctx ? *(uintptr_t *)(s->ctx + 0x17C0) : 0;
        // Equality is evidence only, not authoritative camera identity.
        s->equals_main = s->ctx &&
            memcmp(query, (void *)(s->ctx + 0x14E0), sizeof(s->planes)) == 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static unsigned __fastcall query_hook(void *self, void *query, void *output)
{
    InterlockedIncrement(&total);
    unsigned result = original(self, query, output);
    bool tracked = false;
    for (int i = 0; i < 64; ++i) {
        void *found = InterlockedCompareExchangePointer(&slots[i].query, query, NULL);
        if (found && found != query) continue;
        tracked = true;
        LONG calls = InterlockedIncrement(&slots[i].calls);
        DWORD now = GetTickCount();
        LONG last = InterlockedCompareExchange(&slots[i].sampled, 0, 0);
        if (calls != 1 && (DWORD)(now - (DWORD)last) < 5000) break;
        if (InterlockedCompareExchange(&slots[i].sampled, (LONG)now, last) != last) break;
        Sample s = {};
        if (!read_sample(query, output, &s)) {
            LogInfo("TTVIS: slot=%d calls=%ld sample_read_failed\n", i, calls);
            break;
        }
        LogInfo("TTVIS: slot=%d calls=%ld query=%p self=%p result=%u ctx=%p "
            "mode=%d camera=%p main_planes_equal=%d pvs=%d radius=%.3f extra=%d "
            "eye=(%.3f,%.3f,%.3f) items=%llu secondary=%llu "
            "raw320=[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]\n",
            i, calls, query, self, result, (void *)s.ctx, s.mode, (void *)s.camera,
            s.equals_main, s.pvs, s.radius, s.extra, s.eye[0], s.eye[1], s.eye[2],
            s.items, s.secondary, s.raw[0], s.raw[1], s.raw[2], s.raw[3],
            s.raw[4], s.raw[5], s.raw[6], s.raw[7], s.raw[8], s.raw[9]);
        LogInfo("TTVIS_PLANES: slot=%d "
            "%.9g,%.9g,%.9g,%.9g; %.9g,%.9g,%.9g,%.9g; "
            "%.9g,%.9g,%.9g,%.9g; %.9g,%.9g,%.9g,%.9g; "
            "%.9g,%.9g,%.9g,%.9g; %.9g,%.9g,%.9g,%.9g\n", i,
            s.planes[0],s.planes[1],s.planes[2],s.planes[3],
            s.planes[4],s.planes[5],s.planes[6],s.planes[7],
            s.planes[8],s.planes[9],s.planes[10],s.planes[11],
            s.planes[12],s.planes[13],s.planes[14],s.planes[15],
            s.planes[16],s.planes[17],s.planes[18],s.planes[19],
            s.planes[20],s.planes[21],s.planes[22],s.planes[23]);
        break;
    }
    if (!tracked) InterlockedIncrement(&untracked);
    return result;
}

static bool install()
{
    base = (uintptr_t)GetModuleHandleW(NULL);
    static const unsigned char signature[] = {
        0x4c,0x89,0x44,0x24,0x18,0x53,0x55,0x56,0x57,0x48,0x83,0xec,0x68,
        0x48,0x8b,0xe9,0x49,0x8b,0xd8,0x48,0x81,0xc1,0xf0,0,3,0,0x48,0x8b,0xf2
    };
    static const unsigned char renderer_sig[] = {0x48,0x8b,0x0d,0x20,0x9b,0x20,0x01};
    __try {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
        if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->FileHeader.TimeDateStamp != 0x605CBAE3 ||
            nt->OptionalHeader.SizeOfImage != 0x2255000) return false;
        if (memcmp((void *)(base + 0xB2A610), signature, sizeof(signature)) ||
            memcmp((void *)(base + 0x394269), renderer_sig, sizeof(renderer_sig))) return false;
        if (*(uintptr_t *)(base + 0x13B6FE8) != base + 0xB2EA60 ||
            *(uintptr_t *)(base + 0x13B6FF0) != base + 0xB2A610 ||
            *(uintptr_t *)(base + 0x13B6FF8) != base + 0xB29790) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    original = (QueryFn)(base + 0xB2A610);
    PVOID volatile *entry = (PVOID volatile *)(base + 0x13B6FF0);
    DWORD old = 0, ignored = 0;
    if (!VirtualProtect((void *)entry, sizeof(void *), PAGE_READWRITE, &old)) return false;
    // Aligned pointer replacement: no suspended threads or modified instructions.
    void *was = InterlockedCompareExchangePointer(entry, (void *)&query_hook, (void *)original);
    BOOL restored = VirtualProtect((void *)entry, sizeof(void *), old, &ignored);
    LogInfo("TTVIS: vtable_pointer_installed=%d protection_restored=%d\n",
        was == (void *)original, restored != FALSE);
    return was == (void *)original;
}

static void tick()
{
    LONG current = InterlockedCompareExchange(&state, 0, 0);
    if (current == 0 && InterlockedCompareExchange(&state, 2, 0) == 0) {
        auto it = command_list_globals.find(L"$visibility_probe");
        if (it == command_list_globals.end() || it->second.fval < 0.5f) {
            InterlockedExchange(&state, -2);
            return;
        }
        bool ok = install();
        InterlockedExchange(&state, ok ? 1 : -1);
        LogInfo("TTVIS: passive probe %s; does not change culling.\n", ok ? "ready" : "INACTIVE signature/install failure");
        LogOverlay(LOG_WARNING, ok ? "TrueTriple: registro de visibilidad activo\n" :
            "TrueTriple: fallo al iniciar registro de visibilidad\n");
        current = ok ? 1 : -1;
    }
    if (current != 1) return;
    DWORD now = GetTickCount();
    LONG last = InterlockedCompareExchange(&last_tick, 0, 0);
    if ((DWORD)(now - (DWORD)last) < 2000 ||
        InterlockedCompareExchange(&last_tick, (LONG)now, last) != last) return;
    uintptr_t ctx = 0;
    int mode = -1;
    context(&ctx, &mode);
    LogInfo("TTVIS_HEARTBEAT: calls=%ld untracked=%ld ctx=%p mode=%d\n",
        InterlockedCompareExchange(&total, 0, 0),
        InterlockedCompareExchange(&untracked, 0, 0), (void *)ctx, mode);
}
} // namespace
