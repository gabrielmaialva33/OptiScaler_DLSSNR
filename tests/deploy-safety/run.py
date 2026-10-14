#!/usr/bin/env python3
"""tools/deploy-to-game.sh against fake game directories: what it refuses, what it records, what it undoes.

Each case builds a game directory in a temp dir with fake DLLs -- a few bytes carrying the strings the
script recognises builds by, never a real binary -- runs the real script on it, and checks the files,
the receipt and the exit status. A Python child that mmaps a fake DLL stands in for a running game.
No Wine, no game, no build: the script is always pointed at fake sources with --source-dir, so it can
never reach build-local.sh.
"""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / 'tools' / 'deploy-to-game.sh'
SHIPPED_INI = REPO / 'OptiScaler.ini'

# The markers mirror a real build: OptiScaler carries CheckWorkingMode's line and also names
# ReShade64.dll and the forwarder's exports, which is exactly what must not make it look like ReShade.
PE = b'MZ\x90\x00' + b'\x00' * 60


def opti(tag):
    return PE + b'OptiScaler working as %s\x00ReShade64.dll\x00dlssnr_call_create\x00' + tag.encode()


def forwarder(tag):
    return PE + b'dlssnr_call_create\x00dlssnr_abi_version\x00' + tag.encode()


RESHADE = PE + b"Initializing crosire's ReShade version '%s'\x00"
FOREIGN = PE + b'some other dxgi wrapper\x00'

# Exit codes the script documents.
REFUSED_RUNNING, REFUSED_FOREIGN, REFUSED_SECOND_PROXY = 3, 4, 5


class Failure(Exception):
    pass


def expect(condition, message):
    if not condition:
        raise Failure(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


last_run = []


def run(game, *args, src=None, env_extra=None, proxy=None):
    """The script as it is invoked by hand: options, the game directory, then the optional proxy name.
    --source-dir is added when given; the result is kept for the failure report."""
    env = {k: v for k, v in os.environ.items() if not k.startswith('RETOQUE_DEPLOY_')}
    env.update(env_extra or {})
    cmd = ['bash', str(SCRIPT)]
    if src is not None:
        cmd += ['--source-dir', str(src)]
    cmd += [*args, str(game)] + ([proxy] if proxy else [])
    result = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=60)
    last_run.append(result)
    return result


def deploy_ok(game, *args, src, proxy=None):
    result = run(game, *args, src=src, proxy=proxy)
    expect(result.returncode == 0, f'deploy {" ".join(args)} exited {result.returncode}')
    return result


def make_src(root, tag):
    src = root / f'src-{tag}'
    src.mkdir()
    (src / 'OptiScaler.dll').write_bytes(opti(tag))
    (src / 'nvngx.dll_dlssnr.dll').write_bytes(forwarder(tag))
    return src


def make_game(root, files=None):
    game = root / 'game'
    game.mkdir()
    for rel, data in (files or {}).items():
        (game / rel).parent.mkdir(parents=True, exist_ok=True)
        (game / rel).write_bytes(data)
    return game


def snapshot(root):
    """Every entry under root, hidden ones included: content hash and mode for files, the target for links."""
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        for name in dirnames + filenames:
            path = Path(dirpath, name)
            rel = str(path.relative_to(root))
            if path.is_symlink():
                out[rel] = ('link', os.readlink(path))
            elif path.is_dir():
                out[rel] = ('dir',)
            else:
                out[rel] = ('file', sha(path), path.stat().st_mode & 0o777)
    return out


def receipt(game):
    path = game / '.retoque' / 'receipt.txt'
    if not path.exists():
        return None
    r = {'status': None, 'files': {}, 'backups': [], 'renames': [], 'proxies': []}
    for line in path.read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        kind, *rest = line.split('\t')
        if kind == 'status':
            r['status'] = rest[0]
        elif kind == 'proxy':
            r['proxies'].append(rest[0])
        elif kind == 'file':
            r['files'][rest[1]] = rest[0]
        elif kind == 'backup':
            r['backups'].append(tuple(rest))
        elif kind == 'rename':
            r['renames'].append(tuple(rest))
    return r


def expect_clean_state_dir(game):
    """No temp file left behind, and nothing set aside next to the files (backups live in .retoque/)."""
    state = game / '.retoque'
    if state.exists():
        stray = [p.name for p in state.iterdir() if p.name.startswith(('incoming.', 'receipt.')) and p.name != 'receipt.txt']
        expect(not stray, f'temp files left in .retoque: {stray}')
    siblings = [p.name for p in game.iterdir() if p.suffix in ('.bak', '.orig', '.old') or '.bak' in p.name]
    expect(not siblings, f'sibling backup files: {siblings}')


def plugins_lines(text, key='loadreshade'):
    """The KEY lines inside [Plugins], as simpleini would find them."""
    section, hits = None, []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith('[') and stripped.endswith(']'):
            section = stripped[1:-1].strip().lower()
        elif section == 'plugins' and '=' in stripped and not stripped.startswith((';', '#')):
            if stripped.split('=', 1)[0].strip().lower() == key:
                hits.append(line)
    return hits


CASES = []


def case(fn):
    CASES.append(fn)
    return fn


@case
def clean_install(tmp):
    src = make_src(tmp, 'v1')
    game = make_game(tmp)
    deploy_ok(game, src=src)
    expect((game / 'dxgi.dll').read_bytes() == opti('v1'), 'dxgi.dll is not the source build')
    expect((game / 'OptiScaler.dll').read_bytes() == opti('v1'), 'OptiScaler.dll is not the source build')
    expect((game / 'nvngx.dll_dlssnr.dll').read_bytes() == forwarder('v1'), 'forwarder is not the source one')
    expect((game / 'OptiScaler.ini').read_bytes() == SHIPPED_INI.read_bytes(), 'ini is not the shipped one')
    r = receipt(game)
    expect(r and r['status'] == 'complete', f'receipt status {r and r["status"]}')
    expect(r['proxies'] == ['dxgi.dll'], f'receipt proxies {r["proxies"]}')
    expect(set(r['files']) == {'dxgi.dll', 'OptiScaler.dll', 'nvngx.dll_dlssnr.dll', 'OptiScaler.ini'},
           f'receipt files {sorted(r["files"])}')
    for rel, digest in r['files'].items():
        expect(sha(game / rel) == digest, f'receipt hash of {rel} does not match the file')
    expect(not r['backups'] and not r['renames'], 'a clean install set something aside')
    expect(not (game / '.retoque' / 'backup').exists(), 'a clean install made a backup directory')
    expect_clean_state_dir(game)


@case
def positional_proxy_and_bad_name(tmp):
    src = make_src(tmp, 'v1')
    game = make_game(tmp)
    deploy_ok(game, src=src, proxy='winmm.dll')
    expect((game / 'winmm.dll').read_bytes() == opti('v1'), 'winmm.dll not installed')
    expect(not (game / 'dxgi.dll').exists(), 'dxgi.dll written although the proxy was winmm.dll')
    expect(receipt(game)['proxies'] == ['winmm.dll'], 'receipt does not name winmm.dll')
    before = snapshot(game)
    result = run(game, src=src, proxy='dxgi')
    expect(result.returncode == 1, f'a proxy name without .dll exited {result.returncode}, expected 1')
    expect(snapshot(game) == before, 'a rejected proxy name changed the directory')


@case
def reinstall_over_own_build(tmp):
    game = make_game(tmp)
    deploy_ok(game, src=make_src(tmp, 'v1'))
    ini = game / 'OptiScaler.ini'
    ini.write_text(ini.read_text().replace('LogToFile=auto', 'LogToFile=true', 1))
    user_ini = ini.read_bytes()
    deploy_ok(game, src=make_src(tmp, 'v2'))
    expect((game / 'dxgi.dll').read_bytes() == opti('v2'), 'dxgi.dll not replaced by v2')
    expect((game / 'nvngx.dll_dlssnr.dll').read_bytes() == forwarder('v2'), 'forwarder not replaced by v2')
    expect(ini.read_bytes() == user_ini, 'the user-edited ini was not preserved')
    r = receipt(game)
    expect(r['status'] == 'complete', f'receipt status {r["status"]}')
    expect(r['files']['dxgi.dll'] == sha(game / 'dxgi.dll'), 'receipt still holds the v1 hash')
    expect(not r['backups'], 'our own earlier build was backed up as if it were foreign')
    expect(not (game / '.retoque' / 'backup').exists(), 'a backup directory appeared on a re-install')
    expect_clean_state_dir(game)


@case
def reinstall_identical_writes_nothing(tmp):
    src = make_src(tmp, 'v1')
    game = make_game(tmp)
    deploy_ok(game, src=src)
    inode = (game / 'dxgi.dll').stat().st_ino
    result = deploy_ok(game, src=src)
    expect((game / 'dxgi.dll').stat().st_ino == inode, 'an identical build was rewritten')
    expect('already this build' in result.stdout, 'no "already this build" note')


@case
def earlier_build_without_receipt(tmp):
    # What the old script left everywhere: an OptiScaler build and no receipt. Replaced without a flag,
    # but kept, so --uninstall puts the earlier build back.
    game = make_game(tmp, {'dxgi.dll': opti('old'), 'OptiScaler.dll': opti('old'),
                           'nvngx.dll_dlssnr.dll': forwarder('old')})
    before = snapshot(game)
    deploy_ok(game, src=make_src(tmp, 'v1'))
    expect((game / 'dxgi.dll').read_bytes() == opti('v1'), 'the earlier build was not replaced')
    r = receipt(game)
    backed = {rel for _, rel, _ in r['backups']}
    expect(backed == {'dxgi.dll', 'OptiScaler.dll', 'nvngx.dll_dlssnr.dll'}, f'backups {sorted(backed)}')
    for digest, rel, copy in r['backups']:
        expect(sha(game / copy) == digest == before[rel][1], f'backup of {rel} does not hold what was there')
        expect(copy.startswith('.retoque/backup/'), f'backup outside .retoque/backup: {copy}')
    expect_clean_state_dir(game)
    result = run(game, '--uninstall')
    expect(result.returncode == 0, f'uninstall exited {result.returncode}')
    after = snapshot(game)
    after.pop('OptiScaler.ini', None)  # installed by the deploy, removed again: compare the rest
    expect(after == before, f'uninstall did not restore the earlier build: {sorted(set(after) ^ set(before))}')


@case
def foreign_dxgi_refused(tmp):
    game = make_game(tmp, {'dxgi.dll': FOREIGN})
    before = snapshot(game)
    result = run(game, src=make_src(tmp, 'v1'))
    expect(result.returncode == REFUSED_FOREIGN, f'exited {result.returncode}, expected {REFUSED_FOREIGN}')
    expect('dxgi.dll is not ours' in result.stderr and '--replace-foreign' in result.stderr,
           'refusal does not name the file and the flag')
    expect(snapshot(game) == before, 'a refused deploy changed the directory')
    result = run(game, '--keep-reshade', src=tmp / 'src-v1')
    expect(result.returncode == REFUSED_FOREIGN, '--keep-reshade let a non-ReShade dxgi.dll through')
    expect('not ReShade' in result.stderr, 'refusal does not say the file is not ReShade')
    expect(snapshot(game) == before, 'a refused --keep-reshade deploy changed the directory')


@case
def replace_foreign_then_uninstall(tmp):
    game = make_game(tmp, {'dxgi.dll': FOREIGN, 'game.exe': PE})
    before = snapshot(game)
    deploy_ok(game, '--replace-foreign', src=make_src(tmp, 'v1'))
    expect((game / 'dxgi.dll').read_bytes() == opti('v1'), 'dxgi.dll not replaced')
    r = receipt(game)
    expect(len(r['backups']) == 1 and r['backups'][0][1] == 'dxgi.dll', f'backups {r["backups"]}')
    digest, _, copy = r['backups'][0]
    expect((game / copy).read_bytes() == FOREIGN and digest == sha(game / copy), 'backup does not hold the original')
    expect_clean_state_dir(game)
    result = run(game, '--uninstall')
    expect(result.returncode == 0, f'uninstall exited {result.returncode}')
    expect(snapshot(game) == before, 'uninstall did not return the directory to its state before the deploy')


@case
def reshade_refused_without_flag(tmp):
    game = make_game(tmp, {'dxgi.dll': RESHADE})
    before = snapshot(game)
    result = run(game, src=make_src(tmp, 'v1'))
    expect(result.returncode == REFUSED_FOREIGN, f'exited {result.returncode}, expected {REFUSED_FOREIGN}')
    expect('ReShade' in result.stderr and '--keep-reshade' in result.stderr, 'refusal does not offer --keep-reshade')
    expect(snapshot(game) == before, 'a refused deploy changed the directory')


@case
def keep_reshade_with_shipped_ini(tmp):
    game = make_game(tmp, {'dxgi.dll': RESHADE})
    before = snapshot(game)
    deploy_ok(game, '--keep-reshade', src=make_src(tmp, 'v1'))
    expect((game / 'ReShade64.dll').read_bytes() == RESHADE, 'ReShade64.dll is not the original ReShade')
    expect((game / 'dxgi.dll').read_bytes() == opti('v1'), 'dxgi.dll is not ours')
    shipped = SHIPPED_INI.read_text().splitlines()
    edited = (game / 'OptiScaler.ini').read_text().splitlines()
    expect(len(plugins_lines(SHIPPED_INI.read_text())) == 1, 'the shipped ini no longer has one [Plugins] LoadReshade')
    expect(len(edited) == len(shipped), f'ini went from {len(shipped)} to {len(edited)} lines: a key was inserted')
    changed = [(a, b) for a, b in zip(shipped, edited) if a != b]
    expect(len(changed) == 1 and changed[0][1].strip().lower() == 'loadreshade=true',
           f'ini changes: {changed}')
    expect(plugins_lines((game / 'OptiScaler.ini').read_text()) == ['LoadReshade=true'], 'not exactly one LoadReshade=true')
    r = receipt(game)
    expect(len(r['renames']) == 1 and r['renames'][0][1:] == ('dxgi.dll', 'ReShade64.dll'), f'renames {r["renames"]}')
    expect(r['files']['OptiScaler.ini'] == sha(game / 'OptiScaler.ini'), 'receipt holds the pre-edit ini hash')
    # A second deploy keeps it that way and adds nothing.
    deploy_ok(game, '--keep-reshade', src=make_src(tmp, 'v2'))
    expect(plugins_lines((game / 'OptiScaler.ini').read_text()) == ['LoadReshade=true'], 're-deploy touched the key')
    expect(len(receipt(game)['renames']) == 1, 're-deploy recorded a second rename')
    result = run(game, '--uninstall')
    expect(result.returncode == 0, f'uninstall exited {result.returncode}')
    expect(snapshot(game) == before, 'uninstall did not put ReShade back as dxgi.dll alone')


@case
def keep_reshade_edits_user_ini_in_place(tmp):
    # CRLF, the key twice in [Plugins] in other spellings, and the same key in another section.
    user_ini = (b'[Log]\r\nLogLevel=2\r\n\r\n[Plugins]\r\n; LoadReshade=false in a comment\r\n'
                b'loadreshade = auto\r\nLoadAsiPlugins=auto\r\nLoadReShade=false\r\n\r\n'
                b'[Other]\r\nLoadReshade=false\r\n')
    game = make_game(tmp, {'dxgi.dll': RESHADE, 'OptiScaler.ini': user_ini})
    before = snapshot(game)
    deploy_ok(game, '--keep-reshade', src=make_src(tmp, 'v1'))
    data = (game / 'OptiScaler.ini').read_bytes()
    lines = data.split(b'\r\n')
    expect(len(lines) == len(user_ini.split(b'\r\n')), 'line count changed: a key was inserted or a line lost')
    expect(data.count(b'\n') == data.count(b'\r\n'), 'CRLF line endings were not kept')
    text = data.decode()
    plugins = plugins_lines(text)
    expect(len(plugins) == 2 and all(l.split('=', 1)[1].strip() == 'true' for l in plugins),
           f'[Plugins] LoadReshade lines: {plugins}')
    expect('loadreshade =true' in text and 'LoadReShade=true' in text, 'the lines lost their own spelling')
    expect('[Other]\r\nLoadReshade=false' in text, 'the key in another section was changed')
    expect('; LoadReshade=false in a comment' in text, 'a comment was changed')
    r = receipt(game)
    ini_backups = [b for b in r['backups'] if b[1] == 'OptiScaler.ini']
    expect(len(ini_backups) == 1 and (game / ini_backups[0][2]).read_bytes() == user_ini,
           'the user ini was not backed up before the edit')
    result = run(game, '--uninstall')
    expect(result.returncode == 0, f'uninstall exited {result.returncode}')
    expect(snapshot(game) == before, 'uninstall did not restore the user ini and ReShade')


@case
def keep_reshade_adds_a_missing_key_once(tmp):
    game = make_game(tmp, {'dxgi.dll': RESHADE, 'OptiScaler.ini': b'[Plugins]\nLoadAsiPlugins=auto\n'})
    deploy_ok(game, '--keep-reshade', src=make_src(tmp, 'v1'))
    expect((game / 'OptiScaler.ini').read_text() == '[Plugins]\nLoadReshade=true\nLoadAsiPlugins=auto\n',
           f'got {(game / "OptiScaler.ini").read_text()!r}')
    other = tmp / 'other'
    other.mkdir()
    shutil.move(str(game), str(other / 'game'))
    game = make_game(tmp, {'dxgi.dll': RESHADE, 'OptiScaler.ini': b'[Log]\nLogLevel=2\n'})
    deploy_ok(game, '--keep-reshade', src=tmp / 'src-v1')
    expect((game / 'OptiScaler.ini').read_text() == '[Log]\nLogLevel=2\n\n[Plugins]\nLoadReshade=true\n',
           f'got {(game / "OptiScaler.ini").read_text()!r}')


@case
def keep_reshade_refused_when_slot_taken(tmp):
    src = make_src(tmp, 'v1')
    for taken in ('ReShade64.dll', 'd3d12-ReShade64.dll'):
        root = tmp / taken
        root.mkdir()
        game = make_game(root, {'dxgi.dll': RESHADE, taken: RESHADE + b'other'})
        before = snapshot(game)
        result = run(game, '--keep-reshade', src=src)
        expect(result.returncode == REFUSED_FOREIGN, f'{taken}: exited {result.returncode}')
        expect(taken in result.stderr, f'refusal does not name {taken}')
        expect(snapshot(game) == before, f'{taken}: a refused deploy changed the directory')


@case
def second_proxy_refused(tmp):
    src = make_src(tmp, 'v1')
    game = make_game(tmp, {'WINMM.DLL': opti('elsewhere')})
    before = snapshot(game)
    result = run(game, src=src)
    expect(result.returncode == REFUSED_SECOND_PROXY, f'exited {result.returncode}, expected {REFUSED_SECOND_PROXY}')
    expect('WINMM.DLL' in result.stderr and '--allow-second-proxy' in result.stderr, 'refusal does not name the file')
    expect(snapshot(game) == before, 'a refused deploy changed the directory')
    result = deploy_ok(game, '--allow-second-proxy', src=src)
    expect('also holds an OptiScaler build' in result.stdout, 'no warning when allowed')
    # The game's own winmm.dll (not OptiScaler) is no reason to refuse.
    other = tmp / 'own'
    other.mkdir()
    game = make_game(other, {'winmm.dll': PE + b'system winmm\x00'})
    deploy_ok(game, src=src)


@case
def case_variant_refused(tmp):
    game = make_game(tmp, {'DXGI.DLL': opti('old')})
    before = snapshot(game)
    result = run(game, src=make_src(tmp, 'v1'))
    expect(result.returncode == REFUSED_FOREIGN, f'exited {result.returncode}, expected {REFUSED_FOREIGN}')
    expect('DXGI.DLL' in result.stderr, 'refusal does not name the case variant')
    expect(snapshot(game) == before, 'a refused deploy changed the directory')


@case
def running_game_refused(tmp):
    src = make_src(tmp, 'v1')
    game = make_game(tmp)
    deploy_ok(game, src=src)
    before = snapshot(game)
    child = subprocess.Popen(
        [sys.executable, '-c',
         'import mmap, sys\n'
         'f = open(sys.argv[1], "rb")\n'
         'm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)\n'
         'print("mapped", flush=True)\n'
         'sys.stdin.read()\n',
         str(game / 'dxgi.dll')],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    src2 = make_src(tmp, 'v2')
    try:
        expect(child.stdout.readline().strip() == 'mapped', 'the stand-in game did not map the DLL')
        for args, what in (((), 'deploy'), (('--uninstall',), 'uninstall'), (('--dry-run',), 'dry run')):
            result = run(game, *args, src=None if args == ('--uninstall',) else src2)
            expect(result.returncode == REFUSED_RUNNING, f'{what} exited {result.returncode}, expected {REFUSED_RUNNING}')
            expect(f'pid {child.pid}' in result.stderr and 'dxgi.dll' in result.stderr,
                   f'{what}: refusal does not name the pid and the mapped file')
            expect(snapshot(game) == before, f'{what} changed the directory while it was mapped')
    finally:
        child.stdin.close()
        child.wait(timeout=10)
    deploy_ok(game, src=src2)
    expect((game / 'dxgi.dll').read_bytes() == opti('v2'), 'the deploy after the game exited did not install v2')


@case
def dry_run_writes_nothing(tmp):
    src = make_src(tmp, 'v1')
    fresh = make_game(tmp)
    before = snapshot(fresh)
    result = run(fresh, '--dry-run', src=src)
    expect(result.returncode == 0, f'dry run exited {result.returncode}')
    expect('[plan] install OptiScaler.dll as dxgi.dll' in result.stdout, 'dry run printed no plan')
    expect(snapshot(fresh) == before, 'dry run wrote into an empty directory')

    root = tmp / 'reshade'
    root.mkdir()
    game = make_game(root, {'dxgi.dll': RESHADE})
    before = snapshot(game)
    result = run(game, '-n', '--keep-reshade', src=src)
    expect(result.returncode == 0, f'keep-reshade dry run exited {result.returncode}')
    expect('rename dxgi.dll (ReShade) to ReShade64.dll' in result.stdout, 'dry run did not show the rename')
    expect('LoadReshade=true' in result.stdout, 'dry run did not show the ini edit')
    expect(snapshot(game) == before, 'keep-reshade dry run changed the directory')

    deploy_ok(game, '--keep-reshade', src=src)
    before = snapshot(game)
    result = run(game, '--uninstall', '--dry-run')
    expect(result.returncode == 0 and '[plan] remove dxgi.dll' in result.stdout, 'uninstall dry run printed no plan')
    expect(snapshot(game) == before, 'uninstall dry run changed the directory')


@case
def partial_receipt_on_failure(tmp):
    src = make_src(tmp, 'v1')
    game = make_game(tmp)
    result = run(game, src=src, env_extra={'RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP': '2'})
    expect(result.returncode != 0, 'the simulated failure did not fail the run')
    expect('status partial' in result.stderr, 'no partial-receipt message')
    r = receipt(game)
    expect(r and r['status'] == 'partial', f'receipt status {r and r["status"]}')
    expect(set(r['files']) == {'dxgi.dll', 'OptiScaler.dll'}, f'partial receipt files {sorted(r["files"])}')
    expect(not (game / 'nvngx.dll_dlssnr.dll').exists(), 'a step after the failure ran')
    expect_clean_state_dir(game)
    result = run(game, '--uninstall')
    expect(result.returncode == 0, f'uninstall from a partial receipt exited {result.returncode}')
    expect(snapshot(game) == {}, f'uninstall from a partial receipt left {sorted(snapshot(game))}')
    # And a deploy after a partial one finishes the job.
    run(game, src=src, env_extra={'RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP': '1'})
    deploy_ok(game, src=src)
    expect(receipt(game)['status'] == 'complete', 'the deploy after a partial one did not complete')


@case
def uninstall_leaves_changed_files(tmp):
    game = make_game(tmp)
    deploy_ok(game, src=make_src(tmp, 'v1'))
    ini = game / 'OptiScaler.ini'
    ini.write_text(ini.read_text().replace('LogToFile=auto', 'LogToFile=true', 1))
    edited = ini.read_bytes()
    result = run(game, '--uninstall')
    expect(result.returncode == 0, f'uninstall exited {result.returncode}')
    expect(ini.read_bytes() == edited, 'a user-edited ini was removed or changed')
    expect('OptiScaler.ini: left in place' in result.stdout, 'no note about the file left in place')
    for rel in ('dxgi.dll', 'OptiScaler.dll', 'nvngx.dll_dlssnr.dll'):
        expect(not (game / rel).exists(), f'{rel} was not removed')
    r = receipt(game)
    expect(r and r['status'] == 'uninstall-incomplete' and set(r['files']) == {'OptiScaler.ini'},
           f'receipt after a partial uninstall: {r}')


@case
def uninstall_needs_a_receipt(tmp):
    game = make_game(tmp, {'dxgi.dll': opti('old')})
    before = snapshot(game)
    result = run(game, '--uninstall')
    expect(result.returncode == 1, f'exited {result.returncode}, expected 1')
    expect(snapshot(game) == before, 'uninstall without a receipt changed the directory')
    # A receipt that points outside the directory is refused, not followed.
    (game / '.retoque').mkdir()
    victim = tmp / 'victim.txt'
    victim.write_text('x')
    (game / '.retoque' / 'receipt.txt').write_text(
        f'version\t1\nstatus\tcomplete\nfile\t{sha(victim)}\t../victim.txt\n')
    result = run(game, '--uninstall')
    expect(result.returncode == 1 and victim.exists(), 'a receipt path outside the game directory was followed')


@case
def libxell_rename_recorded(tmp):
    game = make_game(tmp, {'libxell.dll': PE + b'the game libxell', 'OptiScaler/libxell.dll': PE + b'our libxell'})
    before = snapshot(game)
    deploy_ok(game, src=make_src(tmp, 'v1'))
    expect((game / 'OptiScaler' / 'libxell.dll.disabled').exists(), 'our libxell was not disabled')
    expect(('OptiScaler/libxell.dll', 'OptiScaler/libxell.dll.disabled') in [r[1:] for r in receipt(game)['renames']],
           'the libxell rename is not in the receipt')
    run(game, '--uninstall')
    expect(snapshot(game) == before, 'uninstall did not undo the libxell rename')


@case
def shellcheck_clean(tmp):
    if not shutil.which('shellcheck'):
        print('SKIP: shellcheck not on PATH')
        return
    result = subprocess.run(['shellcheck', '-S', 'warning', str(SCRIPT)], capture_output=True, text=True)
    last_run.append(result)
    expect(result.returncode == 0, 'shellcheck reports warnings')


def main():
    failed = []
    for fn in CASES:
        last_run.clear()
        with tempfile.TemporaryDirectory(prefix='deploy-safety-') as tmp:
            try:
                fn(Path(tmp))
            except Failure as error:
                failed.append(fn.__name__)
                print(f'FAIL: {fn.__name__}: {error}')
                if last_run:
                    out = last_run[-1]
                    print('  last command output:')
                    for line in (out.stdout + out.stderr).splitlines()[-25:]:
                        print(f'    {line}')
                continue
        print(f'PASS: {fn.__name__}')
    print(f'{len(CASES) - len(failed)} passed, {len(failed)} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
