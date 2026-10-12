#!/usr/bin/env python3
"""Exact-ID internal frame analysis. Never equates timestamp proximity with identity."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import statistics

SHM_CHILD_STAGES = (
    'shm_lookup', 'shm_source', 'staging_acquire', 'staging_create', 'staging_map',
    'staging_validate', 'shm_copy', 'shm_source_release', 'texture_create',
    'upload_record', 'upload_prepare', 'upload_submit', 'upload_finalize',
    'upload_queue', 'upload_wait', 'shm_cleanup',
)
UPLOAD_QUEUE_CHILD_STAGES = (
    'proxy_prepare', 'proxy_lock', 'proxy_mirror', 'proxy_dirty', 'proxy_bulk_setup',
    'proxy_bulk_pack', 'proxy_bulk_rpc', 'proxy_bulk_ack', 'proxy_submit_pack',
    'proxy_submit_rpc', 'proxy_reply_handle', 'proxy_finalize',
    'rpc_header', 'rpc_send', 'rpc_receive', 'rpc_decode', 'rpc_exchange',
)
BROKER_STAGES = ('broker_receive', 'broker_dispatch', 'broker_reply', 'broker_mapped_copy')
SPAN_STAGES = frozenset(SHM_CHILD_STAGES + UPLOAD_QUEUE_CHILD_STAGES + BROKER_STAGES) | {
    'frame', 'shm_import', 'ahb_acquire', 'render', 'wait', 'vk_submit', 'android_handoff',
}
# These always execute on a successful data-pointer import, including fallback.
SHM_REQUIRED_STAGES = {
    'shm_lookup', 'shm_source', 'staging_acquire', 'staging_validate', 'shm_copy',
    'shm_source_release', 'texture_create', 'upload_record', 'upload_submit',
}


def load(path):
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    if not rows or rows[0].get('type') != 'header' or rows[-1].get('type') != 'footer':
        raise ValueError(f'{path}: missing header/footer')
    header, footer = rows[0], rows[-1]
    events = rows[1:-1]
    if header.get('version') != 1 or header.get('clock') != 'CLOCK_MONOTONIC':
        raise ValueError(f'{path}: unsupported format/clock')
    if len(events) != footer['count'] or any(e.get('type') != 'event' or e.get('event_seq') != i+1 for i,e in enumerate(events)):
        raise ValueError(f'{path}: event sequence/count mismatch')
    for event in events:
        if not header['start_ns'] <= event['mono_ns'] < header['end_ns']:
            raise ValueError(f'{path}: event outside control window')
        event.update(pid=header['pid'], role=header['role'])
    return header, footer, events


def unique_index(events, key):
    groups = defaultdict(list)
    for event in events:
        groups[key(event)].append(event)
    return {identity: rows[0] for identity, rows in groups.items() if len(rows) == 1}


def pair_spans(events):
    pending, output, issues = {}, [], []
    def issue(event, reason):
        issues.append(dict(pid=event['pid'], tid=event['tid'], role=event['role'],
                           attempt=event['attempt'], client=event['client'],
                           stage=event['stage'], mono_ns=event['mono_ns'], reason=reason))
    for event in sorted(events, key=lambda e: (e['mono_ns'], e['event_seq'])):
        stage = event['stage']
        if not stage.endswith(('_begin', '_end')):
            continue
        scope, edge = stage.rsplit('_', 1)
        if scope not in SPAN_STAGES:
            issue(event, 'unknown scope')
            continue
        # attempt binds SHM/frame scopes before an output AHB has been acquired.
        identity = ('attempt', event['attempt']) if event['attempt'] else ('frame', event['key'], event['frame'])
        has_local_identity = scope in UPLOAD_QUEUE_CHILD_STAGES + BROKER_STAGES or scope in ('wait', 'vk_submit') or (scope in SHM_CHILD_STAGES and scope != 'shm_copy')
        discriminator = (event.get('scope',0), event['object'], event['value']) if has_local_identity else ()
        key = (event['pid'], event['tid'], scope, identity, discriminator)
        if edge == 'begin':
            if key in pending:
                issue(event, 'duplicate begin; ambiguous pair discarded')
                pending[key] = None
            else:
                pending[key] = event
        elif key not in pending:
            issue(event, 'unpaired end')
        else:
            begin = pending.pop(key)
            if begin is None:
                continue
            if begin['client'] != event['client']:
                issue(event, 'client identity changed inside scope')
                continue
            if any(begin[field] and event[field] and begin[field] != event[field] for field in ('key','frame')):
                issue(event, 'known frame identity changed inside scope')
                continue
            wall = event['mono_ns'] - begin['mono_ns']
            cpu = event['cpu_ns'] - begin['cpu_ns'] if event['cpu_ns'] and begin['cpu_ns'] else None
            if cpu is not None and not 0 <= cpu <= wall:
                issue(event, 'invalid CPU samples; wall span retained')
                cpu = None
            output.append(dict(role=event['role'], pid=event['pid'], tid=event['tid'], stage=scope,
                               client=begin['client'], attempt=begin['attempt'],
                               key=begin['key'], frame=begin['frame'], scope=begin.get('scope',0),
                               start_ns=begin['mono_ns'], end_ns=event['mono_ns'], wall_ns=wall,
                               cpu_ns=cpu, off_cpu_ns=wall-cpu if cpu is not None else None,
                               status=event['status'], object=event['object'], value=event['value']))
    for begin in pending.values():
        if begin is not None:
            issue(begin, 'unpaired begin')
    return output, issues


def spans(events):
    return pair_spans(events)[0]


def interval_union_ns(spans):
    """Union on one thread's monotonic axis, never a sum of nested durations."""
    end, total = None, 0
    for span in sorted(spans, key=lambda s: (s['start_ns'], s['end_ns'])):
        start, stop = span['start_ns'], span['end_ns']
        total += max(0, stop - max(start, end if end is not None else start))
        end = max(stop, end if end is not None else stop)
    return total


def shm_decomposition(frames, issues, clean, all_spans=None):
    """Same-attempt, same-thread interval coverage; gaps retain no attribution."""
    imports = []
    for frame in frames:
        parents = [s for s in frame['spans'] if s['role']=='gamescope' and s['stage']=='shm_import']
        for parent in parents:
            related = [s for s in (all_spans if all_spans is not None else frame['spans'])
                       if s['role']=='gamescope' and s['stage'] in SHM_CHILD_STAGES
                       and s['pid']==parent['pid'] and s['attempt']==parent['attempt']]
            children = [s for s in related if parent['start_ns'] <= s['start_ns'] <= s['end_ns'] <= parent['end_ns']]
            reasons = []
            if not clean:
                reasons.append('capture incomplete, lossy or late-armed')
            if len(parents) != 1:
                reasons.append('multiple import parents for this attempt')
            if parent['client'] != frame['client']:
                reasons.append('import client differs from successful publication')
            if any(s['tid']!=parent['tid'] or s['client']!=parent['client'] for s in related):
                reasons.append('child thread/client differs from import')
            if len(children) != len(related):
                reasons.append('child crosses import boundary')
            missing = sorted(SHM_REQUIRED_STAGES - {s['stage'] for s in children})
            if missing:
                reasons.append('missing required decomposition scopes: ' + ', '.join(missing))
            if parent['status'] or any(s['status'] for s in children):
                reasons.append('scope reports failure')
            if any(i['pid']==parent['pid'] and i['attempt']==parent['attempt']
                   and i['stage'].rsplit('_',1)[0] in set(SHM_CHILD_STAGES) | {'shm_import'}
                   and not i['reason'].startswith('invalid CPU') for i in issues):
                reasons.append('malformed/unpaired SHM scope for this attempt')
            # Proper nesting or disjoint intervals are safe. Crossing scopes
            # cannot represent the documented sequential/nested call boundaries.
            ordered = sorted(children, key=lambda s:(s['start_ns'], -s['end_ns']))
            stack = []
            for child in ordered:
                while stack and child['start_ns'] >= stack[-1]['end_ns']:
                    stack.pop()
                if stack and child['end_ns'] > stack[-1]['end_ns']:
                    reasons.append('crossing child scopes; remainder ambiguous')
                    break
                stack.append(child)
            covered = interval_union_ns(children) if not reasons else None
            imports.append(dict(client=frame['client'], attempt=frame['attempt'], key=frame['key'], frame=frame['frame'],
                                start_ns=parent['start_ns'], end_ns=parent['end_ns'], total_ns=parent['wall_ns'],
                                stage_inclusive_ns={stage:interval_union_ns([s for s in children if s['stage']==stage])
                                                    for stage in SHM_CHILD_STAGES},
                                stage_span_counts=dict(Counter(s['stage'] for s in children)),
                                covered_ns=covered, unattributed_ns=parent['wall_ns']-covered if covered is not None else None,
                                remainder_unavailable_reasons=reasons))
    valid = [row for row in imports if row['unattributed_ns'] is not None]
    mean_ms = lambda values: statistics.mean(values)/1e6 if values else None
    return dict(count=len(imports), total_import_mean_ms=mean_ms([r['total_ns'] for r in imports]),
                stage_inclusive_mean_ms={stage:mean_ms([r['stage_inclusive_ns'][stage] for r in valid])
                                         for stage in SHM_CHILD_STAGES},
                stage_span_counts=dict(sum((Counter(r['stage_span_counts']) for r in valid), Counter())),
                observed_stage_span_counts=dict(sum((Counter(r['stage_span_counts']) for r in imports), Counter())),
                remainder_samples=len(valid), ambiguous_imports=len(imports)-len(valid),
                paired_total_import_mean_ms=mean_ms([r['total_ns'] for r in valid]),
                covered_import_mean_ms=mean_ms([r['covered_ns'] for r in valid]),
                unattributed_inside_import_mean_ms=mean_ms([r['unattributed_ns'] for r in valid]),
                per_import=imports,
                method='same-attempt/thread nested or disjoint child interval union; inclusive stage means overlap and must not be added; absent optional scopes contribute zero on valid imports; no GPU execution timing')


def bind_proxy_native(events, joined, game_pub):
    """Map namespaces by published key/fence AND semaphore/value, never proximity.

    Bulk operations carry no frame/timeline in the wire protocol. Only bind a
    complete native span contained by exactly one synchronous client RPC with
    the SAME mapped namespace/memory ID/offset. Containment is a causal bound,
    not a nearest timestamp match; partial/duplicate/missing evidence stays raw.
    """
    game = [e for e in events if e['role']=='gamescope']
    native = [e for e in events if e['role']=='android']
    links = [e for e in game if e['stage']=='proxy_submit_link']
    anchors = defaultdict(set)
    native_submits = defaultdict(list)
    for event in native:
        if event['stage']=='vk_submit_end' and event['status']==0:
            native_submits[(event['key'],event['frame'],event['fence'],event['object'],event['value'])].append(event)
    for link in links:
        pub = game_pub.get((link['key'],link['frame']))
        if not link['scope'] or (link['key'],link['frame']) not in joined or not pub or link['fence']!=pub['fence']:
            continue
        matches = native_submits.get((link['key'],link['frame'],link['fence'],link['object'],link['value']), [])
        if len(matches)==1 and matches[0]['scope']:
            anchors[link['scope']].add(matches[0]['scope'])
    reverse = Counter(scope for values in anchors.values() for scope in values)
    mapping = {local:next(iter(values)) for local,values in anchors.items()
               if len(values)==1 and reverse[next(iter(values))]==1}
    exact = defaultdict(list)
    for link in links:
        if link['scope'] in mapping:
            exact[(mapping[link['scope']],link['object'],link['value'])].append(link)
    native_spans, _ = pair_spans(native)
    client_spans, _ = pair_spans(game)
    bulks = [s for s in client_spans if s['stage']=='proxy_bulk_rpc' and s['scope'] in mapping]
    submit_rpcs, bulk_rpcs, native_edges, publications = (defaultdict(list) for _ in range(4))
    for span in client_spans:
        if span['stage']=='proxy_submit_rpc':
            submit_rpcs[(span['attempt'],span['scope'],span['object'],span['value'])].append(span)
    for span in bulks:
        bulk_rpcs[(mapping[span['scope']],span['object'],span['value'])].append(span)
    for event in native:
        native_edges[(event['tid'],event['stage'],event.get('scope',0),event['object'],event['value'])].append(event)
    for identity, pub in game_pub.items():
        if identity in joined: publications[(pub['attempt'],pub['client'])].append(pub)
    unresolved = 0
    last_dispatch, pending_receive = {}, {}
    bindings = []
    for span in sorted(native_spans, key=lambda s:(s["start_ns"], -s["end_ns"])):
        if span['stage'] not in BROKER_STAGES + ('vk_submit',):
            continue
        thread = (span['pid'],span['tid'])
        if span['stage']=='broker_receive':
            if thread in pending_receive: unresolved += 1
            pending_receive[thread] = span
            continue
        candidates = exact.get((span['scope'],span['object'],span['value']), [])
        # memory writes and submits use globally distinct resource IDs within
        # one broker device. Require complete containment for either match.
        candidates = [link for link in candidates
                      if any(s['start_ns']<=span['start_ns']<=span['end_ns']<=s['end_ns']
                             for s in submit_rpcs[(link['attempt'],link['scope'],link['object'],link['value'])])]
        bulk_candidates = [s for s in bulk_rpcs[(span['scope'],span['object'],span['value'])]
                           if s['start_ns']<=span['start_ns']<=span['end_ns']<=s['end_ns']]
        targets = [(c['attempt'],c['client']) for c in candidates+bulk_candidates]
        identity_source = 'published namespace anchor + exact resource/value + unique synchronous RPC containment'
        previous = last_dispatch.get(thread)
        if span['stage']=='broker_reply' and not targets and previous:
            dispatch, target = previous
            if (dispatch['end_ns'] <= span['start_ns'] and
                (dispatch['scope'],dispatch['object'],dispatch['value']) ==
                (span['scope'],span['object'],span['value'])):
                targets = [target]
                identity_source = 'published namespace anchor + immediately preceding decoded same-thread request dispatch'
        if span['stage']=='broker_reply':
            last_dispatch.pop(thread, None)  # one reply per decoded request
        receive = pending_receive.pop(thread, None) if span['stage']=='broker_dispatch' else None
        if span['stage']=='broker_dispatch':
            last_dispatch.pop(thread, None)
            if len(targets)==1: last_dispatch[thread] = (span, targets[0])
        if len(targets)!=1:
            unresolved += 1 + int(receive is not None)
            continue
        bindings.append((span, targets[0], identity_source))
        if receive:
            if receive['scope']==span['scope'] and receive['end_ns']<=span['start_ns']:
                bindings.append((receive, targets[0], 'unique normal device namespace + immediately following decoded same-thread request dispatch'))
            else:
                unresolved += 1
    unresolved += len(pending_receive)
    for span, (attempt, client), identity_source in bindings:
        pubs = publications[(attempt,client)]
        if len(pubs)!=1:
            unresolved += 1
            continue
        pub = pubs[0]
        edges = sum((native_edges[(span['tid'],span['stage']+edge,span['scope'],span['object'],span['value'])] for edge in ('_begin','_end')), [])
        for event in edges:
            if span['start_ns']<=event['mono_ns']<=span['end_ns']:
                event['key'],event['frame'] = pub['key'],pub['frame']
                # Keep native attempt/client zero: these belong to Gamescope.
                event['identity_source'] = identity_source
    return dict(connection_scope_map=mapping, unresolved_native_spans=unresolved,
                method='no wire changes; key/fence/timeline namespace anchors; unique complete synchronous RPC containment; no nearest-time matching')


def upload_queue_decomposition(frames, issues, clean, all_spans):
    rows = []
    for frame in frames:
        for parent in frame['spans']:
            if parent['role']!='gamescope' or parent['stage']!='upload_queue':
                continue
            children = [s for s in all_spans if s['role']=='gamescope' and s['stage'] in UPLOAD_QUEUE_CHILD_STAGES
                        and s['pid']==parent['pid'] and s['attempt']==parent['attempt']
                        and parent['start_ns']<=s['start_ns']<=s['end_ns']<=parent['end_ns']]
            reasons = []
            if not clean: reasons.append('incomplete/lossy/late-armed capture')
            required = {'proxy_prepare','proxy_lock','proxy_mirror','proxy_submit_pack','proxy_submit_rpc','proxy_finalize','rpc_exchange'}
            if not required <= {s['stage'] for s in children}:
                reasons.append('detail not sampled (attempt % 8 != 0), old build or missing scopes')
            if any(s['client']!=parent['client'] or s['tid']!=parent['tid'] for s in children):
                reasons.append('different child client/thread')
            if any(i['pid']==parent['pid'] and i['attempt']==parent['attempt']
                   and i['stage'].rsplit('_',1)[0] in set(UPLOAD_QUEUE_CHILD_STAGES)|{'upload_queue'}
                   and parent['start_ns']<=i['mono_ns']<=parent['end_ns']
                   and not i['reason'].startswith('invalid CPU') for i in issues):
                reasons.append('malformed/unpaired upload queue scope')
            # Only local same-thread coverage can be subtracted. Broker work is
            # concurrent with client receive waits and MUST NOT be added here.
            ordered=sorted(children,key=lambda s:(s['start_ns'],-s['end_ns']))
            stack=[]
            for child in ordered:
                while stack and child['start_ns']>=stack[-1]['end_ns']: stack.pop()
                if stack and child['end_ns']>stack[-1]['end_ns']:
                    reasons.append('crossing child scopes'); break
                stack.append(child)
            if parent['status'] or any(s['status'] for s in children): reasons.append('scope failure')
            crossing = [s for s in all_spans if s['role']=='gamescope' and s['stage'] in UPLOAD_QUEUE_CHILD_STAGES
                        and s['pid']==parent['pid'] and s['attempt']==parent['attempt']
                        and s['start_ns']<parent['end_ns'] and s['end_ns']>parent['start_ns'] and s not in children]
            if crossing: reasons.append('child crosses queue boundary')
            covered=interval_union_ns(children) if not reasons else None
            broker = [s for s in frame['spans'] if s['role']=='android' and s['stage'] in BROKER_STAGES+('vk_submit',)
                      and parent['start_ns']<=s['start_ns']<=s['end_ns']<=parent['end_ns']]
            rpcs = [s for s in children if s['stage'] in ('proxy_bulk_rpc','proxy_submit_rpc')]
            service = [s for s in broker if any(rpc['start_ns']<=s['start_ns']<=s['end_ns']<=rpc['end_ns'] for rpc in rpcs)]
            # Excludes reply instrumentation which races the client's final read;
            # that race stays visible in the separately reported broker stages.
            rpc_coverage = interval_union_ns(rpcs)
            complete_broker = bool(rpcs) and all(len([b for b in service if b['stage']=='broker_dispatch'
                and rpc['start_ns']<=b['start_ns']<=b['end_ns']<=rpc['end_ns']
                and (rpc['object'],rpc['value'])==(b['object'],b['value'])])==1 for rpc in rpcs)
            broker_coverage = interval_union_ns(service) if complete_broker else None
            submit = [s for s in children if s['stage']=='proxy_submit_rpc']
            exchange = [s for s in children if s['stage']=='rpc_exchange' and
                        any(rpc['start_ns']<=s['start_ns']<=s['end_ns']<=rpc['end_ns'] for rpc in submit)]
            first_submit = min((s['start_ns'] for s in exchange), default=None)
            rows.append(dict(attempt=parent['attempt'],client=parent['client'],key=frame['key'],frame=frame['frame'],
                             start_ns=parent['start_ns'],end_ns=parent['end_ns'],total_ns=parent['wall_ns'],
                             stage_inclusive_ns={name:interval_union_ns([s for s in children if s['stage']==name]) for name in UPLOAD_QUEUE_CHILD_STAGES},
                             covered_ns=covered,unattributed_ns=parent['wall_ns']-covered if covered is not None else None,
                             before_submit_rpc_ns=first_submit-parent['start_ns'] if first_submit is not None else None,
                             broker_attribution_complete=complete_broker,
                             broker_stage_span_counts=dict(Counter(s['stage'] for s in broker)),
                             broker_stage_inclusive_ns={name:interval_union_ns([s for s in broker if s['stage']==name]) if complete_broker and any(s['stage']==name for s in broker) else None for name in BROKER_STAGES+('vk_submit',)},
                             rpc_wall_ns=rpc_coverage,observed_broker_inside_rpc_ns=broker_coverage,
                             rpc_outside_observed_broker_ns=rpc_coverage-broker_coverage if complete_broker else None,
                             remainder_unavailable_reasons=reasons))
    valid=[r for r in rows if r['unattributed_ns'] is not None]
    mean=lambda values:statistics.mean(values)/1e6 if values else None
    broker_valid=[r for r in valid if r['broker_attribution_complete']]
    return dict(count=len(rows),remainder_samples=len(valid),unavailable_queues=len(rows)-len(valid),
                total_queue_mean_ms=mean([r['total_ns'] for r in rows]),
                paired_total_queue_mean_ms=mean([r['total_ns'] for r in valid]),
                stage_inclusive_mean_ms={name:mean([r['stage_inclusive_ns'][name] for r in valid]) for name in UPLOAD_QUEUE_CHILD_STAGES},
                covered_queue_mean_ms=mean([r['covered_ns'] for r in valid]),
                before_submit_rpc_mean_ms=mean([r['before_submit_rpc_ns'] for r in valid if r['before_submit_rpc_ns'] is not None]),
                broker_complete_samples=len(broker_valid),
                paired_rpc_wall_mean_ms=mean([r['rpc_wall_ns'] for r in broker_valid]),
                broker_stage_sample_counts={name:sum(r['broker_stage_inclusive_ns'][name] is not None for r in broker_valid) for name in BROKER_STAGES+('vk_submit',)},
                broker_stage_inclusive_mean_ms={name:mean([r['broker_stage_inclusive_ns'][name] for r in broker_valid if r['broker_stage_inclusive_ns'][name] is not None]) for name in BROKER_STAGES+('vk_submit',)},
                rpc_wall_mean_ms=mean([r['rpc_wall_ns'] for r in valid]),
                observed_broker_inside_rpc_mean_ms=mean([r['observed_broker_inside_rpc_ns'] for r in broker_valid]),
                rpc_outside_observed_broker_mean_ms=mean([r['rpc_outside_observed_broker_ns'] for r in broker_valid]),
                unattributed_inside_queue_mean_ms=mean([r['unattributed_ns'] for r in valid]),per_queue=rows,
                method='attempt % 8 detailed sample; same-thread interval union only; inclusive means overlap; broker spans are concurrent and excluded from subtraction; not GPU execution time')


def analyze(paths, window=None):
    captures = [load(path) for path in paths]
    if len(captures) != 2 or sorted(h['role'] for h,_,_ in captures) != ['android','gamescope']:
        raise ValueError('need exactly one Gamescope and one Android journal')
    headers = [h for h,_,_ in captures]
    if len({h['pid'] for h in headers}) != 2:
        raise ValueError('Gamescope and Android journals need different process identities')
    if len({(h['run'],h['start_ns'],h['end_ns']) for h in headers}) != 1:
        raise ValueError('journals are from different runs/windows')
    if window is None:
        window = (headers[0]['start_ns'], headers[0]['end_ns'])
    if not all(h['start_ns'] <= window[0] < window[1] <= h['end_ns'] for h in headers):
        raise ValueError('journal does not cover the external sample window')
    events = [event for _,_,rows in captures for event in rows]
    for event in events:
        event['within_sample'] = window[0] <= event['mono_ns'] < window[1]
    game = [e for e in events if e['role'] == 'gamescope']
    native = [e for e in events if e['role'] == 'android']
    pubs = lambda rows: [e for e in rows if e['stage'] == 'publish' and e['key'] and e['frame'] and e['status'] == 0]
    identity = lambda e: (e['key'],e['frame'])
    game_pub = unique_index(pubs(game), identity)
    native_pub = unique_index(pubs(native), identity)
    joined = {key for key in game_pub.keys() & native_pub.keys()
              if (game_pub[key]['sync'],game_pub[key]['fence']) == (native_pub[key]['sync'],native_pub[key]['fence'])
              and game_pub[key]['sync'] and game_pub[key]['fence']}
    producer = unique_index([native_pub[key] for key in joined], lambda e:(e['key'],e['fence']))
    # The native submit precedes publication and has no frame yet. Bind only a
    # unique (key,fence) which later published successfully in THIS process/run.
    submits = []
    for event in native:
        if event['stage'].startswith('vk_submit_') and event['frame'] == 0:
            pub = producer.get((event['key'],event['fence']))
            if pub and event['mono_ns'] <= pub['mono_ns']:
                event['frame'] = pub['frame']
                event['identity_source'] = 'unique successful publish of key/fence'
                if event['stage'] == 'vk_submit_end' and event['status'] == 0:
                    submits.append(event)
    timeline = unique_index(submits, lambda e:(e.get('scope',0),e['object'],e['value']))
    for event in native:
        if event['stage'].startswith('wait_') and not event['frame']:
            submit = timeline.get((event.get('scope',0),event['object'],event['value'])) if event.get('scope') else None
            if submit and event['mono_ns'] >= submit['mono_ns']:
                event['key'],event['frame'] = submit['key'],submit['frame']
                event['identity_source'] = 'same native device namespace/timeline semaphore/value as bound submit'
    proxy_correlation = bind_proxy_native(events, joined, game_pub)
    all_spans, span_issues = pair_spans(events)
    commits = unique_index([e for e in game if e['stage'] == 'client_commit'], lambda e:e['client'])
    frames, durations = [], []
    for key in sorted(joined):
        attempt = game_pub[key]['attempt']
        rows = [e for e in events if (e['role'] == 'gamescope' and e['attempt'] and e['attempt'] == attempt)
                or (e['role'] == 'android' and identity(e) == key)]
        rows.sort(key=lambda e:e['mono_ns'])
        commit = commits.get(game_pub[key]['client'])
        frame_spans = [dict(s, joined_key=key[0], joined_frame=key[1]) for s in all_spans
                       if ((s['role']=='gamescope' and attempt and s['attempt']==attempt)
                           or (s['role']=='android' and (s['key'],s['frame'])==key))
                       and window[0] <= s['start_ns'] <= s['end_ns'] < window[1]]
        durations += frame_spans
        latch = [e['value'] for e in rows if e['stage']=='android_latch_time' and e['status']==0 and 0 < e['value'] <= e['mono_ns']]
        lifecycle_stages = {'ahb_acquire_end','outer_commit','android_submit','pool_return','outer_release'}
        inside = {e['stage'] for e in rows if e['within_sample'] and e['status']==0}
        frames.append(dict(ownership_lifecycle_inside_sample=lifecycle_stages <= inside, key=key[0],frame=key[1],attempt=attempt,client=game_pub[key]['client'],
                           client_commit=commit, events=rows, spans=frame_spans, latch_ns=latch,
                           physical_present_ns=None, pool_return_observed=any(e['stage']=='pool_return' for e in rows)))
    counts = Counter(e['role']+'/'+e['stage'] for e in events if window[0] <= e['mono_ns'] < window[1])
    summaries = {}
    for scope in sorted({s['role']+'/'+s['stage'] for s in durations}):
        subset = [s for s in durations if s['role']+'/'+s['stage']==scope]
        paired = [s for s in subset if s['cpu_ns'] is not None]
        cpu = [s['cpu_ns'] for s in paired]
        wall = sorted(s['wall_ns'] for s in subset)
        summaries[scope] = dict(count=len(wall), wall_mean_ms=statistics.mean(wall)/1e6,
                               wall_p95_ms=wall[min(len(wall)-1,int(len(wall)*.95))]/1e6,
                               cpu_samples=len(cpu),cpu_mean_ms=statistics.mean(cpu)/1e6 if cpu else None,
                               paired_wall_mean_ms=statistics.mean(s['wall_ns'] for s in paired)/1e6 if paired else None,
                               off_cpu_mean_ms=statistics.mean(s['off_cpu_ns'] for s in paired)/1e6 if paired else None)
    in_window = [e for e in events if window[0] <= e['mono_ns'] < window[1]]
    def identities(role, stage, status=0):
        return {(e['key'],e['frame']) for e in in_window if e['role']==role and e['stage']==stage
                and e['status']==status and e['key'] and e['frame']}
    successful = dict(gamescope_outer_commits=len(identities('gamescope','outer_commit')),
                      android_new_submits=len(identities('android','android_submit')),
                      android_pool_returns=len(identities('android','pool_return')),
                      android_reported_latches=len({(e['key'],e['frame']) for e in native
                        if e['stage']=='android_latch_time' and e['status']==0 and window[0] <= e['value'] < window[1]}))
    client_counts = Counter()
    window_counts = defaultdict(Counter)
    for event in in_window:
        if event['role']=='gamescope' and event['stage']=='client_commit':
            category=('selected_' if event['status'] else 'unselected_')+('mapped' if event['value'] else 'unmapped')
            client_counts[category]+=1
            window_counts[str(event['object'])][category]+=1
    quality = [dict(role=h['role'],pid=h['pid'],**f,ready_before_start=h.get('ready_ns',h['end_ns'])<=h['start_ns']) for h,f,_ in captures]
    clean = all(q['complete'] and q['dropped']==0 and q['ready_before_start'] for q in quality)
    return dict(run=headers[0]['run'], window_ns=window, sample_seconds=(window[1]-window[0])/1e9,
                capture_quality=quality, lossless_complete_window=clean,
                observed_client_commit_counts=client_counts, window_commit_counts=dict(window_counts),
                observed_successful_frame_counts=successful, successful_rates_per_second={k:v/((window[1]-window[0])/1e9) for k,v in successful.items()},
                observed_counts=counts, rates_per_second={k:v/((window[1]-window[0])/1e9) for k,v in counts.items()},
                joined_frames=len(frames), frames=frames, stage_spans=summaries,
                proxy_native_correlation=proxy_correlation,
                upload_queue_decomposition=upload_queue_decomposition(frames, span_issues, clean, all_spans),
                span_diagnostics=span_issues, shm_import_decomposition=shm_decomposition(frames, span_issues, clean, all_spans),
                unmatched_publish_ids=[list(k) for k in sorted((game_pub.keys() | native_pub.keys())-joined)],
                interpretation='observations only; boundary frames/loss are explicit; nested scopes overlap, never sum them',
                coverage_gaps=['CEF/XDamage IDs are external and not equal to wlroots commit IDs',
                               'latch timestamp is not physical scanout; transaction callback time is delivery time',
                               'release fence timestamp is first successful readability observation, not kernel signal time',
                               'same-token Android repeats are application repeats, not physical repeated refreshes',
                               'native uploads require unique published namespace anchors and complete synchronous RPC containment; unmatched raw events retained',
                               'broker header wait has no request identity; body receive needs one normal device and adjacent decoded dispatch, otherwise remains unassigned',
                               'rpc_receive includes socket blocking and scheduling; exact scheduler wakeup delay is unavailable',
                               'proxy details sample every eighth attempt; off-CPU includes all descheduling, not lock contention alone',
                               'wait wall/CPU timestamps do not measure GPU execution or exact scheduler wakeup latency'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path,help='capture output or frame-journal directory')
    args=parser.parse_args()
    root=args.directory/'frame-journal' if (args.directory/'frame-journal').is_dir() else args.directory
    window=None
    # Direct frame-journal/ invocation must retain the external sample bounds,
    # rather than silently including trailing guard frames after affinity restore.
    capture_root = args.directory.parent if args.directory.name=='frame-journal' else args.directory
    report=capture_root/'guest-report.txt'
    if report.is_file():
        markers={line.split('=',1)[0]:json.loads(line.split('=',1)[1]) for line in report.read_text().splitlines()
                 if line.startswith(('PERF_SAMPLING_START=','PERF_SAMPLING_END='))}
        window=(markers['PERF_SAMPLING_START']['monotonic_ns'],markers['PERF_SAMPLING_END']['monotonic_ns'])
    print(json.dumps(analyze(sorted(root.glob('trace-*.jsonl')),window),indent=2))


if __name__ == '__main__':
    main()
