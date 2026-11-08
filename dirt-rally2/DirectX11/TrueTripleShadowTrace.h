#pragma once
// Temporary opt-in diagnostic. Copies inputs, never edits shader state or data.
#include <d3d11_1.h>
#include <wrl/client.h>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <string>
#include <atomic>
#include <share.h>

namespace TrueTripleShadowTrace {
using Microsoft::WRL::ComPtr;
inline bool IsShadow(uint64_t h) {
    return h == 0x7b1c3954b16cb924ULL || h == 0x8f4b91ddaa6d349cULL ||
        h == 0x2a12730f88510d16ULL || h == 0x794efcbfc3a6b0c3ULL ||
        h == 0xdcedabb6dc07fd9eULL || h == 0xf09ffcea62344a8eULL;
}
#pragma pack(push, 1)
struct Record {
    uint32_t magic = 0x31534854; // THS1
    uint32_t size = sizeof(Record);
    uint64_t sequence = 0, ms = 0, shader = 0, context = 0;
    uint32_t recordFrame = 0, executeFrame = 0, readyFrame = 0, deferred = 0, thread = 0;
    uint32_t groups[3] = {};
    uint32_t bytes[4] = {}; // cb0, cb3, cb5, canonical IniParams texture
    uint32_t first[3] = {}, count[3] = {};
};
#pragma pack(pop)
struct Slot {
    Record record;
    ComPtr<ID3D11Resource> copy[4];
    ComPtr<ID3D11Query> event;
    ComPtr<ID3D11CommandList> list;
    bool pending = false, submitted = false;
    void Retire() {
        pending = false; list.Reset();
        // Deferred lists can be replayed. Never recycle their destinations/query.
        if (record.deferred) { for (auto& c : copy) c.Reset(); event.Reset(); }
    }
};
class Recorder {
    std::mutex mutex;
    Slot slots[32];
    FILE *file = nullptr;
    std::wstring statusPath;
    uint64_t start = 0, written = 0, accepted = 0, done = 0, dropped = 0, errors = 0;
    HRESULT lastError = S_OK;
    bool initialized = false;
    std::atomic<bool> accepting{false};
    std::atomic<bool> open{false};
    void Status(const char *phase) {
        FILE *s = nullptr;
        if (!_wfopen_s(&s, statusPath.c_str(), L"wb") && s) {
            fprintf(s, "TTST1 phase=%s queued=%llu written=%llu dropped=%llu errors=%llu bytes=%llu last_hr=%08lx\n",
                phase, accepted, done, dropped, errors, written, lastError);
            fclose(s);
        }
    }
    bool CopyInput(ID3D11DeviceContext *ctx, ID3D11Device *device, ID3D11Resource *source,
                   ComPtr<ID3D11Resource>& copy, uint32_t& bytes) {
        if (!source) { copy.Reset(); bytes = 0; return true; }
        D3D11_RESOURCE_DIMENSION kind; source->GetType(&kind);
        if (kind == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d; static_cast<ID3D11Buffer*>(source)->GetDesc(&d);
            if (d.ByteWidth > 65536) return false;
            bytes = d.ByteWidth;
            if (copy) {
                D3D11_RESOURCE_DIMENSION old; copy->GetType(&old);
                if (old != kind) copy.Reset();
                else { D3D11_BUFFER_DESC prev; static_cast<ID3D11Buffer*>(copy.Get())->GetDesc(&prev);
                    if (prev.ByteWidth != d.ByteWidth) copy.Reset(); }
            }
            if (!copy) {
                d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                d.MiscFlags = 0; d.StructureByteStride = 0;
                ComPtr<ID3D11Buffer> b;
                if (FAILED(device->CreateBuffer(&d, nullptr, &b))) return false;
                copy = b;
            }
        } else if (kind == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
            D3D11_TEXTURE1D_DESC d; static_cast<ID3D11Texture1D*>(source)->GetDesc(&d);
            if (d.Format != DXGI_FORMAT_R32G32B32A32_FLOAT || d.Width > 4096 || d.ArraySize != 1 || d.MipLevels != 1) return false;
            bytes = d.Width * 16;
            if (copy) {
                D3D11_RESOURCE_DIMENSION old; copy->GetType(&old);
                if (old != kind) copy.Reset();
                else { D3D11_TEXTURE1D_DESC prev; static_cast<ID3D11Texture1D*>(copy.Get())->GetDesc(&prev);
                    if (prev.Width != d.Width || prev.Format != d.Format) copy.Reset(); }
            }
            if (!copy) {
                d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
                ComPtr<ID3D11Texture1D> t;
                if (FAILED(device->CreateTexture1D(&d, nullptr, &t))) return false;
                copy = t;
            }
        } else return false;
        ctx->CopyResource(copy.Get(), source);
        return true;
    }
public:
    bool Enabled() { return accepting; } // Called on the serialized context path after Init.
    void Start(const std::wstring& path) {
        if (file) return;
        file = _wfsopen(path.c_str(), L"wb", _SH_DENYNO);
        if (!file) return;
        setvbuf(file, nullptr, _IOFBF, 1024 * 1024);
        statusPath = path + L".status.txt";
        start = GetTickCount64(); accepting = true; open = true; Status("armed");
    }
    void Init() {
        std::lock_guard<std::mutex> lock(mutex);
        if (initialized) return;
        initialized = true;
        wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe); dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        if (GetFileAttributesW((dir + L"TrueTriple-shadow-trace.arm").c_str()) == INVALID_FILE_ATTRIBUTES) return;
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t name[128]; swprintf_s(name, L"TrueTriple-shadow-trace-%04u%02u%02u-%02u%02u%02u-%lu.bin",
            t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,GetCurrentProcessId());
        Start(dir + name);
    }
    void Capture(ID3D11DeviceContext *ctx, uint64_t hash, uint32_t frame, const UINT groups[3],
                 ID3D11Buffer *const buffers[3], const UINT first[3], const UINT counts[3], ID3D11Texture1D *ini) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!accepting) return;
        if (GetTickCount64() - start > 300000 || accepted >= 60000 || written >= 64 * 1024 * 1024) {
            accepting = false; fflush(file); Status("limit-draining"); return;
        }
        Slot *slot = nullptr;
        for (auto& s : slots) if (!s.pending) { slot = &s; break; }
        if (!slot) { ++dropped; return; }
        ComPtr<ID3D11Device> device; ctx->GetDevice(&device);
        if (!slot->event) {
            D3D11_QUERY_DESC d = { D3D11_QUERY_EVENT, 0 };
            if (FAILED(device->CreateQuery(&d, &slot->event))) { ++errors; return; }
        }
        auto& r = slot->record; r = Record();
        r.sequence = ++accepted; r.ms = GetTickCount64() - start; r.shader = hash;
        r.context = reinterpret_cast<uint64_t>(ctx); r.recordFrame = frame;
        r.deferred = ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED; r.thread = GetCurrentThreadId();
        r.executeFrame = r.deferred ? UINT32_MAX : frame;
        slot->submitted = !r.deferred;
        memcpy(r.groups, groups, sizeof(r.groups)); memcpy(r.first, first, sizeof(r.first)); memcpy(r.count, counts, sizeof(r.count));
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            ID3D11Resource *source = i == 3 ? static_cast<ID3D11Resource*>(ini) : buffers[i];
            if (!CopyInput(ctx, device.Get(), source, slot->copy[i], r.bytes[i])) { ok = false; break; }
            r.size += r.bytes[i];
        }
        // Even a partial copy must complete before this staging slot can be reused.
        if (!ok) { ++errors; r.magic = 0; }
        ctx->End(slot->event.Get()); slot->pending = true;
    }
    void Finish(ID3D11DeviceContext *ctx, ID3D11CommandList *list) {
        if (!open || !list) return;
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& s : slots) if (s.pending && !s.submitted && !s.list &&
            s.record.context == reinterpret_cast<uint64_t>(ctx)) s.list = list;
    }
    void Executed(ID3D11CommandList *list, uint32_t frame) {
        if (!open) return;
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& s : slots) if (s.pending && s.list.Get() == list) {
            s.submitted = true; s.record.executeFrame = frame;
        }
    }
    void Pump(ID3D11DeviceContext *ctx, uint32_t frame) {
        if (!open) return;
        if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
        std::lock_guard<std::mutex> lock(mutex);
        if (!file) return;
        for (auto& s : slots) {
            if (!s.pending || !s.submitted) continue;
            BOOL ready = FALSE;
            HRESULT hr = ctx->GetData(s.event.Get(), &ready, sizeof(ready), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE || (hr == S_OK && !ready)) continue;
            if (FAILED(hr)) { ++errors; lastError=hr; s.Retire(); continue; }
            if (!s.record.magic) { s.Retire(); continue; }
            std::vector<unsigned char> payload;
            bool ok = true;
            for (int i = 0; i < 4; ++i) {
                if (!s.record.bytes[i]) continue;
                D3D11_MAPPED_SUBRESOURCE m = {};
                hr = ctx->Map(s.copy[i].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
                if (FAILED(hr)) { ok = false; ++errors; lastError=hr; break; }
                const auto *p = static_cast<const unsigned char*>(m.pData);
                payload.insert(payload.end(), p, p + s.record.bytes[i]);
                ctx->Unmap(s.copy[i].Get(), 0);
            }
            if (ok && written + s.record.size <= 64 * 1024 * 1024) {
                s.record.readyFrame = frame;
                if (fwrite(&s.record, sizeof(Record), 1, file) != 1 || fwrite(payload.data(), 1, payload.size(), file) != payload.size()) {
                    accepting = false; ++errors;
                } else { written += s.record.size; ++done; }
                if (done == 1 || done % 120 == 0) { fflush(file); Status(accepting ? "recording" : "draining"); }
            }
            s.Retire();
        }
        if (!accepting) {
            bool pending = false; for (auto& s : slots) pending |= s.pending;
            if (!pending) { fflush(file); Status("stopped"); fclose(file); file = nullptr; open = false; }
        }
    }
    void Close() {
        std::lock_guard<std::mutex> lock(mutex);
        accepting = false;
        if (file) { fflush(file); Status("closed"); fclose(file); file = nullptr; open = false; }
    }
    ~Recorder() { if (file) fclose(file); }
};
inline Recorder& Session() { static Recorder recorder; return recorder; }

// Shadow bindings belong to EACH context; Get* is forbidden on deferred contexts.
struct Bindings {
    ComPtr<ID3D11Buffer> cb[3];
    UINT first[3] = {}, count[3] = {};
    void Set(UINT start, UINT n, ID3D11Buffer *const *buffers, const UINT *offset = nullptr, const UINT *length = nullptr) {
        if (!Session().Enabled()) return;
        const UINT slots[] = {0, 3, 5};
        for (int i = 0; i < 3; ++i) if (slots[i] >= start && slots[i] < start+n) {
            UINT j = slots[i]-start; cb[i] = buffers ? buffers[j] : nullptr;
            first[i] = offset ? offset[j] : 0;
            count[i] = length ? length[j] : 4096;
        }
    }
    void Clear() { for (auto& b : cb) b.Reset(); memset(first, 0, sizeof(first)); memset(count, 0, sizeof(count)); }
};
}
