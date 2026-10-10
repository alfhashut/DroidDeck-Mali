/* Observe once, then ALWAYS perform the original real wait with its exact
 * arguments. Counter-query failure affects attribution only, never the wait. */
static VkResult native_profiled_wait(PFN_vkGetSemaphoreCounterValueKHR counter,
        PFN_vkWaitSemaphoresKHR wait, VkDevice device, const VkSemaphoreWaitInfoKHR *info,
        uint64_t timeout, struct dd_wait_sample *sample) {
    uint64_t query_start = dd_perf_now();
    sample->before_result = (uint32_t)counter(device, info->pSemaphores[0], &sample->before);
    sample->query_ns = dd_perf_now() - query_start;
    uint64_t cpu_start = dd_perf_cpu_now(), wall_start = dd_perf_now();
    VkResult result = wait(device, info, timeout);
    sample->wall_ns = dd_perf_now() - wall_start;
    sample->cpu_ns = dd_perf_cpu_now() - cpu_start;
    return result;
}
