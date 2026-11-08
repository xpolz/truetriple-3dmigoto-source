#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>

// DR2 1.18.130.9032: the solar fitter reads camera.aspect twice: to build
// perspective cascade corners AND to stabilize/quantize the light bounds.
// Supply a stack-local, read-only camera snapshot to that one call. Never
// restore the shared CPU camera: grass/objects must retain their wide view.
namespace TrueTripleShadowFit {
using Fit = uintptr_t (__fastcall *)(void *, void *, void *, const void *, void *, void *);
using Gate = bool (*)();
static Fit original = nullptr;
static Gate enabled = nullptr;
static volatile LONG install_state = 0, corrections = 0, misses = 0;
static SRWLOCK camera_lock = SRWLOCK_INIT;
struct CameraAspect { const void *camera; float native_aspect, wide_aspect; };
static CameraAspect cameras[16] = {};
static unsigned next_camera = 0;
constexpr size_t CameraBytes = 0x1d0; // last read is MOVAPS [r15+1c0]

static bool ValidAspect(float f) { return std::isfinite(f) && f > 0.05f && f < 1000.0f; }

// Called after the camera constructor. Store a coherent pair tied to the
// actual camera object, not a global "last m00" from an unrelated viewport.
static void Publish(const void *camera, float native_aspect, float wide_aspect)
{
    AcquireSRWLockExclusive(&camera_lock);
    unsigned slot = 16;
    for (unsigned i=0; i<16; ++i) if (cameras[i].camera == camera) { slot=i; break; }
    if (slot==16) slot = next_camera++ % 16;
    cameras[slot] = {camera, native_aspect, wide_aspect};
    ReleaseSRWLockExclusive(&camera_lock);
}

static bool Snapshot(const void *camera, unsigned char *copy)
{
    CameraAspect pair = {};
    AcquireSRWLockShared(&camera_lock);
    for (unsigned i=0; i<16; ++i) if (cameras[i].camera==camera) { pair=cameras[i]; break; }
    ReleaseSRWLockShared(&camera_lock);
    if (!camera || !ValidAspect(pair.native_aspect) || !ValidAspect(pair.wide_aspect)) return false;
    __try {
        // The exact fitter only reads this object. It never calls its vtable,
        // writes it, retains its address, or forwards it to another function.
        const auto bytes = static_cast<const unsigned char *>(camera);
        float actual; memcpy(&actual, bytes+0x130, 4);
        if (!ValidAspect(actual) || fabsf(actual-pair.wide_aspect) > pair.wide_aspect*1e-5f) return false;
        if (fabsf(actual-pair.native_aspect) <= actual*1e-5f) return false;
        memcpy(copy, camera, CameraBytes);
        memcpy(copy+0x130, &pair.native_aspect, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static uintptr_t __fastcall Query(void *owner, void *slices, void *context,
    const void *camera, void *pose, void *light)
{
    alignas(16) unsigned char local[CameraBytes];
    if (enabled && enabled() && Snapshot(camera, local)) {
        InterlockedIncrement(&corrections);
        return original(owner, slices, context, local, pose, light);
    }
    InterlockedIncrement(&misses);
    return original(owner, slices, context, camera, pose, light);
}

static bool Validate(unsigned char *base)
{
    __try {
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        if (dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0 || dos->e_lfanew>0x100000) return false;
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base+dos->e_lfanew);
        if (nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
            nt->FileHeader.TimeDateStamp!=0x605CBAE3 || nt->OptionalHeader.SizeOfImage!=0x2255000) return false;
        const unsigned char args[] = {0x4d,0x8b,0xcd,0x4c,0x89,0x74,0x24,0x20,0x4c,0x8b,0xc3,0x48,0x8b,0xce};
        if (memcmp(base+0x3c63c4,args,sizeof(args))) return false;
        const auto call = base+0x3c63d2;
        int32_t rel; memcpy(&rel,call+1,4);
        if (call[0]!=0xe8 || call+5+rel!=base+0x3a8da0) return false;
        const unsigned char load[] = {0xf3,0x41,0x0f,0x10,0xbf,0x30,0x01,0x00,0x00};
        const unsigned char mul[]  = {0xf3,0x41,0x0f,0x59,0x87,0x30,0x01,0x00,0x00};
        return !memcmp(base+0x3a8ed6,load,sizeof(load)) && !memcmp(base+0x3a9424,mul,sizeof(mul));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static float NativeAspect(intptr_t context)
{
    __try {
        auto rt=*reinterpret_cast<intptr_t *>(context+0xfa0);
        if (!rt) return 0;
        unsigned w=*reinterpret_cast<unsigned *>(rt+0xa4), h=*reinterpret_cast<unsigned *>(rt+0xa8);
        if (!w || !h) return 0;
        float value=(float(w)/float(h))* *reinterpret_cast<float *>(context+0xfe0);
        return ValidAspect(value) ? value : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void PublishConstructed(const void *camera, float native_aspect)
{
    float wide=0;
    __try { wide=*reinterpret_cast<const float *>(static_cast<const unsigned char *>(camera)+0x130); }
    __except (EXCEPTION_EXECUTE_HANDLER) { native_aspect=0; }
    Publish(camera,native_aspect,wide);
}

static bool Install(Gate gate)
{
    if (InterlockedCompareExchange(&install_state,1,0)) return install_state==2;
    auto base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
    if (!Validate(base)) { InterlockedExchange(&install_state,-1); return false; }
    SYSTEM_INFO si; GetSystemInfo(&si);
    auto origin = (reinterpret_cast<uintptr_t>(base)+0x2255000+si.dwAllocationGranularity-1) & ~(uintptr_t(si.dwAllocationGranularity)-1);
    unsigned char *relay=nullptr;
    for (uintptr_t delta=0; delta<0x60000000 && !relay; delta+=si.dwAllocationGranularity)
        relay=static_cast<unsigned char *>(VirtualAlloc(reinterpret_cast<void *>(origin+delta),4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if (!relay) { InterlockedExchange(&install_state,-1); return false; }
    intptr_t distance = reinterpret_cast<intptr_t>(relay)-reinterpret_cast<intptr_t>(base+0x3c63d7);
    if (distance<INT32_MIN || distance>INT32_MAX) { VirtualFree(relay,0,MEM_RELEASE); InterlockedExchange(&install_state,-1); return false; }
    // Tail jump preserves the game's return address and both stack arguments.
    const unsigned char jump[] = {0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};
    memcpy(relay,jump,sizeof(jump));
    auto target=&Query; memcpy(relay+2,&target,8);
    DWORD old;
    if (!VirtualProtect(relay,4096,PAGE_EXECUTE_READ,&old) || !FlushInstructionCache(GetCurrentProcess(),relay,sizeof(jump))) {
        VirtualFree(relay,0,MEM_RELEASE); InterlockedExchange(&install_state,-1); return false;
    }
    original=reinterpret_cast<Fit>(base+0x3a8da0); enabled=gate;
    // Entire CALL lies within one aligned eight-byte word; atomically publish
    // the rel32 without suspending threads or relocating a function prologue.
    constexpr uintptr_t slot_rva=0x3c63d0;
    LONG64 before,after; memcpy(&before,base+slot_rva,8); after=before;
    int32_t rel=static_cast<int32_t>(distance); memcpy(reinterpret_cast<unsigned char *>(&after)+3,&rel,4);
    if (!VirtualProtect(base+slot_rva,8,PAGE_EXECUTE_READWRITE,&old)) { InterlockedExchange(&install_state,-1); return false; }
    auto slot=reinterpret_cast<volatile LONG64 *>(base+slot_rva);
    bool changed=InterlockedCompareExchange64(slot,after,before)==before;
    bool flushed=changed && FlushInstructionCache(GetCurrentProcess(),base+slot_rva,8);
    if (changed && !flushed) { InterlockedCompareExchange64(slot,before,after); FlushInstructionCache(GetCurrentProcess(),base+slot_rva,8); }
    DWORD ignored; VirtualProtect(base+slot_rva,8,old,&ignored);
    InterlockedExchange(&install_state,flushed ? 2 : -1);
    return flushed;
}
}
