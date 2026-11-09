"""Adversarial checks for the required-PASS result contract (no network)."""
import copy
import os
from pathlib import Path
import tempfile
from unittest.mock import patch
import unittest
import run_nk04_matrix as matrix


def passing():
    events = []
    def emit(name):
        events.append(dict(Action='run', Test=name))
        for child in sorted(n for n in matrix.EXPECTED if n.rpartition('/')[0] == name):
            emit(child)
        events.append(dict(Action='pass', Test=name))
    for top in sorted(matrix.TOPS):
        emit(top)
    events.append(dict(Action='pass'))
    return events


class AcceptanceContract(unittest.TestCase):
    def test_hostile_go_environment(self):
        hostile = dict(GOPATH='/isolated/gopath', GOCACHE='/isolated/cache',
                       GOMODCACHE='/isolated/modules', GOARCH='wasm',
                       GOFLAGS='-run=NoSuchTest', GORACE='exitcode=0 log_path=/hidden',
                       GOENV='/saved/settings', GODEBUG='unwanted=1')
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, hostile):
            env = matrix.environment(Path(tmp))
        self.assertEqual(env['GORACE'], 'log_path=stderr exitcode=66')
        self.assertEqual(env['GOFLAGS'], '')
        self.assertEqual(env['GOENV'], 'off')
        self.assertEqual(env['GOPROXY'], 'off')
        self.assertNotIn('GOARCH', env)
        self.assertNotIn('GODEBUG', env)

    def test_complete_pass(self):
        self.assertEqual(matrix.classify(passing(), 0, 'PASS\n'), [])

    def test_missing_leaf(self):
        e = passing()
        leaf = 'TestNK04MatrixPacketBoundaries/tuic_stream/65535'
        e = [x for x in e if x.get('Test') != leaf]
        self.assertTrue(matrix.classify(e, 0, 'PASS\n'))

    def test_duplicate_leaf(self):
        e = passing()
        e[1:1] = copy.deepcopy(e[:2])
        self.assertTrue(matrix.classify(e, 0, 'PASS\n'))

    def test_unknown_test(self):
        e = passing()
        e[0:0] = [dict(Action='run', Test='TestUnexpected'), dict(Action='pass', Test='TestUnexpected')]
        self.assertTrue(matrix.classify(e, 0, 'PASS\n'))

    def test_skip_and_assertion_fail(self):
        for action in ('skip', 'fail'):
            e = passing()
            next(x for x in e if x['Action'] == 'pass')['Action'] = action
            with self.subTest(action=action):
                self.assertTrue(matrix.classify(e, 0, 'PASS\n'))

    def test_process_status_cannot_be_overridden_by_pass(self):
        for status in (1, 66, -9):
            with self.subTest(status=status):
                self.assertTrue(matrix.classify(passing(), status, 'PASS\n'))

    def test_raw_failure_after_package_pass(self):
        for diagnostic in ('WARNING: DATA RACE', 'panic: test panic', 'fatal error: test',
                           '--- FAIL: TestOther', '--- SKIP: TestOther'):
            with self.subTest(diagnostic=diagnostic):
                self.assertTrue(matrix.classify(passing(), 0, 'PASS\n' + diagnostic))

    def test_missing_package_pass(self):
        self.assertTrue(matrix.classify(passing()[:-1], 0, 'PASS\n'))

    def test_duplicate_package_pass(self):
        self.assertTrue(matrix.classify(passing() + [dict(Action='pass')], 0, 'PASS\n'))

    def test_reversed_lifecycle(self):
        self.assertTrue(matrix.classify(list(reversed(passing())), 0, 'PASS\n'))


if __name__ == '__main__':
    unittest.main()
