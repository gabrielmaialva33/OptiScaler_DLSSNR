# Assemble a release zip. Run: pwsh tools\package_release.ps1 [-Version <tag>] [-SkipBuild]
#
# The build output directory is not the release: it also holds import libraries, export files, debug
# symbols, and whatever earlier experiments left behind. Shipping that folder wholesale is how a
# release ends up containing a DLL nobody meant to publish, so this copies an explicit list and
# refuses anything not on it.
#
# What is deliberately NOT here: nvngx_dlssnr.dll. That is NVIDIA's, it is not ours to redistribute,
# and the user supplies their own copy per game folder. Only the ~115 KB forwarder ships, with
# install-windows.ps1/.cmd from tools\ to put it, OptiScaler.dll and the OptiScaler\ folder in place.

param(
    [string]$Version = "v0.1.0-dlssnr",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

# Derived rather than hardcoded, so this packages whichever checkout it is sitting in. There is more
# than one now -- the experiment runs in a git worktree beside the main tree, and a hardcoded root
# silently packages the other one's build output while reporting success.
$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)   # this script lives in tools/
$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
$stage = "$root\release\$Version"
$zip = "$root\release\OptiScaler-DLSSNR-$Version.zip"

if (-not $SkipBuild) {
    foreach ($proj in @("$root\retoque\dlssnr\forwarder\dlssnr_forwarder.vcxproj", "$root\OptiScaler.sln")) {
        $out = & $msb $proj /p:Configuration=Release /p:Platform=x64 /v:minimal /m 2>&1
        $err = $out | Select-String "error "
        if ($err) { Write-Host "FAILED: $proj"; $err | Select-Object -First 6; exit 1 }
    }
    Write-Host "built"
}

$src = "$root\x64\Release\a"

# The forwarder is taken from its own build output, not from the shared folder. The solution build
# does not reliably rebuild it, and a stale one here would ship silently.
#
# A fresh checkout has no per-project output directory until that project has been built on its own,
# so fall back to the shared folder rather than failing. The export check below is what actually
# guards against a stale one, and it runs either way.
$forwarder = "$root\retoque\dlssnr\forwarder\x64\Release\a\nvngx.dll_dlssnr.dll"

if (-not (Test-Path $forwarder)) {
    $forwarder = "$root\x64\Release\a\nvngx.dll_dlssnr.dll"
    Write-Host "forwarder: using the shared build output ($forwarder)"
}

# dlssnr_abi_version is here for the case the other three cannot see: a forwarder whose exports are
# all present but whose argument lists are from an older build. The name alone is a weak check --
# it says the forwarder knows about versioning, not that it agrees with this one -- but the number
# itself lives in the DLL's data rather than its export names, so it is the loader that compares it
# against kDlssNrForwarderAbi and refuses. This only has to catch a forwarder built before the export
# existed at all.
$exports = @("dlssnr_call_create", "dlssnr_call_evaluate", "dlssnr_call_set_extras", "dlssnr_abi_version")
$bytes = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($forwarder))
$missing = @($exports | Where-Object { $bytes.IndexOf($_) -lt 0 })

if ($missing.Count -gt 0) {
    Write-Host "STALE forwarder: missing $($missing -join ', ')"
    exit 1
}

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# Files, then folders. Anything not named here does not ship.
$files = @(
    "OptiScaler.dll",
    "OptiScaler.ini",
    "setup_windows.bat",
    "setup_linux.sh",
    "!! EXTRACT ALL FILES TO GAME FOLDER !!",
    "READ ME - DLSS Neural Rendering.txt"
)

foreach ($f in $files) {
    if (Test-Path "$src\$f") { Copy-Item "$src\$f" "$stage\$f" -Force }
    else { Write-Host "missing from build output: $f" }
}

foreach ($d in @("Licenses", "OptiScaler")) {
    Copy-Item "$src\$d" "$stage\$d" -Recurse -Force
}

Copy-Item $forwarder "$stage\nvngx.dll_dlssnr.dll" -Force

# The Windows installer ships beside the DLLs it installs (tools\README.md). It is source, not build output,
# so it comes from tools\ as checked out, and a release without it is refused rather than shipped. The .cmd
# is written with CRLF whatever the checkout's line ends: cmd.exe can miss a label in a batch file with LF.
foreach ($f in @("install-windows.ps1", "install-windows.cmd")) {
    $from = "$root\tools\$f"
    if (-not (Test-Path $from)) {
        Write-Host "REFUSING: tools\$f is missing; the release would ship without its installer"
        exit 1
    }
    if ($f -like "*.cmd") {
        $text = [System.IO.File]::ReadAllText($from) -replace "`r?`n", "`r`n"
        [System.IO.File]::WriteAllText("$stage\$f", $text, [System.Text.Encoding]::ASCII)
    } else {
        Copy-Item $from "$stage\$f" -Force
    }
}

# Optional dictionaries are source assets, not build-output discoveries. Keep them inactive:
# the loader only reads OptiScaler.lang immediately beside the DLL, never this subdirectory.
$translationStage = "$stage\translations\optional"
New-Item -ItemType Directory -Force -Path $translationStage | Out-Null
Copy-Item "$root\retoque\translations\optional\pt-BR.lang" "$translationStage\pt-BR.lang" -Force
Copy-Item "$root\retoque\translations\README.md" "$stage\translations\README.md" -Force
if (Test-Path "$stage\OptiScaler.lang") {
    throw "REFUSING: an active localization dictionary must not ship beside the DLL"
}


# Logging on, in the release only.
#
# Upstream ships LogToFile=auto, which resolves to false, and the source ini is theirs -- changing it
# in the repo would put a log-behaviour change into a PR that is about neural rendering. But this is
# an experimental build whose notes ask people to attach OptiScaler.log, and the first release shipped
# asking for a file that was never written.
#
# Info rather than Trace: every line explaining why the pass did not start is Info or worse, so it
# answers the common report at almost no cost. Crash reports need Trace and synchronous writes, and
# the notes say so rather than everyone paying for it.
$iniPath = "$stage\OptiScaler.ini"
$ini = Get-Content $iniPath -Raw
$ini = $ini -replace '(?m)^LogToFile=auto', 'LogToFile=true'
$ini = $ini -replace '(?m)^LogLevel=auto', 'LogLevel=2'
Set-Content $iniPath $ini -Encoding utf8 -NoNewline

$check = Select-String -Path $iniPath -Pattern '^LogToFile=|^LogLevel=' | ForEach-Object { $_.Line }
Write-Host "log settings: $($check -join ', ')"

# Belt and braces: nothing that is a build artifact, and nothing from the abandoned warp work, may
# survive into the zip regardless of how it got into the staging folder.
Get-ChildItem $stage -Recurse -Include *.exp, *.lib, *.pdb, *.ilk, *latewarp* | Remove-Item -Force

# No feature may ship switched on by accident.
#
# A global regex on "^Enabled=auto" once turned on five sections at once -- output scaling,
# sharpening, the magnifier and two more -- while trying to enable one, because the ini has six keys
# called Enabled in six different sections. That was in a test install rather than a release, and
# only because nothing was checking. This checks.
$on = Select-String -Path "$stage\OptiScaler.ini" -Pattern '^Enabled=true'

if ($on) {
    Write-Host "REFUSING: the packaged ini has features switched on:"
    $on | ForEach-Object { "  line $($_.LineNumber): $($_.Line)" }
    exit 1
}

Write-Host "ini verified: nothing switched on by default"

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal

Write-Host ""
Write-Host "staged at $stage"
Get-ChildItem $stage | ForEach-Object { "  {0,-42} {1,10:N0}" -f $_.Name, $_.Length }
Write-Host ""
Write-Host ("zip: {0}  ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
