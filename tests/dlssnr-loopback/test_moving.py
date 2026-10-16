#!/usr/bin/env python3
"""Host checks for the moving GPU fixture's independent evidence analyzer."""
import importlib.util
from pathlib import Path
import unittest
import tempfile
import struct
import os
import signal
import subprocess
import sys
from types import SimpleNamespace
import run_moving

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('moving', HERE / 'analyze_moving.py')
moving = importlib.util.module_from_spec(spec) if spec else None
if spec and spec.loader and (HERE / 'analyze_moving.py').exists():
    spec.loader.exec_module(moving)


class MovingEvidence(unittest.TestCase):
    def test_nearest_rank_and_empty_rejection(self):
        self.assertTrue(hasattr(moving, 'percentile'), 'moving analyzer is not implemented')
        self.assertEqual(moving.percentile([4, 1, 3, 2], .5), 2)
        self.assertEqual(moving.percentile([4, 1, 3, 2], .95), 4)
        with self.assertRaises(ValueError):
            moving.percentile([], .5)
        for invalid in (-1., 0., float('nan')):
            with self.assertRaises(ValueError):
                moving.percentile([invalid], .5)

    def test_guide_oracle_uses_depth_and_previous_projection(self):
        self.assertTrue(hasattr(moving, 'check_guides'), 'moving guide oracle is not implemented')
        # Identity camera: stationary plane at device depth .5; previous model translates +.25 NDC.
        identity = [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.]
        prev = identity.copy()
        prev[12] = .25
        meta = dict(width=32, height=24, inverse=identity, previous=prev, jitter=[0., 0.])
        depth = [.5] * (32 * 24)
        motion = [v for _ in depth for v in (4., 0.)]
        result = moving.check_guides(meta, depth, motion)
        self.assertAlmostEqual(result['max_error_px'], 0., places=10)
        with self.assertRaisesRegex(ValueError, 'motion'):
            moving.check_guides(meta, depth, [0.] * len(motion))
        with self.assertRaisesRegex(ValueError, 'finite'):
            moving.check_guides(meta, depth, [float('nan')] * len(motion))

    def test_nonfinite_projection_metadata_is_rejected(self):
        identity = [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.]
        for key in ('inverse', 'previous', 'jitter'):
            for invalid in (float('nan'), float('inf'), -float('inf')):
                meta = dict(width=32, height=24, inverse=identity.copy(), previous=identity.copy(), jitter=[0., 0.])
                meta[key][0] = invalid
                with self.subTest(key=key, invalid=invalid), self.assertRaisesRegex(ValueError, 'finite'):
                    moving.check_guides(meta, [.5] * (32*24), [0.] * (32*24*2))

    def test_depth_and_jitter_are_part_of_motion_oracle(self):
        identity = [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.]
        previous = identity.copy()
        previous[8] = .25  # Independent X/Z shear gives two pixels of motion at depth .5.
        meta = dict(width=32, height=24, inverse=identity, previous=previous, jitter=[.125, -.25])
        moving.check_guides(meta, [.5] * 768, [v for _ in range(768) for v in (2., 0.)])
        with self.assertRaisesRegex(ValueError, 'motion'):
            moving.check_guides(meta, [.25] * 768, [v for _ in range(768) for v in (2., 0.)])

    def test_jitter_changes_inverse_projection_oracle(self):
        identity = [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.]
        previous = identity.copy()
        previous[0], previous[5] = 2., .5
        jx, jy = .25, -.375
        meta = dict(width=32, height=24, inverse=identity, previous=previous, jitter=[jx, jy])
        # Independent planar magnification formula; nonzero jitter changes the expected motion.
        motion = [v for y in range(24) for x in range(32)
                  for v in (x+.5-jx-16, -.5*(y+.5-jy-12))]
        moving.check_guides(meta, [.5] * 768, motion)
        with self.assertRaisesRegex(ValueError, 'motion'):
            moving.check_guides(dict(meta, jitter=[0., 0.]), [.5] * 768, motion)
        # Bilinear output edits are evaluated in de-jittered coordinates, independently of guide UVs.
        ramp = [v for y in range(4) for x in range(4) for v in (float(x), float(y), x+y, 1.)]
        self.assertEqual(moving.sample_rgb(ramp, 1.25, 1.5, 4, 4), [1.25, 1.5, 2.75])

    def test_raw_readback_rejects_truncation_and_nonfinite_values(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'image.raw'
            path.write_bytes(b'bad')
            with self.assertRaisesRegex(ValueError, 'incomplete'):
                moving.read_raw(path, 1, 2, 1)
            path.write_bytes(struct.pack('<2e', 0., float('nan')))
            with self.assertRaisesRegex(ValueError, 'nonfinite'):
                moving.read_raw(path, 1, 2, 1)

    @unittest.skipUnless(sys.platform == 'linux', 'Linux process-group isolation')
    def test_timeout_kills_fixture_descendants(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            code = ("import subprocess,sys,time; from pathlib import Path; "
                    "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']); "
                    "Path('child.pid').write_text(str(p.pid)); time.sleep(30)")
            with self.assertRaises(subprocess.TimeoutExpired):
                run_moving.execute([sys.executable, '-c', code], cwd=directory, env=dict(os.environ),
                                   log=directory / 'child.log', timeout=.5)
            pid = int((directory / 'child.pid').read_text())
            try:
                fields = Path(f'/proc/{pid}/stat').read_text().split()
            except FileNotFoundError:
                fields = None
            self.assertTrue(fields is None or fields[2] == 'Z', 'timed-out descendant still running')

    def test_stale_pass_reports_are_invalidated_before_setup_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('moving-report.json', 'provenance.json'):
                (root / name).write_text('{"old":"PASS"}')
            for name in run_moving.__dict__.get('TRIALS', ('off-a', 'on-a', 'on-b', 'off-b')):
                (root / name).mkdir()
                (root / name / 'old.raw').write_bytes(b'old')
            runner = SimpleNamespace(RUN=root)
            with self.assertRaisesRegex(RuntimeError, 'prerequisite'):
                run_moving.run_trials(runner, Path('/missing'), {}, Path('/missing'))
            self.assertFalse((root / 'moving-report.json').exists())
            self.assertFalse((root / 'provenance.json').exists())
            self.assertFalse(any(root.glob('*/old.raw')))

    def test_processing_disabled_is_rejected(self):
        self.assertTrue(hasattr(moving, 'processing_coverage'), 'moving processing gate is not implemented')
        self.assertGreater(moving.processing_coverage([0., 1.], [0.1, 1.], [0.1, 1.])['mae'], 0)
        with self.assertRaisesRegex(ValueError, 'processing'):
            moving.processing_coverage([0., 1.], [0., 1.], [0., 1.])
        with self.assertRaisesRegex(ValueError, 'repeat'):
            moving.processing_coverage([0., 1.], [0.1, 1.], [0.5, 1.])


if __name__ == '__main__':
    unittest.main()
