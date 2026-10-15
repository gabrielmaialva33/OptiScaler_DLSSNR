#!/usr/bin/env python3
"""Drive the step-0 MFG-below-Ada probe logic (retoque/framegen/dlssg/MfgProbe.h) on the host.

The header is std-only and is compiled straight from production, not copied. The suite proves the
architecture gate exits before anything else, the fatbin/cubin census over synthetic containers, the
exact text of both log lines, and foreign-loader detection. When a real nvngx_dlssg.dll is present
(Crimson Desert's, read in place and never copied), it also holds the census against the offline
inventory in docs/dlssnr/design/mfg-rtx20-30.md. That case skips when the file is absent.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
repo = root.parents[1]

# The real provider to census against, if it is installed. Built from Path.home() so no home path is
# ever written into a tracked file (tests/repo-privacy guards that). A relative Steam path is not
# personal. The binary skips the case gracefully when the file is absent.
provider = Path.home() / '.local/share/Steam/steamapps/common/Crimson Desert/bin64/nvngx_dlssg.dll'

with tempfile.TemporaryDirectory(prefix='optiscaler-mfg-probe-') as directory:
    binary = str(Path(directory) / 'test')
    # The header under test is included straight from production (retoque/framegen/dlssg/), not copied.
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                    '-I', str(repo / 'retoque/framegen/dlssg'),
                    str(root / 'test.cpp'), '-o', binary], check=True)
    args = [binary]
    if provider.is_file():
        args.append(str(provider))
    # A census that loops on a wrapped length would hang; fail it fast instead of at the runner's ceiling.
    subprocess.run(args, check=True, timeout=120)
