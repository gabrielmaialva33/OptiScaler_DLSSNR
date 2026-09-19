#!/usr/bin/env python3
"""Diff the NRID identity reports the harness and production each emit before CreateFeature(18).

Both sides call DlssNr::Identity::ReportAll, so the lines are produced by one implementation and any
difference between them is a difference in what the snippet was handed -- not in how it was
described. Lines whose value is an address or a handle are compared for presence, not for equality;
they differ every run by construction.
"""
import re
import sys
from pathlib import Path

VOLATILE = ('device.removedReason',)  # compared, but a difference here is a finding, not noise


def report(path):
    text = Path(path).read_text(errors='replace')
    lines = [m.group(1).strip() for m in re.finditer(r'NRID (.+)', text)]
    if not lines:
        raise SystemExit(f'{path}: no NRID lines. Was the build with the reporter installed?')
    return lines


def key(line):
    """What makes a line comparable to another.

    "module=<name> ngxExports=<n>" repeats, once per module, so the bare key collapses every module
    into one entry and reports the last -- which is how a five-module list first looked like a
    one-module list here. A repeating line is keyed by its identity, not by its field name.
    """
    if line.startswith('module='):
        return 'module:' + line.split()[0][len('module='):]
    return line.split('=', 1)[0] if '=' in line else line


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: compare_identity.py <harness log> <production log>')

    left, right = report(sys.argv[1]), report(sys.argv[2])
    lk = {key(l): l for l in left}
    rk = {key(l): l for l in right}

    same, differ, only = 0, [], []
    for k in sorted(set(lk) | set(rk)):
        if k not in lk or k not in rk:
            only.append((k, lk.get(k), rk.get(k)))
        elif lk[k] == rk[k]:
            same += 1
        else:
            differ.append((k, lk[k], rk[k]))

    print(f'identical: {same}')
    if differ:
        print(f'\ndifferent: {len(differ)}')
        for k, a, b in differ:
            print(f'  {k}\n    harness    {a}\n    production {b}')
    if only:
        print(f'\npresent on one side only: {len(only)}')
        for k, a, b in only:
            print(f'  {k}\n    harness    {a or "-"}\n    production {b or "-"}')
    if not differ and not only:
        print('\nthe two reports are identical: the difference is not in anything this reports')
    return 0


if __name__ == '__main__':
    sys.exit(main())
