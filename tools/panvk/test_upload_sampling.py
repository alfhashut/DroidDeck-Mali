#!/usr/bin/env python3
"""Pure Python ordered-stream model + source contracts; no native compilation."""
from pathlib import Path
import re
import unittest
import analyze_frame_journal as journal

ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT/'tools/mali-vulkan/frame_trace.h').read_text()
STRIDE = int(re.search(r'#define DD_FT_UPLOAD_DETAIL_STRIDE (\d+)u',HEADER)[1])

class UploadSamplingTests(unittest.TestCase):
    def selected(self, completed):
        return completed % STRIDE == STRIDE-1

    def test_shared_every_sixteenth_rule(self):
        self.assertEqual(STRIDE,16)
        self.assertEqual(STRIDE,journal.UPLOAD_DETAIL_STRIDE)
        self.assertIn('completed % DD_FT_UPLOAD_DETAIL_STRIDE == DD_FT_UPLOAD_DETAIL_STRIDE - 1',HEADER)
        self.assertEqual([n+1 for n in range(64) if self.selected(n)],[16,32,48,64])

    def test_client_broker_same_request_identity_and_capture_phase(self):
        client=broker=0; selected=[]
        # R submits/chunks do not advance the shared next-U ordinal. Observe an
        # arbitrary sub-window, not a reset at the first recorded event.
        for ordinal in range(1,49):
            fixed_client=self.selected(client)
            for memory,offset in [(9,0),(9,524288),(10,0)]:
                self.assertEqual(fixed_client,self.selected(broker),(ordinal,memory,offset))
                if fixed_client and ordinal>=11:
                    selected.append((ordinal,memory,offset))
            # Existing nonzero-fence compositor R does not advance either side.
            self.assertEqual(fixed_client,self.selected(broker))
            self.assertEqual(client,broker)
            # Existing zero-fence U submit reply advances both counters.
            self.assertEqual(fixed_client,self.selected(broker))
            broker+=1;client+=1
        self.assertEqual({row[0] for row in selected},{16,32,48})
        self.assertEqual(len(selected),9)
        # A fresh successful normal begin resets both diagnostic phases.
        client=broker=0
        self.assertFalse(self.selected(client));self.assertEqual(client,broker)

    def test_fixed_pair_selection_survives_counter_transition(self):
        for completed in range(64):
            frozen=self.selected(completed)
            events=['begin'] if frozen else []
            completed+=1  # actual submit completes before final scope end
            if frozen: events.append('end')
            self.assertIn(events,([],['begin','end']))
        proxy=(ROOT/'tools/mali-vulkan/frame_trace_proxy.h').read_text()
        event_writer=proxy.split('static void proxy_trace(',1)[1].split('struct proxy_trace_guard',1)[0]
        self.assertIn('proxy_trace_sink && proxy_trace_detailed',event_writer)
        self.assertNotIn('%',event_writer)
        self.assertIn('dd_ft_upload_detail_selected(proxy_trace_completed)',proxy)
        broker=(ROOT/'app/src/main/cpp/malivulkan/system_broker.c').read_text()
        self.assertIn('broker_trace_detailed = dd_ft_upload_detail_selected(s->trace_upload_completed)',broker)
        reader=broker.split('static int broker_trace_read_request(',1)[1].split('static struct dd_ft_id broker_trace_request',1)[0]
        self.assertEqual(reader.count('if (trace.scope) dd_ft_event('),2)
        copy=(ROOT/'app/src/main/cpp/malivulkan/interop_commands.h').read_text()
        self.assertIn('const int trace_copy = broker_trace_detailed && d->normal.enabled',copy)
        self.assertEqual(copy.count('if (trace_copy) dd_ft_event('),2)

    def test_same_wire_counter_filter_and_reset_on_both_endpoints(self):
        for name in ['tools/mali-vulkan/icd_proxy.c','app/src/main/cpp/malivulkan/system_broker.c']:
            source=(ROOT/name).read_text()
            self.assertIn('op == MB_RENDERER_SUBMIT && request_bytes == 28 && !mb_get_u32(request + 12)',source)
            self.assertIn('op == MB_NORMAL_BEGIN',source)
            self.assertIn('trace_upload_completed',source)
        logical=(ROOT/'tools/mali-vulkan/device_icd.h').read_text()
        self.assertIn('__atomic_load_n',logical)  # setter is outside connection lock

    def test_essential_events_do_not_use_detail_gate(self):
        native=(ROOT/'app/src/main/cpp/malivulkan/renderer_commands.h').read_text()
        submit=native.split('DD_FT_SUBMIT_BEGIN',1)[1].split('DD_FT_SUBMIT_END',1)[0]
        self.assertNotIn('broker_trace_detailed',submit)
        self.assertNotIn('dd_ft_upload_detail_selected',submit)
        self.assertIn('if (d->normal.enabled) dd_ft_event(DD_FT_SUBMIT_BEGIN',native)
        self.assertIn('if (d->normal.enabled) dd_ft_event(DD_FT_SUBMIT_END',native)
        proxy=(ROOT/'tools/mali-vulkan/renderer_icd.h').read_text()
        link=proxy.split('proxy_trace_sink(DD_FT_PROXY_DETAIL_POLICY',1)[1].split('/* Persistently mapped',1)[0]
        self.assertIn('proxy_trace_sink(DD_FT_PROXY_SUBMIT_LINK',link)
        self.assertNotIn('if (proxy_trace_detailed)',link)

if __name__=='__main__': unittest.main()
