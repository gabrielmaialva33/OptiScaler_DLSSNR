#!/usr/bin/env python3
"""Fail when a tracked file carries something personal: a home path, a LAN or tailnet address, a token.

The tree is public: the default branch is mirrored to origin minutes after each commit. A 2026-10-13
audit found three home paths in tracked text and nothing worse; this keeps it that way. Vendored code
(external/, OptiScaler/include/), binary files and this suite's own samples are not scanned. A line that must keep a match can
say so with the marker `privacy: allow`.
"""
from pathlib import Path
import re
import subprocess
import sys

repo = Path(__file__).resolve().parents[2]

PATTERNS = [
    ('home path', re.compile(r'/home/[a-z_][a-z0-9_.-]*/')),
    ('home path (Wine Z: drive)', re.compile(r'[Zz]:\\+home\\+[a-z_]', re.IGNORECASE)),
    ('Windows user profile', re.compile(r'[A-Za-z]:\\+Users\\+(?!Public\b|Default\b|<|%|\$|\{)[A-Za-z0-9]')),
    # Not inside a URL: a host name ends right before the slash there (media.githubusercontent.com/media/...).
    ('mounted library path', re.compile(r'(?<![\w.])/(?:mnt|run/media|media)/[A-Za-z0-9_-]+/[A-Za-z]')),
    ('tailnet address', re.compile(r'\b100\.(?:6[4-9]|[7-9]\d|1[01]\d|12[0-7])\.\d{1,3}\.\d{1,3}\b')),
    ('tailnet host name', re.compile(r'\b[a-z0-9-]+\.ts\.net\b')),
    ('LAN address', re.compile(r'\b192\.168\.\d{1,3}\.\d{1,3}\b')),
    ('GitHub token', re.compile(r'\b(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{30,}|\bgithub_pat_[A-Za-z0-9_]{20,}')),
    ('API key', re.compile(r'\bsk-(?:ant-)?[A-Za-z0-9_-]{20,}|\bAKIA[0-9A-Z]{16}\b|\bxox[abprs]-[A-Za-z0-9-]{10,}')),
    ('private key', re.compile(r'-----BEGIN (?:[A-Z]+ )?PRIVATE KEY-----')),
]

SKIP_PREFIXES = ('external/', 'OptiScaler/include/', 'retoque/include/', 'tests/repo-privacy/')


def tracked_files():
    out = subprocess.run(['git', 'ls-files', '-z'], cwd=repo, check=True, capture_output=True).stdout
    return [p for p in out.decode().split('\0') if p and not p.startswith(SKIP_PREFIXES)]


def text_of(path):
    data = (repo / path).read_bytes() if (repo / path).is_file() else b''
    if b'\0' in data[:8192]:
        return None
    return data.decode('utf-8', errors='replace')


def scan(files):
    hits = []
    for path in files:
        text = text_of(path)
        if text is None:
            continue
        for number, line in enumerate(text.splitlines(), 1):
            if 'privacy: allow' in line:
                continue
            for name, pattern in PATTERNS:
                if pattern.search(line):
                    hits.append(f'{path}:{number}: {name}')
    return hits


def self_test():
    # Each pattern must fire on a sample and stay quiet on its near miss, or the scan proves nothing.
    samples = {
        'home path': ('see /home/someone/.local/share', 'see ~/.local/share'),
        'home path (Wine Z: drive)': (r'Z:\home\someone\x', r'Z:$(pwd)'),
        'Windows user profile': (r'C:\Users\someone\AppData', r'C:\Users\%USERNAME%\AppData'),
        'mounted library path': ('/mnt/games/SteamLibrary', 'https://media.example.com/media/Org/Repo'),
        'tailnet address': ('ssh 100.101.102.103', 'version 100.0.1.2'),
        'tailnet host name': ('desk.tail1234.ts.net', 'tsnet'),
        'LAN address': ('192.168.1.20', '192.168'),
        'GitHub token': ('ghp_' + 'a' * 36, 'ghp_short'),
        'API key': ('AKIA' + 'A' * 16, 'AKIA'),
        'private key': ('-----BEGIN OPENSSH PRIVATE KEY-----', '-----BEGIN CERTIFICATE-----'),
    }
    patterns = dict(PATTERNS)
    bad = []
    for name, (hit, miss) in samples.items():
        if not patterns[name].search(hit):
            bad.append(f'self-test: {name} does not match {hit!r}')
        if patterns[name].search(miss):
            bad.append(f'self-test: {name} matches its near miss {miss!r}')
    if set(samples) != set(patterns):
        bad.append('self-test: every pattern needs a sample')
    return bad


failures = self_test()
files = tracked_files()
failures += scan(files)
for failure in failures:
    print(f'FAIL: {failure}')
if failures:
    sys.exit(1)
print(f'PASS: no personal path, address or token in {len(files)} tracked files')
