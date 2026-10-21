#!/usr/bin/env python3
"""Known-answer tests for the binary stack gate; no device or timing claims."""
import unittest
from unittest.mock import patch
from types import SimpleNamespace

from check_ui_stack import check_frames, demangle_clones, entry_frames


UI = '(anonymous namespace)::DeviceUiTask(void*)'
SUBMIT = 'device_ui_internal::RequestEpdUiRefresh(wqn::UiFrame const&)'
RENDER = UI + '::{lambda()#1}::operator()() const'
INITIAL = UI + '::{lambda()#2}::operator()() const'


class StackGateTest(unittest.TestCase):
    def good(self):
        return {UI: 1600, SUBMIT: 256, RENDER: 4512, INITIAL: 4416}

    def passes(self, frames):
        return all(passed for _label, passed, _detail in check_frames(frames))

    def test_good(self):
        self.assertTrue(self.passes(self.good()))

    def test_missing_all(self):
        self.assertFalse(self.passes({}))

    def test_missing_ui(self):
        frames = self.good()
        del frames[UI]
        self.assertFalse(self.passes(frames))

    def test_missing_submit(self):
        frames = self.good()
        del frames[SUBMIT]
        self.assertFalse(self.passes(frames))

    def test_one_render_only(self):
        frames = self.good()
        del frames[INITIAL]
        self.assertFalse(self.passes(frames))

    def test_old_ui_frame(self):
        frames = self.good()
        frames[UI] = 5744
        self.assertFalse(self.passes(frames))

    def test_old_submit_frame(self):
        frames = self.good()
        frames[SUBMIT] = 4416
        self.assertFalse(self.passes(frames))

    def test_old_binary(self):
        self.assertFalse(self.passes({UI: 5744, SUBMIT: 4416}))

    def test_render_exceeds_budget(self):
        frames = self.good()
        frames[RENDER] = 6160
        self.assertFalse(self.passes(frames))

    def test_ui_boundary(self):
        frames = self.good()
        frames[UI] = 2048
        self.assertTrue(self.passes(frames))
        frames[UI] = 2064
        self.assertFalse(self.passes(frames))

    def test_submit_boundary(self):
        frames = self.good()
        frames[SUBMIT] = 512
        self.assertTrue(self.passes(frames))
        frames[SUBMIT] = 528
        self.assertFalse(self.passes(frames))

    def test_decimal_and_hex_entries(self):
        text = ('preamble, not a function\n'
                '42010000 <' + UI + '>:\n'
                '42010000:  000000 entry a1, 0x640\n'
                '42011000 <' + SUBMIT + '>:\n'
                '42011000:  000000 entry a1, 256\n'
                '42012000 <' + RENDER + '>:\n'
                '42012000:  000000 entry a1, 4512\n'
                '42013000 <' + INITIAL + '>:\n'
                '42013000:  000000 entry a1, 0x1140\n')
        self.assertEqual(entry_frames(text), self.good())
        self.assertTrue(self.passes(entry_frames(text)))

    def test_no_entry_is_not_zero(self):
        self.assertEqual(entry_frames('42010000 <' + UI + '>:\n  nop\n'), {})

    def test_duplicate_symbol_keeps_worst(self):
        text = ('42010000 <' + UI + '>:\n entry a1, 5744\n'
                '42011000 <' + UI + '>:\n entry a1, 32\n')
        self.assertEqual(entry_frames(text)[UI], 5744)

    def test_gcc_clone_names(self):
        frames = self.good()
        del frames[RENDER]
        frames['_ZZN12_GLOBAL__N_112DeviceUiTaskEPvENKUlvE0_clEv$constprop$0'] = 4512
        with patch('check_ui_stack.subprocess.run', return_value=SimpleNamespace(stdout=RENDER + '\n')) as run:
            normalized = demangle_clones(frames, 'test-cxxfilt')
            self.assertEqual(run.call_args.kwargs['input'],
                             '_ZZN12_GLOBAL__N_112DeviceUiTaskEPvENKUlvE0_clEv\n')
            self.assertTrue(self.passes(normalized))

    def test_demangle_bad_count(self):
        with patch('check_ui_stack.subprocess.run', return_value=SimpleNamespace(stdout='')):
            with self.assertRaises(ValueError):
                demangle_clones({'_Ztest$constprop$0': 16}, 'test-cxxfilt')

    def test_demangle_collision_keeps_worst(self):
        with patch('check_ui_stack.subprocess.run', return_value=SimpleNamespace(stdout=UI + '\n')):
            self.assertEqual(demangle_clones({UI: 1536, '_Ztest$part$0': 5744}, 'test-cxxfilt')[UI], 5744)


if __name__ == '__main__':
    unittest.main()
