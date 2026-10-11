#!/usr/bin/env python3
"""Analyze one synchronized capture. Timestamp neighbors never imply frame identity.

Requires an existing Perfetto trace_processor_shell executable; no downloads,
phone mutations, renderer changes, or cross-run timing sums.
"""
import argparse
import csv
import io
import json
from pathlib import Path
import subprocess

from summarize_affinity import records, summarize, moments


def window(report):
    anchors = report['frame-clock-start']+report['frame-clock-end']
    if len(anchors) != 2:
        raise ValueError('need start/end BOOTTIME anchors')
    lower = max(a['boottime_ns']-a['monotonic_after_ns'] for a in anchors)
    upper = min(a['boottime_ns']-a['monotonic_before_ns'] for a in anchors)
    if lower > upper:
        raise ValueError('clock offsets disagree; suspend/clock discontinuity or invalid anchors')
    start = report['PERF_SAMPLING_START'][0]['monotonic_ns']
    end = report['PERF_SAMPLING_END'][0]['monotonic_ns']
    if end <= start:
        raise ValueError('invalid sampling interval')
    return dict(monotonic_start_ns=start, monotonic_end_ns=end,
                boot_minus_monotonic_ns=(lower+upper)//2,
                clock_uncertainty_ns=(upper-lower+1)//2,
                boot_start_ns=start+(lower+upper)//2,
                boot_end_ns=end+(lower+upper)//2)


def latency_rows(directory, bounds):
    snapshots = []
    for phase in ('before', 'after'):
        path = directory/('surfaceflinger-latency-'+phase+'.json')
        if path.exists():
            snapshots += json.loads(path.read_text())
    path = directory/'surfaceflinger-latency-samples.jsonl'
    if path.exists():
        snapshots += [json.loads(line) for line in path.read_text().splitlines()]
    found = set()
    a, b = bounds['monotonic_start_ns'], bounds['monotonic_end_ns']
    for row in snapshots:
        if not row['layer'].startswith('droiddeck_wayland_game') or row.get('returncode') != 0:
            continue
        for line in row.get('output', '').splitlines():
            words = line.split()
            if len(words) != 3 or not all(value.isdigit() for value in words):
                continue
            desired, present, ready = map(int, words)
            if min(desired, present, ready) <= 0 or max(desired, present, ready) >= 2**63-1:
                continue
            if a <= present < b:
                found.add((row['layer'], desired, present, ready))
    return [dict(layer=layer, desired_ns=desired, actual_present_ns=present, frame_ready_ns=ready)
            for layer, desired, present, ready in sorted(found, key=lambda item: item[2])]


def candidates(stamp, rows, radius):
    # An ambiguous match stays ambiguous. Never use nearest-neighbor assignment
    # to manufacture CEF -> AHB -> Android frame IDs or a dropped-frame count.
    return [row for row in rows if abs(row['monotonic_ns']-stamp) <= radius]


def validate_renderer(renderer):
    text = str(renderer.get('glRenderer', '')).lower()
    if (renderer.get('displayType') != 'ANGLE_VULKAN' or 'mali-g52' not in text or
            'panvk' not in text or 'llvmpipe' in text or 'swiftshader' in text):
        raise ValueError('fresh live CEF renderer does not prove ANGLE Vulkan/PanVK/Mali-G52')


def pending_intervals(rows, start, end):
    opened = {}
    previous = {}
    intervals = []
    for row in rows:
        stamp, value, name = int(row['ts']), float(row['value']), row['name']
        if value == 1 and previous.get(name) == 0 and name not in opened and start <= stamp < end:
            opened[name] = stamp
        elif value == 0 and name in opened:
            begin = opened.pop(name)
            if stamp < end:
                intervals.append(dict(track=name, begin_boottime_ns=begin, end_boottime_ns=stamp,
                                      pending_ms=(stamp-begin)/1e6))
        elif value != 1:
            # Multiple pending transactions cannot be assigned an individual
            # residence time from a gauge. Drop the ambiguous interval.
            opened.pop(name, None)
        previous[name] = value
    return intervals


def system_queries(bounds, pids):
    a, b = bounds['boot_start_ns'], bounds['boot_end_ns']
    ids = ','.join(str(int(pid)) for pid in sorted(pids)) or '-1'
    return [
        'select start_ts,end_ts from trace_bounds',
        "select name,severity,value from stats where severity in ('error','data_loss') and value>0",
        f"""select ts,dur,display_frame_token,surface_frame_token,layer_name,present_type,jank_type
        from actual_frame_timeline_slice where ts>={a} and ts<{b}
        and layer_name like '%droiddeck_wayland_game%' order by ts""",
        f"""select s.id,s.ts,s.dur,s.name from slice s
        join slice parent on parent.id=s.parent_id join slice grand on grand.id=parent.parent_id
        where s.ts>={a} and s.ts<{b} and s.name like 'droiddeck_wayland_game%'
        and parent.name='addSurfaceFrame' and grand.name='latchBuffer' order by s.ts""",
        f"""select p.pid,t.tid,t.name,st.state,count(*) observations,
        sum(min(st.ts+st.dur,{b})-max(st.ts,{a})) as duration_ns
        from thread_state st join thread t using(utid) join process p using(upid)
        where p.pid in ({ids}) and st.dur>=0 and st.ts<{b} and st.ts+st.dur>{a}
        group by p.pid,t.tid,st.state order by p.pid,t.tid,st.state""",
        f"""select count(*) as compositions from slice where ts>={a} and ts<{b}
        and name like 'hwc (droiddeck_wayland_game%'""",
        "select int_value from metadata where name='trace_time_clock_id'",
        f"""select c.ts,c.value,t.name from counter c join counter_track t on t.id=c.track_id
        where c.ts>={a} and c.ts<{b} and t.name like 'BufferTX - droiddeck_wayland_game%'
        order by c.ts,c.id""",
    ]


def trace_data(processor, trace, queries):
    result = subprocess.run([str(processor), str(trace), '-Q', ';\n'.join(queries)+';'],
                            text=True, capture_output=True, timeout=120, check=True)
    blocks = result.stdout.strip().split('\n\n')
    if len(blocks) != len(queries):
        raise ValueError('unexpected trace processor output; retain raw trace for inspection')
    return [list(csv.DictReader(io.StringIO(block))) for block in blocks], result.stderr


def analyze(directory, processor):
    report = records(directory/'guest-report.txt')
    # Includes live renderer proof, per-thread mask restoration, and CEF trace
    # completeness checks. Refuse to silently analyze a failed experiment.
    arm = summarize(directory)
    validate_renderer(arm['renderer'])
    if arm['mask'] != '0xff':
        raise ValueError('frame experiment requires temporary 0xff affinity')
    bounds = window(report)
    pids = {int(pid) for inventory in report['process-inventory'] for pid, row in inventory.items()
            if row['role'] in ('gamescope', 'Xwayland', 'steam', 'steamwebhelper', 'broker', 'android-app')}
    # Android app PID is deliberately captured on the host; guest PRoot cannot
    # always inspect it. Do not confuse absent guest inventory with no broker.
    native = directory/'native-cpu.jsonl'
    if native.exists():
        for line in native.read_text().splitlines():
            for row in json.loads(line).get('rows', []):
                if row['key'].startswith('process/'):
                    pids.add(int(row['key'].split('/')[1]))
    tables, messages = trace_data(processor, directory/'system-trace.pftrace', system_queries(bounds, pids))
    trace_bounds, health, timeline, latches, states, compositions, clocks, pending = tables
    if len(clocks) != 1 or int(clocks[0]['int_value']) != 6:
        raise ValueError('system trace does not use BOOTTIME; refusing guessed clock alignment')
    t = trace_bounds[0]
    coverage = int(t['start_ts']) <= bounds['boot_start_ns'] and int(t['end_ts']) >= bounds['boot_end_ns']
    scheduler_recorded = 'sched/sched_switch' in (directory/'perfetto-config.pbtxt').read_text()
    pending_cycles = pending_intervals(pending, bounds['boot_start_ns'], bounds['boot_end_ns'])
    present = latency_rows(directory, bounds)
    seconds = arm['seconds']
    offset = report['swap-trace-integrity'][0]['trace_minus_monotonic_us']
    cef = []
    for row in report['cef-frame-timestamps']:
        if row['name'] != 'SkiaOutputSurfaceImplOnGpu::SwapBuffers' or row['phase'] != 'X':
            continue
        for sample in row['samples']:
            stamp = round((sample['ts_us']-offset)*1000)
            if bounds['monotonic_start_ns'] <= stamp < bounds['monotonic_end_ns']:
                cef.append(dict(monotonic_ns=stamp, pid=row['pid'], tid=row['tid'],
                                wall_us=sample['duration_us'], ids=sample['ids']))
    latch_rows = [dict(slice_id=int(row['id']), name=row['name'],
                       monotonic_ns=int(row['ts'])-bounds['boot_minus_monotonic_ns']) for row in latches]
    x11_rows = []
    if report['swap-xserver-clock-anchor'] and report['x11-damage-observation']:
        anchor = report['swap-xserver-clock-anchor'][0]
        midpoint = (anchor['begin_ns']+anchor['end_ns'])//2
        for xid, timestamps in report['x11-damage-observation'][0]['server_ms_timestamps'].items():
            for stamp in set(timestamps):
                delta = ((stamp-anchor['server_ms']+2**31) % 2**32)-2**31
                x11_rows.append(dict(window=xid, server_ms=stamp, monotonic_ns=midpoint+delta*1000000,
                                     uncertainty_ns=(anchor['end_ns']-anchor['begin_ns'])//2+1000000))
    # Preserve individual timestamps and nearby observations for manual viewing.
    # This is not a stage-duration or causality join.
    per_present = [dict(**row, nearby_cef_swaps=candidates(row['actual_present_ns'], cef, 100000000),
                        nearby_x11_updates=candidates(row['actual_present_ns'], x11_rows, 100000000),
                        nearby_layer_latches=candidates(row['actual_present_ns'], latch_rows, 100000000),
                        correlation='timestamp candidates within +/-100 ms; no shared frame identity')
                   for row in present]
    gaps = {key: None for key in ('gamescope_commit_ids', 'shm_wall_cpu_ms', 'compositor_wall_cpu_ms',
        'vulkan_submit_completion_ms', 'ahb_acquire_ids', 'handoff_release_per_frame',
            'gamescope_compositor_hz', 'coalesced_cef_frames', 'repeated_client_frames',
            'repeated_android_frames', 'queue_depth')}
    fresh = [line for row in report['fresh-log-lines'] for line in row.get('rows', [])]
    return dict(directory=str(directory), sample=bounds, seconds=seconds, mask=arm['mask'],
        restored=arm['restored'], renderer=arm['renderer'], trace_bounds=t,
        system_trace_covers_sample=coverage, trace_health=health,
        scheduler_recorded=scheduler_recorded,
        scheduler_state_split_trustworthy=scheduler_recorded and coverage and not health,
        scheduler_note='Vendor task_state_invalid errors invalidate sleep/runqueue classification; not GPU timing.',
        per_thread_scheduler_states=states, native_layer_frame_timeline=timeline,
        native_layer_latch_hz=len(latches)/seconds if coverage else None,
        native_layer_hwc_composition_count=int(compositions[0]['compositions']),
        native_layer_pending_buffer_transactions=pending,
        pending_buffer_max=max((float(row['value']) for row in pending), default=None),
        pending_single_transaction_intervals=pending_cycles,
        pending_single_transaction_ms=moments([row['pending_ms'] for row in pending_cycles]),
        pending_note='SurfaceFlinger pending buffer transactions; departures mean latch OR drop. Gauge cycles are not bridge frame IDs, GPU time, or AHB ownership duration.',
        hwc_note='Compositions may include unchanged layer content; not a unique-frame/repeat count.',
        native_layer_presentations=per_present, native_layer_present_hz=len(present)/seconds,
        native_layer_present_interval_ms=moments([(y['actual_present_ns']-x['actual_present_ns'])/1e6
                                                  for x, y in zip(present, present[1:]) if x['layer']==y['layer']]),
        latency_note='128-entry histories deduplicated across snapshots; missing history can undercount. Presentation is distinct from AHB release.',
        cef_frame_events=arm['cef_frame_events'], x11_updates=arm['x11_window_updates'],
        x11_windows=report['x11-window-snapshot'], sampled_threads=arm['sampled_threads'],
        android=arm['android'], swap_wall_ms=arm['swap_wall_ms'], swap_cpu_ms=arm['swap_cpu_ms'],
        swap_off_cpu_ms=arm['swap_off_cpu_ms'], stages_unobservable=gaps,
        session_log_consumers=report['frame-session-log-consumer'],
        gamescope_stage_coverage=report['frame-stage-coverage'],
        fresh_warning_lines=[line for line in fresh if 'KBASE_IOCTL_MEM_JIT_INIT' in line or
                             '[KFAULT]' in line or 'BadMatch' in line or 'AcquirePixmap' in line],
        log_coverage=report['log-sample-coverage'],
        trace_processor_messages=messages,
        conclusion='No exact end-to-end bottleneck attribution without missing Gamescope/AHB frame IDs and stage scopes. No timings combined across runs.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--trace-processor', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.directory, args.trace_processor), indent=2))
