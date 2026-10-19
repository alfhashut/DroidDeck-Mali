#!/usr/bin/env python3
"""Compare bounded affinity captures; never interpret poll duration as GPU time."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
import statistics
import sys

sys.path.append(str(Path(__file__).resolve().parents[1] / 'mali-vulkan'))
from steam_swap_details import cpu_bounds, sleep_queue_bounds


def records(path):
    result = defaultdict(list)
    for line in path.read_text().splitlines():
        key, separator, value = line.partition('=')
        if separator:
            try:
                result[key].append(json.loads(value))
            except ValueError:
                pass
    return result


def moments(values):
    values = sorted(values)
    if not values:
        return None
    return dict(n=len(values), mean=statistics.mean(values),
                p50=statistics.median(values), p95=values[int((len(values)-1)*0.95)])


def android_rate(text, start, end):
    rows = []
    for line in text.splitlines():
        match = re.search(r'^\s*(\d+\.\d+).*MaliPerf Android:.*presented-total=(\d+) released-total=(\d+)', line)
        if match and start <= float(match[1]) <= end:
            rows.append(tuple(map(float, match.groups())))
    if len(rows) < 2 or rows[-1][0] <= rows[0][0]:
        return dict(available=False, reason='need two cumulative native counters inside the sampling interval')
    span = rows[-1][0]-rows[0][0]
    if rows[-1][1] < rows[0][1] or rows[-1][2] < rows[0][2]:
        return dict(available=False, reason='counter reset/session change')
    return dict(available=True, counter_span_s=span,
                handoff_hz=(rows[-1][1]-rows[0][1])/span,
                release_hz=(rows[-1][2]-rows[0][2])/span,
                meaning='subinterval inside this arm; cumulative handoff/release, not scanout or per-frame GPU time')


def summarize(directory):
    report = records(directory / 'guest-report.txt')
    restore = report['affinity-restored']
    if not restore or not restore[-1].get('ok') or report['affinity-experiment-error'] or report['affinity-deadline-reached']:
        raise ValueError('affinity restoration/run validity failed: '+str(directory))
    start, end = report['PERF_SAMPLING_START'][0], report['PERF_SAMPLING_END'][0]
    a, b = start['monotonic_ns']/1000, end['monotonic_ns']/1000
    seconds = (b-a)/1e6
    integrity = report['swap-trace-integrity'][0]
    if not integrity.get('complete') or integrity.get('data_loss') is not False or integrity.get('event_cap_hit') or not integrity.get('clocks_aligned'):
        raise ValueError('incomplete or unaligned trace: '+str(directory))
    offset, error = integrity['trace_minus_monotonic_us'], integrity['clock_error_us']
    swaps = report['swap-sample']
    samples = {(row['pid'], row['tid']): row['samples'] for row in report['swap-thread-samples']}
    queues, nonrunnable, poll_counts = [], [], Counter()
    threads = {}
    for swap in swaps:
        key = (swap['pid'], swap['tid'])
        counter = cpu_bounds(samples.get(key, []), swap['trace_begin_us']-offset,
                             swap['trace_end_us']-offset, error)
        exact = swap.get('exact_trace_cpu')
        split = sleep_queue_bounds(exact['trace_off_cpu_ms'], counter) if exact else None
        if split:
            queues.append((split['runnable_queue_ms_lower'], split['runnable_queue_ms_upper']))
            nonrunnable.append((split['nonrunnable_ms_lower'], split['nonrunnable_ms_upper']))
        for row in swap.get('thread_state_observations') or []:
            detail = row.get('syscall_detail') or {}
            if row.get('state') not in ('S', 'D') or detail.get('name') != 'ppoll' or not detail.get('stable_observation'):
                continue
            kinds = {fd.get('kind') for fd in detail.get('fds', [])}
            poll_counts['mali' if 'mali-device' in kinds else 'unclassified'] += 1
    for key, rows in samples.items():
        interior = [row for row in rows if row['begin_ns']/1000 >= a and row['end_ns']/1000 <= b]
        if len(interior) < 2:
            continue
        first, last = interior[0], interior[-1]
        threads[str(key)] = dict(counter_span_s=(last['end_ns']-first['begin_ns'])/1e9,
            cpu_s=(last['cpu_ns']-first['cpu_ns'])/1e9,
            completed_runqueue_s=(last['runqueue_ns']-first['runqueue_ns'])/1e9,
            affinities=sorted({tuple(row.get('affinity', [])) for row in interior}),
            last_cpus=sorted({row.get('last_cpu') for row in interior if row.get('last_cpu') is not None}),
            max_gap_ms=max((y['begin_ns']-x['end_ns'])/1e6 for x,y in zip(interior, interior[1:])),
            meaning='whole sampling subinterval counters, not allocation to swaps; completed queue waits can cross boundaries')
    cef = []
    for row in report['cef-frame-timestamps']:
        count = sum(a+error <= value['ts_us']-offset <= b-error for value in row['samples'])
        cef.append(dict(pid=row['pid'], tid=row['tid'], event=row['name'], phase=row['phase'],
                        count=count, event_hz=count/seconds, capped=row['capped']))
    x11 = {}
    if report['swap-xserver-clock-anchor'] and report['x11-damage-observation']:
        x = report['swap-xserver-clock-anchor'][0]
        midpoint = (x['begin_ns']+x['end_ns'])/2000
        uncertainty = (x['end_ns']-x['begin_ns'])/2000+1000
        for window, values in report['x11-damage-observation'][0]['server_ms_timestamps'].items():
            count = 0
            for stamp in set(values):
                t = midpoint+(((stamp-x['server_ms']+2**31) % 2**32)-2**31)*1000
                count += a+uncertainty <= t <= b-uncertainty
            x11[window] = dict(batches=count, batches_hz=count/seconds)
    gpu = report['swap-cef-gpu-info'][0] if report['swap-cef-gpu-info'] else {}
    aux = gpu.get('auxAttributes', {})
    return dict(directory=str(directory), mask=report['affinity-experiment-start'][0]['mask'],
        seconds=seconds, restored=restore[-1], clock_error_ms=error/1000,
        renderer={k: aux.get(k) for k in ('displayType', 'glRenderer', 'glImplementationParts')},
        swap_wall_ms=moments([row['wall_ms'] for row in swaps]),
        swap_cpu_ms=moments([row['exact_trace_cpu']['trace_thread_cpu_ms'] for row in swaps if row.get('exact_trace_cpu')]),
        swap_off_cpu_ms=moments([row['exact_trace_cpu']['trace_off_cpu_ms'] for row in swaps if row.get('exact_trace_cpu')]),
        mean_swap_runqueue_ms_bounds=[statistics.mean(v[i] for v in queues) for i in (0,1)] if queues else None,
        mean_swap_nonrunnable_ms_bounds=[statistics.mean(v[i] for v in nonrunnable) for i in (0,1)] if nonrunnable else None,
        scheduler_bounded_swaps=len(queues), blocked_ppoll_observations=dict(poll_counts),
        exact_mali_poll_sleep_ms=None, exact_poll_wakeup_to_run_ms=None, gpu_execution_ms=None,
        cef_frame_events=cef, x11_window_updates=x11,
        android=android_rate((directory/'native-logcat.txt').read_text(), start['realtime_ns']/1e9, end['realtime_ns']/1e9),
        sampled_threads=threads,
        warning='No per-wakeup trace. Nonrunnable residual includes all sleeping causes. Compare this arm only; do not sum stage timings from separate runs.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directories', nargs='+', type=Path)
    args = parser.parse_args()
    for directory in args.directories:
        print(json.dumps(summarize(directory), sort_keys=True))
