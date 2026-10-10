/* Fixed slot count; completion knowledge is supplied by the real backend.
 * No slot destructor frees Vulkan resources: shutdown must prove safety. */
#pragma once
#include <cstdint>
inline thread_local bool maliStagingUploadRecording = false;
struct MaliStagingUploadScope {
    bool previous = maliStagingUploadRecording;
    MaliStagingUploadScope() { maliStagingUploadRecording = true; }
    ~MaliStagingUploadScope() { maliStagingUploadRecording = previous; }
};
template<class Backend> struct MaliStagingPool {
    static constexpr unsigned limit = 3, preferred = 2;
    using Resource = typename Backend::Resource;
    struct Slot { Resource resource{}; uint64_t capacity = 0, sequence = 0; bool live = false, reserved = false; };
    struct Counters { uint64_t hits = 0, misses = 0, waits = 0, created = 0, resized = 0, failures = 0, mapReuses = 0; } stats;
    Backend backend;
    Slot slots[limit]{};
    unsigned cursor = 0;
    unsigned live() const { unsigned n = 0; for (const auto &s : slots) n += s.live; return n; }
    unsigned mappedLive() const { unsigned n = 0; for (const auto &s : slots) n += s.live && s.resource.mapped; return n; }
    bool safe(const Slot &s) { return !s.sequence || backend.completed(s.sequence); }
    int make(Slot &s, uint64_t bytes, Slot *&out) {
        int result = backend.create(bytes, s.resource);
        if (result) { ++stats.failures; return result; }
        s.live = s.reserved = true; s.capacity = bytes; s.sequence = 0;
        ++stats.created; out = &s; return 0;
    }
    int acquire(uint64_t bytes, Slot *&out) {
        out = nullptr;
        if (!bytes) return Backend::invalid;
        // Warm up two slots, then prefer completed resources over growing.
        if (live() < preferred) for (auto &s : slots) if (!s.live) {
            ++stats.misses; return make(s, bytes, out);
        }
        for (unsigned i = 0; i < limit; ++i) {
            unsigned index = (cursor + i) % limit; auto &s = slots[index];
            if (!s.live || s.reserved || s.capacity < bytes || !safe(s)) continue;
            ++stats.hits; ++stats.mapReuses; s.reserved = true; cursor = (index + 1) % limit; out = &s; return 0;
        }
        ++stats.misses;
        // Resize only a proven completed slot. Never destroy pending storage.
        for (auto &s : slots) if (s.live && !s.reserved && safe(s)) {
            backend.destroy(s.resource, s.sequence); s = {}; ++stats.resized; return make(s, bytes, out);
        }
        for (auto &s : slots) if (!s.live) return make(s, bytes, out);
        Slot *oldest = nullptr;
        for (auto &s : slots) if (!s.reserved && (!oldest || s.sequence < oldest->sequence)) oldest = &s;
        if (!oldest) { ++stats.failures; return Backend::invalid; }
        ++stats.waits;
        int result = backend.wait(oldest->sequence, oldest->resource);
        if (result || !safe(*oldest)) { ++stats.failures; return result ? result : Backend::invalid; }
        if (oldest->capacity < bytes) {
            backend.destroy(oldest->resource, oldest->sequence); *oldest = {}; ++stats.resized; return make(*oldest, bytes, out);
        }
        ++stats.mapReuses; oldest->reserved = true; out = oldest; return 0;
    }
    struct Lease {
        Slot *slot = nullptr;
        Lease() = default;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        ~Lease() { if (slot) slot->reserved = false; }
        void submitted(uint64_t sequence) { slot->sequence = sequence; slot->reserved = false; slot = nullptr; }
    };
    int shutdown() {
        int first = 0;
        for (auto &s : slots) {
            if (!s.live) continue;
            int result = s.reserved ? Backend::invalid : safe(s) ? 0 : backend.wait(s.sequence, s.resource);
            if (result || !safe(s)) { if (!first) first = result ? result : Backend::invalid; continue; }
            backend.destroy(s.resource, s.sequence); s = {};
        }
        return first; // Failed/pending slots remain live, never assumed released.
    }
};
