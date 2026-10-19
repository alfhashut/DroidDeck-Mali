/* Normal-frame local attribution. Fixed counters; exclusive nested scopes.
 * Wall includes RPC/wait/scheduler time. Thread CPU is execution, not GPU time. */
#pragma once
#include "normal_perf.h"
#include <cstdio>
#include <sched.h>
enum MaliCpuGroup { DD_CPU_FRAME, DD_CPU_SHM, DD_CPU_COMPOSITE, DD_CPU_OUTPUT, DD_CPU_GROUPS };
enum MaliCpuStage { DD_CPU_SCENE, DD_CPU_SHM_ACCESS, DD_CPU_COPY, DD_CPU_DESCRIPTORS,
    DD_CPU_UNIFORMS, DD_CPU_COMMANDS, DD_CPU_SOURCE, DD_CPU_TARGET, DD_CPU_SYNC,
    DD_CPU_RETIRE, DD_CPU_SUBMIT, DD_CPU_POOL, DD_CPU_OTHER, DD_CPU_STAGES };
struct MaliCpuDetail {
    struct Sample { uint64_t wall = 0, cpu = 0, sampledWall = 0; } samples[DD_CPU_GROUPS][DD_CPU_STAGES]{};
    bool active = false; unsigned group = DD_CPU_FRAME, stage = DD_CPU_OTHER;
    bool sampleCpu = false; uint64_t serial = 0, cpuSamples = 0;
    uint64_t wall = 0, cpu = 0, shmBytes = 0, uniformBytes = 0, uniformWrites = 0;
    uint64_t uniformOffset = 0, uniformSize = 0, processStart = 0;
    void charge() {
        uint64_t w = dd_perf_now(), c = sampleCpu ? dd_perf_cpu_now() : 0;
        auto &s = samples[group][stage]; s.wall += w - wall;
        if (sampleCpu) s.sampledWall += w - wall;
        s.cpu += c >= cpu ? c - cpu : 0; wall = w; cpu = c;
    }
    void begin() {
        group = DD_CPU_FRAME; stage = DD_CPU_OTHER;
        // THREAD_CPUTIME_ID can be a syscall under proot. Sample one frame in
        // 16; all frames retain the monotonic wall breakdown without that cost.
        sampleCpu = serial++ % 16 == 0; cpuSamples += sampleCpu;
        wall = dd_perf_now(); cpu = sampleCpu ? dd_perf_cpu_now() : 0; active = true;
    }
    void end() { if (active) { charge(); active = false; } }
    void report(unsigned attempts, double parentMs) {
        static const char *groups[] = {"frame-local", "shm-cpu", "compositor-cpu", "output-local"};
        static const char *stages[] = {"scene", "shm-access", "memcpy-convert", "descriptors", "uniforms",
            "commands", "source-resource", "output-resource", "local-sync", "retirement", "submit", "staging-pool", "other"};
        double divisor = (attempts ? attempts : 1) * 1e6;
        double cpuDivisor = (cpuSamples ? cpuSamples : 1) * 1e6;
        for (unsigned g = 0; g < DD_CPU_GROUPS; ++g) {
            uint64_t w = 0, c = 0, sampledWall = 0;
            std::printf("MaliPerf %s:", groups[g]);
            for (unsigned i = 0; i < DD_CPU_STAGES; ++i) {
                auto &s = samples[g][i]; w += s.wall; c += s.cpu; sampledWall += s.sampledWall;
                if (s.wall || s.cpu) std::printf(" %s=%.3f/%.3fms", stages[i], s.wall / divisor, s.cpu / cpuDivisor);
            }
            std::printf(" total=%.3fms thread-CPU=%.3fms off-CPU-est=%.3fms CPU-samples=%llu", w / divisor, c / cpuDivisor,
                (sampledWall > c ? sampledWall - c : 0) / cpuDivisor, (unsigned long long)cpuSamples);
            if (g == DD_CPU_COMPOSITE) std::printf(" parent=%.3fms parent-minus-stages=%.3fms", parentMs, parentMs - w / divisor);
            std::puts(" (exclusive wall-all/thread-CPU-sampled ms/attempt; overlaps existing stage/RPC totals)");
        }
        double n = attempts ? attempts : 1;
        std::printf("MaliPerf client-writes: staging-SHM-copy=%.0fbytes/attempt uniform-store=%.0fbytes/attempt stores=%.2f/attempt last-uniform-offset/size=%llu/%llu (store extents, not byte differences; padding/repeated stores may be included; no scans/hashes)\n",
            shmBytes / n, uniformBytes / n, uniformWrites / n,
            (unsigned long long)uniformOffset, (unsigned long long)uniformSize);
        struct timespec t{}; uint64_t process = 0;
        if (!clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t)) process = uint64_t(t.tv_sec) * 1000000000ull + t.tv_nsec;
        int core = sched_getcpu(); long khz = -1;
        if (core >= 0) {
            char path[128]; std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", core);
            if (FILE *file = std::fopen(path, "r")) { if (std::fscanf(file, "%ld", &khz) != 1) khz = -1; std::fclose(file); }
        }
        std::printf("MaliPerf host-context: process-CPU-delta=%.3fms valid=%u sampled-core=%d current-frequency-kHz=%ld (one boundary sample, -1=unavailable; thermal not sampled; no throttling inference)\n",
            processStart && process >= processStart ? (process - processStart) / 1e6 : 0,
            unsigned(processStart && process >= processStart), core, khz);
        uint64_t nextSerial = serial; *this = {}; processStart = process; serial = nextSerial;
    }
};
inline MaliCpuDetail maliCpuDetail;
struct MaliCpuScope {
    unsigned group = 0, stage = 0; bool enabled;
    explicit MaliCpuScope(unsigned s, unsigned g = DD_CPU_GROUPS) : enabled(maliCpuDetail.active) {
        if (!enabled) return;
        auto &p = maliCpuDetail; p.charge(); group = p.group; stage = p.stage;
        if (g < DD_CPU_GROUPS) p.group = g;
        p.stage = s;
    }
    void set(unsigned s) { if (enabled) { maliCpuDetail.charge(); maliCpuDetail.stage = s; } }
    void finish() {
        if (enabled) { maliCpuDetail.charge(); maliCpuDetail.group = group; maliCpuDetail.stage = stage; enabled = false; }
    }
    ~MaliCpuScope() { finish(); }
};
