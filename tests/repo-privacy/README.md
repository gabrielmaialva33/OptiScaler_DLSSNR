# repo-privacy

The default branch is public: it is mirrored to `origin` minutes after each commit (see `CLAUDE.md`).
This suite fails when a tracked file carries something personal:

- a home path (`/home/<user>/`, or the Wine form `Z:\home\<user>`), a Windows user profile
  (`C:\Users\<name>`), or a mounted library path (`/mnt/<disk>/...`);
- a tailnet address (`100.64.0.0/10`) or `*.ts.net` host name, or a LAN address (`192.168.x.x`);
- a GitHub token, an API key or a private key block.

It reads `git ls-files`, skips vendored code (`external/`, `OptiScaler/include/`), binary files and its
own directory (which holds the samples), and
needs only Python. Before scanning, every pattern is checked against a sample it must match and a near
miss it must not, so a pattern that silently matches nothing fails the run.

A line that has to keep a match (an example in documentation, say) can carry the marker
`privacy: allow`; use that sparingly and prefer `~/`, `<library>` or `$HOME` forms.

Added on 2026-10-13, after an audit found three home paths in tracked text and nothing worse.

```bash
python3 tests/repo-privacy/run.py
```
