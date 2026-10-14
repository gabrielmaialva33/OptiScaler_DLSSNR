<#
.SYNOPSIS
Installs Retoque (OptiScaler with DLSS Neural Rendering) into a game folder on Windows, or takes it out.

.DESCRIPTION
The Windows counterpart of tools/deploy-to-game.sh, shipped in the release zip beside the DLLs. It puts
OptiScaler.dll in the game folder under a proxy name the game loads (dxgi.dll by default), with
OptiScaler.dll and the DLSS-NR forwarder (nvngx.dll_dlssnr.dll) beside it, the release's OptiScaler\
support folder (FSR, XeSS and Agility SDK DLLs), and OptiScaler.ini only when the game has none.

Every change is planned before the first write, and nothing is written when:
  - the game is running (exit 3): a process has a module loaded from the game folder, or a file this would
    touch cannot be opened exclusively. A DLL overwritten while its game ran is how Cyberpunk died two
    minutes later (2026-09-19).
  - a file at one of our names is not ours (exit 4). Ours means the receipt's hash matches, or the file is
    an OptiScaler build or forwarder (it carries their marker string); those are backed up and replaced
    without a flag. ReShade at the proxy name takes -KeepReShade, anything else -ReplaceForeign.
  - another proxy name in the folder already holds OptiScaler (exit 5). Both load and their hooks fight
    (Divinity, 2026-09-19). -AllowSecondProxy overrides.

-KeepReShade renames ReShade (dxgi.dll to ReShade64.dll, d3d12.dll to d3d12-ReShade64.dll) and sets
[Plugins] LoadReshade=true in OptiScaler.ini by replacing the line that is there. A second copy of a key is
never added: the parser keeps the last one, so the shipped one would win.

What an install wrote, set aside or renamed goes into <game>\.retoque\receipt.txt, in exactly the format
tools/deploy-to-game.sh writes (version 1, tab-separated, paths relative and with '/'), so either script
uninstalls what the other installed. It is rewritten after every step; an install that stops part-way leaves
it marked partial. Backups go to <game>\.retoque\backup\<UTC stamp>\, never beside the DLL. -Uninstall
removes only recorded files whose hash still matches, puts back what was set aside and undoes the renames.

NVIDIA's model, nvngx_dlssnr.dll, is never copied, moved or removed; it is only reported when missing.

Exit status: 0 done (or a dry run that would go ahead), 1 usage or error, 3 the game is running, 4 a file
in the way is not ours, 5 another proxy name already holds OptiScaler.

RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP=<n> stops a run with exit 1 after its n-th step, as a failed copy would.
It is the same variable tools/deploy-to-game.sh reads, and exists for tests/windows-installer.

.PARAMETER GameDir
The folder that holds the game's executable (for Unreal Engine games usually <Game>\Binaries\Win64).

.PARAMETER ProxyName
The name OptiScaler is installed under: dxgi.dll (default), winmm.dll, version.dll, dbghelp.dll, d3d12.dll,
winhttp.dll or wininet.dll.

.PARAMETER SourceDir
Where OptiScaler.dll, nvngx.dll_dlssnr.dll, OptiScaler.ini and the OptiScaler\ folder are taken from.
Defaults to this script's own folder, which is where the release zip puts them.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File install-windows.ps1 -GameDir "D:\Games\Crimson Desert\bin64"

.EXAMPLE
powershell -ExecutionPolicy Bypass -File install-windows.ps1 -GameDir "D:\Games\Crimson Desert\bin64" -Uninstall
#>

[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Position = 0)] [string]$GameDir,
    [Parameter(Position = 1)] [string]$ProxyName = 'dxgi.dll',
    [string]$SourceDir,
    [switch]$KeepReShade,
    [switch]$ReplaceForeign,
    [switch]$AllowSecondProxy,
    [switch]$DryRun,
    [switch]$Uninstall,
    [switch]$Help
)

# PowerShell 5.1 (Windows PowerShell) and 7 both run this. Keep the file ASCII: 5.1 reads a script without a
# byte-order mark in the ANSI code page, so one typographic quote can change how a line parses.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# Names OptiScaler answers to when the game loads it (dllmain.cpp, dllNames). Any of them in the game folder
# can be loaded, which is how Divinity ended up with two instances.
$ProxyNames = @('dxgi.dll', 'winmm.dll', 'version.dll', 'dbghelp.dll', 'd3d12.dll', 'winhttp.dll', 'wininet.dll')

# What a DLL is, from its bytes; the strings tools/deploy-to-game.sh reads. "OptiScaler working as" is
# CheckWorkingMode's log line, in every OptiScaler build. "dlssnr_call_" prefixes the forwarder's exports.
# OptiScaler itself contains "ReShade" (it loads ReShade64.dll), so ReShade is tested only after OptiScaler
# has been ruled out.
$MarkOptiScaler = 'OptiScaler working as'
$MarkForwarder = 'dlssnr_call_'
$MarkReShade = 'ReShade'

$ForwarderName = 'nvngx.dll_dlssnr.dll'
$ModelName = 'nvngx_dlssnr.dll'
$IniName = 'OptiScaler.ini'
$SupportDir = 'OptiScaler'

$OnWindows = [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT
$Sep = [string][IO.Path]::DirectorySeparatorChar
$PathCompare = if ($OnWindows) { [StringComparison]::OrdinalIgnoreCase } else { [StringComparison]::Ordinal }
$Latin1 = [Text.Encoding]::GetEncoding(28591)   # one char per byte: binary and ini text round-trip unchanged
$Utf8 = New-Object Text.UTF8Encoding -ArgumentList $false
$BomLatin1 = [string][char]0xEF + [char]0xBB + [char]0xBF
$ReHeader = [regex]'^\s*\[([^\]]*)\]\s*$'
$ReKey = [regex]'^\s*([^=;#\s][^=]*)=(.*)$'
# File.Move(src, dst, overwrite) exists on .NET Core (PowerShell 7) only; 5.1 uses File.Replace instead.
$MoveOverwrite = [IO.File].GetMethod('Move', [type[]]@([string], [string], [bool]))

# The function scopes below have their own; this is what the command line gave.
$ScriptParams = $PSBoundParameters

$script:Game = ''
$script:Proxy = ''
$script:StateDir = ''
$script:ReceiptPath = ''
$script:BackupRel = ''
$script:StopCode = 1
$script:RunMode = 'deploy'
$script:ReceiptOpen = $false   # true while a run is changing the folder; a failure then marks the receipt
$script:Incoming = $null       # a temp file not yet renamed into place, removed on exit
$script:ReshadeRoute = $false

# The receipt, as loaded and as it will be written back.
$script:FileSha = [ordered]@{}   # relative path -> sha256, in the order first written
$script:Backups = New-Object 'Collections.Generic.List[string]'   # "sha<TAB>path<TAB>copy"
$script:Renames = New-Object 'Collections.Generic.List[string]'   # "sha<TAB>from<TAB>to"
$script:Proxies = New-Object 'Collections.Generic.List[string]'
$script:OldStatus = ''

$script:Plan = New-Object 'Collections.Generic.List[object]'
$script:Planned = @{}   # relative path -> sha it will have once the plan has run
$script:Refusals = New-Object 'Collections.Generic.List[string]'
$script:RefuseCode = 0
$script:Removed = New-Object 'Collections.Generic.List[string]'

# ------------------------------------------------------------------------------------------------------------
# Output
# ------------------------------------------------------------------------------------------------------------

function Write-Line([string]$Text) { Write-Host $Text }

function Write-ErrorLine([string]$Text) { [Console]::Error.WriteLine($Text) }

function Stop-Run([int]$Code, [string]$Message) {
    if ($Message) { Write-ErrorLine "[-] Error: $Message" }
    $script:StopCode = $Code
    throw 'retoque-stop'
}

function Show-Usage {
    Write-Host @'
Usage: install-windows.ps1 -GameDir <folder> [-ProxyName dxgi.dll] [options]
       install-windows.ps1 -GameDir <folder> -Uninstall [-DryRun]

  -GameDir <folder>   the folder with the game's .exe (Unreal Engine: <Game>\Binaries\Win64)
  -ProxyName <name>   dxgi.dll (default), winmm.dll, version.dll, dbghelp.dll, d3d12.dll,
                      winhttp.dll or wininet.dll
  -DryRun             print the plan (and any refusal); write nothing
  -Uninstall          remove what <folder>\.retoque\receipt.txt records (files whose hash still
                      matches), restore what was set aside, undo the renames
  -KeepReShade        the proxy name holds ReShade: rename it to ReShade64.dll and set
                      [Plugins] LoadReshade=true so OptiScaler loads it
  -ReplaceForeign     replace a file at one of our names that is not ours; the original is kept
                      under .retoque\backup\ and -Uninstall puts it back
  -AllowSecondProxy   install although another proxy name here already holds OptiScaler
  -SourceDir <folder> take the files from there instead of this script's folder

Refuses, writing nothing, while the game is running. Never touches nvngx_dlssnr.dll.
Exit status: 0 ok, 1 error, 3 game running, 4 a file in the way is not ours, 5 second proxy.
'@
}

# ------------------------------------------------------------------------------------------------------------
# Paths and files. .NET calls rather than *-Item cmdlets throughout: a game folder named "Game [GOTY]" is a
# wildcard pattern to the cmdlets' -Path, and nothing here should ever be one.
# ------------------------------------------------------------------------------------------------------------

function Get-FullPath([string]$Rel) { [IO.Path]::Combine($script:Game, $Rel.Replace('/', $Sep)) }

function Get-Attributes([string]$Path) {
    # -1 when nothing is there. A dangling link still has attributes, so it counts as something.
    return [int](New-Object IO.FileInfo -ArgumentList $Path).Attributes
}

function Test-Entry([string]$Rel) { (Get-Attributes (Get-FullPath $Rel)) -ne -1 }

function Test-RegularFile([string]$Rel) {
    $a = Get-Attributes (Get-FullPath $Rel)
    return ($a -ne -1) -and (($a -band [int][IO.FileAttributes]::Directory) -eq 0) -and
        (($a -band [int][IO.FileAttributes]::ReparsePoint) -eq 0)
}

function Get-Inner($Exception) {
    # PowerShell wraps what a .NET call throws; the IOException underneath is what tells the cases apart.
    $e = $Exception
    while ($null -ne $e.InnerException -and $e -is [Management.Automation.RuntimeException]) { $e = $e.InnerException }
    return $e
}

function Open-Shared([string]$Path) {
    # Read access that tolerates other openers: a DLL a process has mapped can still be read and hashed.
    return [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite, Delete')
}

# sha256 of a file's content, lower-case hex as sha256sum prints it, or "-" when there is none to read.
function Get-Sha([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { return '-' }
    $stream = $null
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = Open-Shared $Path
        return ([BitConverter]::ToString($hasher.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } catch {
        return '-'
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $hasher.Dispose()
    }
}

function Read-Text([string]$Path) {
    $buffer = New-Object IO.MemoryStream
    $stream = Open-Shared $Path
    try { $stream.CopyTo($buffer) } finally { $stream.Dispose() }
    return $Latin1.GetString($buffer.ToArray())
}

function Test-Marker([string]$Path, [string]$Marker) {
    try { return (Read-Text $Path).IndexOf($Marker, [StringComparison]::Ordinal) -ge 0 } catch { return $false }
}

# Every entry under the game folder whose relative path equals $Rel ignoring case, in its own spelling.
# Windows sees one file there; a case-sensitive file system can hold two, and Wine would see both as one name.
function Find-Variants([string]$Rel) {
    $found = @('')
    foreach ($part in $Rel.Split('/')) {
        $next = @()
        foreach ($base in $found) {
            $dir = if ($base -eq '') { $script:Game } else { Get-FullPath $base }
            if (-not [IO.Directory]::Exists($dir)) { continue }
            foreach ($entry in [IO.Directory]::GetFileSystemEntries($dir)) {
                $name = [IO.Path]::GetFileName($entry)
                if ([string]::Equals($name, $part, [StringComparison]::OrdinalIgnoreCase)) {
                    $next += $(if ($base -eq '') { $name } else { "$base/$name" })
                }
            }
        }
        $found = $next
    }
    return $found
}

# A receipt path must stay inside the game folder: relative, '/'-separated, no '.' or '..' component, nothing
# Windows would read as a drive, a stream or another separator, and no component Windows would trim (a
# trailing dot or space turns ".. " into ".."). A receipt is plain text in a game folder; it is not trusted.
function Test-SafeRel([string]$Rel) {
    if ([string]::IsNullOrEmpty($Rel) -or $Rel.StartsWith('/')) { return $false }
    if ($Rel.IndexOfAny([char[]]"`t`n`r\:") -ge 0) { return $false }
    foreach ($part in $Rel.Split('/')) {
        if ($part -eq '' -or $part -eq '.' -or $part -eq '..' -or $part.EndsWith('.') -or $part.EndsWith(' ')) { return $false }
    }
    try { $full = [IO.Path]::GetFullPath((Get-FullPath $Rel)) } catch { return $false }
    return $full.StartsWith($script:Game + $Sep, $PathCompare)
}

function Test-ModelPath([string]$Rel) { [IO.Path]::GetFileName($Rel.Replace('/', $Sep)) -ieq $ModelName }

function New-TempPath([string]$Prefix) {
    $chars = 'abcdefghijklmnopqrstuvwxyz0123456789'
    do {
        $suffix = -join (1..6 | ForEach-Object { $chars[(Get-Random -Maximum $chars.Length)] })
        $path = [IO.Path]::Combine($script:StateDir, "$Prefix.$suffix")
    } while ((Get-Attributes $path) -ne -1)
    return $path
}

# Renames $From over $To. A rename replaces the directory entry; copying onto the target would rewrite a
# file in place, which is the Cyberpunk failure if anything still has it loaded.
function Move-Over([string]$From, [string]$To) {
    if ($null -ne $MoveOverwrite) { [IO.File]::Move($From, $To, $true) }
    elseif ([IO.File]::Exists($To)) { [IO.File]::Replace($From, $To, [NullString]::Value, $true) }
    else { [IO.File]::Move($From, $To) }
}

function New-ParentDir([string]$Path) {
    $dir = [IO.Path]::GetDirectoryName($Path)
    if (-not [IO.Directory]::Exists($dir)) { [void][IO.Directory]::CreateDirectory($dir) }
}

# ------------------------------------------------------------------------------------------------------------
# Receipt
# ------------------------------------------------------------------------------------------------------------

function Set-FileRecord([string]$Rel, [string]$Sha) { $script:FileSha[$Rel] = $Sha }

function Remove-FileRecord([string]$Rel) { $script:FileSha.Remove($Rel) }

function Remove-Line($List, [string]$Line) {
    for ($i = $List.Count - 1; $i -ge 0; $i--) { if ($List[$i] -ceq $Line) { $List.RemoveAt($i) } }
}

function Add-Proxy([string]$Name) { if ($script:Proxies -notcontains $Name) { $script:Proxies.Add($Name) } }

function Read-Receipt {
    if (-not [IO.File]::Exists($script:ReceiptPath)) { return }
    $text = [IO.File]::ReadAllText($script:ReceiptPath, $Utf8)   # drops a byte-order mark if an editor added one
    foreach ($raw in $text.Split([char[]]"`n")) {
        $line = if ($raw.EndsWith("`r")) { $raw.Substring(0, $raw.Length - 1) } else { $raw }
        if ($line -eq '' -or $line.StartsWith('#')) { continue }
        $f = $line.Split([char[]]"`t", 4)
        $kind = $f[0]
        $a = if ($f.Length -gt 1) { $f[1] } else { '' }
        $b = if ($f.Length -gt 2) { $f[2] } else { '' }
        $c = if ($f.Length -gt 3) { $f[3] } else { '' }
        switch -CaseSensitive ($kind) {
            'version' { if ($a -ne '1') { Stop-Run 1 "$($script:ReceiptPath) is version '$a'; this script reads version 1" } }
            'status' { $script:OldStatus = $a }
            'updated' { }
            'proxy' { Add-Proxy $a }
            'file' {
                if (-not (Test-SafeRel $b)) { Stop-Run 1 "unsafe path '$b' in $($script:ReceiptPath)" }
                Set-FileRecord $b $a
            }
            'backup' {
                if (-not ((Test-SafeRel $b) -and (Test-SafeRel $c))) { Stop-Run 1 "unsafe path in $($script:ReceiptPath): $b $c" }
                $script:Backups.Add("$a`t$b`t$c")
            }
            'rename' {
                if (-not ((Test-SafeRel $b) -and (Test-SafeRel $c))) { Stop-Run 1 "unsafe path in $($script:ReceiptPath): $b $c" }
                $script:Renames.Add("$a`t$b`t$c")
            }
            default { Stop-Run 1 "unrecognised record '$kind' in $($script:ReceiptPath)" }
        }
    }
}

# Written whole to a temp file beside it and renamed over the old one, so a reader (or a crash) sees the
# previous receipt or the new one, never half of one. LF line ends and no byte-order mark: the Linux script
# reads it with `read`, and a mark would make its first record unrecognisable.
function Write-Receipt([string]$Status) {
    $sb = New-Object Text.StringBuilder
    [void]$sb.Append("# Retoque install receipt: what install-windows.ps1 or tools/deploy-to-game.sh wrote, set aside or renamed here.`n")
    [void]$sb.Append("# Their uninstall reads it. Tab-separated; paths are relative to the game directory.`n")
    $now = [DateTime]::UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", [Globalization.CultureInfo]::InvariantCulture)
    [void]$sb.Append("version`t1`nstatus`t$Status`nupdated`t$now`n")
    foreach ($p in $script:Proxies) { [void]$sb.Append("proxy`t$p`n") }
    foreach ($rel in $script:FileSha.Keys) { [void]$sb.Append("file`t$($script:FileSha[$rel])`t$rel`n") }
    foreach ($line in $script:Backups) { [void]$sb.Append("backup`t$line`n") }
    foreach ($line in $script:Renames) { [void]$sb.Append("rename`t$line`n") }
    $tmp = New-TempPath 'receipt'
    [IO.File]::WriteAllText($tmp, $sb.ToString(), $Utf8)
    Move-Over $tmp $script:ReceiptPath
}

# ------------------------------------------------------------------------------------------------------------
# Is the game running?
# ------------------------------------------------------------------------------------------------------------

function Add-Refusal([int]$Code, [string]$Text) {
    $script:Refusals.Add($Text)
    if ($script:RefuseCode -eq 0) { $script:RefuseCode = $Code }
}

function Stop-IfRefused {
    if ($script:Refusals.Count -eq 0) { return }
    foreach ($r in $script:Refusals) { Write-ErrorLine "[-] Refusing: $r" }
    Write-ErrorLine '[-] Nothing was written.'
    Stop-Run $script:RefuseCode
}

# Two checks, because neither sees everything. A process's module list names every DLL it loaded from the
# folder, but the modules of an elevated or protected process (a game under anti-cheat) cannot be read. A
# file a process has loaded cannot be opened for writing with no sharing, whoever the process is, but that
# only covers the files tried. Any hit refuses, with no override.
function Assert-NotRunning([string[]]$Rels) {
    $prefix = $script:Game + $Sep
    $hits = New-Object 'Collections.Generic.List[string]'
    $unreadable = 0
    foreach ($proc in @(Get-Process)) {
        if ($proc.Id -eq $PID) { continue }
        $modules = $null
        try { $modules = @($proc.Modules) } catch { $unreadable++; continue }
        foreach ($module in $modules) {
            $file = $null
            try { $file = [string]$module.FileName } catch { $file = $null }
            if ($file -and $file.StartsWith($prefix, $PathCompare)) {
                $hits.Add("pid $($proc.Id) ($($proc.ProcessName)) has $($file.Substring($prefix.Length)) loaded")
                break
            }
        }
    }
    if ($unreadable -gt 0) {
        Write-Line "[=] $unreadable process(es) could not be inspected (system or elevated); files in use are checked directly"
    }

    $denied = New-Object 'Collections.Generic.List[string]'
    $seen = @{}
    foreach ($rel in $Rels) {
        if (-not $rel -or $seen.Contains($rel)) { continue }
        $seen[$rel] = $true
        $path = Get-FullPath $rel
        if (-not [IO.File]::Exists($path)) { continue }
        $stream = $null
        try {
            $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        } catch {
            $e = Get-Inner $_.Exception
            if ($e -is [UnauthorizedAccessException]) { $denied.Add($rel) }
            elseif ($e -is [IO.FileNotFoundException] -or $e -is [IO.DirectoryNotFoundException]) { }
            elseif ($e -is [IO.IOException]) { $hits.Add("$rel is open in another process ($($e.Message.Trim()))") }
            else { throw }
        } finally {
            if ($null -ne $stream) { $stream.Dispose() }
        }
    }

    if ($hits.Count -gt 0) {
        $msg = 'files in this folder are in use; overwriting a DLL a running game has loaded is how Cyberpunk died on 2026-09-19:'
        foreach ($h in $hits) { $msg += "`n      $h" }
        $msg += "`n    Quit the game, wait for it to exit, then run this again."
        Add-Refusal 3 $msg
        Stop-IfRefused
    }
    if ($denied.Count -gt 0) {
        $msg = "Windows denies write access to $($denied -join ', ') in $($script:Game). Run this from an administrator prompt (the game may be under Program Files), or clear the read-only flag."
        if ($DryRun) { Write-Line "[!] $msg" } else { Stop-Run 1 $msg }
    }
}

# ------------------------------------------------------------------------------------------------------------
# OptiScaler.ini, read the way simpleini reads it: section and key names compare without case, ';' and '#'
# start comment lines, and the last of several equal keys wins. Bytes go through Latin-1, so a line this does
# not change comes back byte for byte, line ending included.
# ------------------------------------------------------------------------------------------------------------

function Split-Lines([string]$Text) {
    if ($Text.Length -eq 0) { return @() }
    $lines = $Text.Split([char[]]"`n")
    if ($Text.EndsWith("`n")) { $lines = $lines[0..($lines.Length - 2)] }
    return $lines
}

function Get-IniHeader([string]$Body) {
    $m = $ReHeader.Match($Body)
    if ($m.Success) { return $m.Groups[1].Value.Trim() }
    return $null
}

function Get-IniKey([string]$Body) {
    $m = $ReKey.Match($Body)
    if ($m.Success) { return @($m.Groups[1].Value.Trim(), $m.Groups[2].Value.Trim()) }
    return $null
}

function Get-IniValues([string]$Text, [string]$Section, [string]$Key) {
    if ($Text.StartsWith($BomLatin1)) { $Text = $Text.Substring(3) }
    $want = $Section.ToLowerInvariant()
    $wantKey = $Key.ToLowerInvariant()
    $current = $null
    $values = @()
    foreach ($line in @(Split-Lines $Text)) {
        $body = if ($line.EndsWith("`r")) { $line.Substring(0, $line.Length - 1) } else { $line }
        $header = Get-IniHeader $body
        if ($null -ne $header) {
            $current = $header.ToLowerInvariant()
        } elseif ($current -ceq $want) {
            $kv = Get-IniKey $body
            if ($null -ne $kv -and $kv[0].ToLowerInvariant() -ceq $wantKey) { $values += $kv[1] }
        }
    }
    return $values
}

# $Text with $Key=$Value in [$Section]. Every existing $Key line in the section gets the value, keeping its
# own spelling and line ending; only when the section has no such key at all is a line added, right under
# the header, so the result never holds two. A missing section is appended.
function Set-IniText([string]$Text, [string]$Section, [string]$Key, [string]$Value) {
    $bom = ''
    if ($Text.StartsWith($BomLatin1)) { $bom = $BomLatin1; $Text = $Text.Substring(3) }
    $want = $Section.ToLowerInvariant()
    $wantKey = $Key.ToLowerInvariant()
    $insert = @(Get-IniValues $Text $Section $Key).Count -eq 0
    $done = $false
    $current = $null
    $sb = New-Object Text.StringBuilder
    foreach ($line in @(Split-Lines $Text)) {
        $body = $line
        $cr = ''
        if ($line.EndsWith("`r")) { $body = $line.Substring(0, $line.Length - 1); $cr = "`r" }
        $header = Get-IniHeader $body
        if ($null -ne $header) {
            $current = $header.ToLowerInvariant()
            [void]$sb.Append($line).Append("`n")
            if ($insert -and -not $done -and $current -ceq $want) {
                [void]$sb.Append("$Key=$Value$cr`n")
                $done = $true
            }
            continue
        }
        if ($current -ceq $want) {
            $kv = Get-IniKey $body
            if ($null -ne $kv -and $kv[0].ToLowerInvariant() -ceq $wantKey) {
                [void]$sb.Append($body.Substring(0, $body.IndexOf('='))).Append("=$Value$cr`n")
                continue
            }
        }
        [void]$sb.Append($line).Append("`n")
    }
    if ($insert -and -not $done) { [void]$sb.Append("`n[$Section]`n$Key=$Value`n") }
    return $bom + $sb.ToString()
}

# ------------------------------------------------------------------------------------------------------------
# The plan: every change is decided, and every refusal found, before the first write. A dry run prints it; a
# real run carries it out one step at a time, recording each step in the receipt.
# ------------------------------------------------------------------------------------------------------------

function Add-Plan([string]$Kind, [string]$A, [string]$B, [string]$C, [string]$Text) {
    $script:Plan.Add([pscustomobject]@{ Kind = $Kind; A = $A; B = $B; C = $C; Text = $Text })
}

function Select-BackupDir {
    $base = '.retoque/backup/' + [DateTime]::UtcNow.ToString("yyyyMMdd'T'HHmmss'Z'", [Globalization.CultureInfo]::InvariantCulture)
    $script:BackupRel = $base
    $n = 1
    while (Test-Entry $script:BackupRel) {
        $n++
        $script:BackupRel = "$base-$n"
    }
}

# What the file at $Rel is:
#   absent       nothing there
#   same         byte-identical to what is about to be installed
#   ours         what an earlier install wrote, unchanged since (the receipt's hash)
#   known        carries $Marker: an OptiScaler build or forwarder no receipt here records
#   reshade      ReShade
#   foreign      anything else, or a link
#   casevariant  the name exists in another case (case-sensitive file systems only)
#   notfile      a directory
function Get-Class([string]$Rel, [string]$Marker, [string]$SrcSha) {
    $r = @{ Class = 'absent'; Why = '' }
    if (-not $OnWindows) {
        foreach ($hit in @(Find-Variants $Rel)) {
            if ($hit -cne $Rel) {
                $r.Class = 'casevariant'
                $r.Why = "$hit is already here, and to Windows it is the same name"
                return $r
            }
        }
    }
    $path = Get-FullPath $Rel
    $attr = Get-Attributes $path
    if ($attr -eq -1) { return $r }
    if ($attr -band [int][IO.FileAttributes]::ReparsePoint) { $r.Class = 'foreign'; $r.Why = 'it is a link'; return $r }
    if ($attr -band [int][IO.FileAttributes]::Directory) { $r.Class = 'notfile'; $r.Why = 'it is not a regular file'; return $r }
    $sha = Get-Sha $path
    if ($SrcSha -and $sha -eq $SrcSha) { $r.Class = 'same' }
    elseif ($script:FileSha.Contains($Rel) -and $script:FileSha[$Rel] -eq $sha) { $r.Class = 'ours' }
    elseif ($Marker -and (Test-Marker $path $Marker)) { $r.Class = 'known' }
    elseif (Test-Marker $path $MarkReShade) { $r.Class = 'reshade' }
    else { $r.Class = 'foreign'; $r.Why = 'it is not an OptiScaler build, and no install here recorded it' }
    return $r
}

function Add-KeepReShadePlan([string]$Rel) {
    $new = $null
    if ($Rel -ieq 'dxgi.dll') { $new = 'ReShade64.dll' }
    # ReShade picks its API from its own file name; OptiScaler accepts this one for that reason.
    elseif ($Rel -ieq 'd3d12.dll') { $new = 'd3d12-ReShade64.dll' }
    else {
        Add-Refusal 4 "$Rel is ReShade; -KeepReShade handles ReShade installed as dxgi.dll or d3d12.dll only"
        return
    }
    $taken = @(Find-Variants 'ReShade64.dll') + @(Find-Variants 'd3d12-ReShade64.dll')
    if ($taken.Count -gt 0) {
        Add-Refusal 4 "$Rel is ReShade, but $($taken -join ' ') is already here and OptiScaler would load that one. Move one of them aside first."
        return
    }
    Add-Plan rename $Rel $new '' "rename $Rel (ReShade) to $new; OptiScaler loads it from there"
    $script:ReshadeRoute = $true
}

function Add-DllPlan([string]$Rel, [string]$Src, [string]$Marker, [string]$SrcSha, [bool]$IsProxy) {
    $hint = "Pass -ReplaceForeign to set it aside under $($script:BackupRel)/ and install ours"
    $srcName = [IO.Path]::GetFileName($Src)
    $c = Get-Class $Rel $Marker $SrcSha
    $installs = $true
    switch ($c.Class) {
        'absent' { Add-Plan install $Src $Rel '' "install $srcName as $Rel" }
        'same' { Add-Plan keep $Rel '' '' "${Rel}: already this build, left as it is" }
        'ours' { Add-Plan install $Src $Rel '' "replace $Rel (installed here earlier, unchanged since)" }
        'known' {
            Add-Plan backup $Rel '' '' "back up $Rel (an earlier build no install here recorded) to $($script:BackupRel)/"
            Add-Plan install $Src $Rel '' "install $srcName as $Rel"
        }
        'reshade' {
            if ($IsProxy -and $KeepReShade) {
                Add-KeepReShadePlan $Rel
                Add-Plan install $Src $Rel '' "install $srcName as $Rel"
            } elseif ($ReplaceForeign) {
                Add-Plan backup $Rel '' '' "back up $Rel (ReShade) to $($script:BackupRel)/"
                Add-Plan install $Src $Rel '' "install $srcName as $Rel"
            } else {
                $msg = "$Rel is ReShade."
                if ($IsProxy) {
                    $msg += ' Pass -KeepReShade to rename it to ReShade64.dll and have OptiScaler load it, or'
                    $msg += " -ReplaceForeign to set it aside under $($script:BackupRel)/."
                } else {
                    $msg += " $hint."
                }
                Add-Refusal 4 $msg
                $installs = $false
            }
        }
        'foreign' {
            if ($ReplaceForeign) {
                Add-Plan backup $Rel '' '' "back up $Rel ($($c.Why -replace '^it is ', '')) to $($script:BackupRel)/"
                Add-Plan install $Src $Rel '' "install $srcName as $Rel"
            } else {
                $msg = "$Rel is not ours: $($c.Why)."
                if ($IsProxy -and $KeepReShade) { $msg += ' It is not ReShade either, so -KeepReShade does not apply.' }
                Add-Refusal 4 "$msg $hint."
                $installs = $false
            }
        }
        default {
            Add-Refusal 4 "${Rel}: $($c.Why). Rename or move it, then run this again."
            $installs = $false
        }
    }
    if ($installs) { $script:Planned[$Rel] = $SrcSha }
    if ($IsProxy -and $KeepReShade -and -not $script:ReshadeRoute -and ($c.Class -eq 'same' -or $c.Class -eq 'ours')) {
        # An earlier -KeepReShade install: the rename is in the receipt; keep the ini key true.
        foreach ($line in $script:Renames) {
            if ($line.ToLowerInvariant().EndsWith('reshade64.dll')) { $script:ReshadeRoute = $true }
        }
        if (-not $script:ReshadeRoute) { Write-Line "[=] -KeepReShade: no ReShade at $Rel, nothing to keep" }
    }
}

# The release's OptiScaler\ folder (FSR, XeSS and Agility SDK DLLs), as File/Rel pairs, Rel being
# "OptiScaler/<path>". The folder is OptiScaler's own, so a different file in it is an older one: it is backed
# up and replaced like an unrecorded OptiScaler build, without a flag.
function Get-SupportFiles([string]$SrcRoot) {
    $root = [IO.Path]::Combine($SrcRoot, $SupportDir)
    if (-not [IO.Directory]::Exists($root)) { return @() }
    $out = @()
    foreach ($file in @([IO.Directory]::GetFiles($root, '*', [IO.SearchOption]::AllDirectories) | Sort-Object)) {
        # Run from the game folder itself, this folder is the game's: a libxell set aside by an earlier install
        # is not part of any release.
        if ($file.EndsWith('.disabled', [StringComparison]::OrdinalIgnoreCase)) { continue }
        $rel = $SupportDir + '/' + $file.Substring($root.Length + 1).Replace($Sep, '/')
        $out += [pscustomobject]@{ File = $file; Rel = $rel }
    }
    return $out
}

function Add-SupportPlan($Pairs) {
    $ours = "$SupportDir/libxell.dll"
    foreach ($pair in $Pairs) {
        $file = $pair.File
        $rel = $pair.Rel
        if (Test-ModelPath $rel) {
            Write-Line "[!] $rel not installed: NVIDIA's model is never copied by this script"
            continue
        }
        $srcSha = Get-Sha $file
        if ($rel -ieq $ours -and (Test-RegularFile 'libxell.dll') -and (Get-Sha (Get-FullPath 'libxell.dll')) -ne $srcSha) {
            # A mismatched pair crashed Dying Light: The Beast; see Add-LibxellPlan.
            Write-Line "[=] libxell.dll: the game ships its own, so $ours is not installed"
            continue
        }
        $c = Get-Class $rel $null $srcSha
        $why = "${rel}: $($c.Why). Rename or move it, then run this again."
        if ($c.Class -eq 'absent') {
            Add-Plan install $file $rel '' "install $rel"
        } elseif ($c.Class -eq 'same') {
            Add-Plan keep $rel '' '' "${rel}: already this version, left as it is"
        } elseif ($c.Class -eq 'ours') {
            Add-Plan install $file $rel '' "replace $rel (installed here earlier, unchanged since)"
        } elseif ($c.Class -eq 'casevariant' -or $c.Class -eq 'notfile') {
            Add-Refusal 4 $why
            continue
        } elseif ($c.Why -eq 'it is a link' -and -not $ReplaceForeign) {
            Add-Refusal 4 "$rel is not ours: it is a link. Pass -ReplaceForeign to set it aside under $($script:BackupRel)/ and install ours."
            continue
        } else {
            Add-Plan backup $rel '' '' "back up $rel (a different version no install here recorded) to $($script:BackupRel)/"
            Add-Plan install $file $rel '' "install $rel"
        }
        $script:Planned[$rel] = $srcSha
    }
}

function Test-OtherProxies {
    foreach ($name in $ProxyNames) {
        if ($name -ieq $script:Proxy) { continue }
        foreach ($hit in @(Find-Variants $name)) {
            if (-not (Test-RegularFile $hit)) { continue }
            if (-not (Test-Marker (Get-FullPath $hit) $MarkOptiScaler)) { continue }
            if ($AllowSecondProxy) {
                Write-Line "[!] $hit also holds an OptiScaler build; installing anyway (-AllowSecondProxy). If the game loads both, their hooks fight."
            } else {
                Add-Refusal 5 ("$hit already holds an OptiScaler build, and $($script:Proxy) would be a second one in this folder. " +
                    'The game can load both, and their hooks fight (Divinity, 2026-09-19). ' +
                    "Run -Uninstall, or move $hit aside (e.g. to $hit.off), or pass -AllowSecondProxy.")
            }
        }
    }
}

function Add-IniPlan([string]$SrcIni) {
    $iniRel = $IniName
    $hits = @(Find-Variants $IniName)
    if ($hits.Count -gt 0) { $iniRel = $hits[0] }
    $text = ''
    # The user's own OptiScaler.ini is never replaced; the release's is copied only when there is none.
    if (-not (Test-Entry $iniRel)) {
        if ([IO.File]::Exists($SrcIni)) {
            Add-Plan install $SrcIni $IniName '' "install the default $IniName"
            $text = Read-Text $SrcIni
        }
    } else {
        Write-Line "[=] Keeping the $iniRel that is already here"
        $text = Read-Text (Get-FullPath $iniRel)
    }

    if (-not $script:ReshadeRoute) { return }
    $values = @(Get-IniValues $text 'Plugins' 'LoadReshade')
    if ($values.Count -eq 1 -and $values[0] -ieq 'true') {
        Write-Line "[=] ${iniRel}: [Plugins] LoadReshade is already true"
        return
    }
    if (Test-Entry $iniRel) {
        $sha = Get-Sha (Get-FullPath $iniRel)
        if (-not ($script:FileSha.Contains($iniRel) -and $script:FileSha[$iniRel] -eq $sha)) {
            Add-Plan backup $iniRel '' '' "back up $iniRel to $($script:BackupRel)/ before editing it"
        }
    }
    $how = if ($values.Count -gt 0) { "replacing the $($values.Count) line(s) it already has" } else { 'the section has no such key, so one line is added' }
    Add-Plan setini $iniRel 'Plugins' 'LoadReshade=true' "set [Plugins] LoadReshade=true in $iniRel ($how)"
}

# Stand our libxell down when the game ships its own. XeLL_Proxy::InitXeLLProper points one at the other with
# RedirectAllExports, and a missing export becomes a null function pointer: Dying Light: The Beast crashed at
# startup three launches in four. Renamed, never deleted, and the rename is in the receipt.
function Add-LibxellPlan {
    $ours = "$SupportDir/libxell.dll"
    $theirs = 'libxell.dll'
    $oursThere = $script:Planned.Contains($ours) -or (Test-RegularFile $ours)
    if ((Test-RegularFile $theirs) -and $oursThere) {
        $oursSha = if ($script:Planned.Contains($ours)) { $script:Planned[$ours] } else { Get-Sha (Get-FullPath $ours) }
        if ((Get-Sha (Get-FullPath $theirs)) -eq $oursSha) {
            Write-Line "[=] libxell.dll: the game's copy is ours, leaving it"
        } elseif (Test-Entry "$ours.disabled") {
            Write-Line "[!] libxell.dll: ours and $ours.disabled both exist; leaving both as they are"
        } else {
            Add-Plan rename $ours "$ours.disabled" '' 'libxell.dll: the game ships its own, ours renamed to .disabled'
        }
    } elseif (Test-RegularFile "$ours.disabled") {
        Write-Line '[=] libxell.dll: ours already disabled'
    }
}

# ------------------------------------------------------------------------------------------------------------
# Steps
# ------------------------------------------------------------------------------------------------------------

# Copied beside the target, then renamed over it, so a file is never rewritten in place.
function Copy-Into([string]$Src, [string]$Rel) {
    $target = Get-FullPath $Rel
    New-ParentDir $target
    $script:Incoming = New-TempPath 'incoming'
    [IO.File]::Copy($Src, $script:Incoming, $false)
    Move-Over $script:Incoming $target
    $script:Incoming = $null
}

function Backup-File([string]$Rel) {
    $copy = "$($script:BackupRel)/$Rel"
    $copyPath = Get-FullPath $copy
    New-ParentDir $copyPath
    $sha = Get-Sha (Get-FullPath $Rel)
    [IO.File]::Copy((Get-FullPath $Rel), $copyPath, $false)
    $script:Backups.Add("$sha`t$Rel`t$copy")
}

function Rename-Entry([string]$From, [string]$To) {
    if (Test-Entry $To) { Stop-Run 1 "$To appeared while installing; stopping" }
    $sha = Get-Sha (Get-FullPath $From)
    [IO.File]::Move((Get-FullPath $From), (Get-FullPath $To))
    $script:Renames.Add("$sha`t$From`t$To")
}

function Set-IniKey([string]$Rel, [string]$Section, [string]$KeyValue) {
    $path = Get-FullPath $Rel
    $text = if ([IO.File]::Exists($path)) { Read-Text $path } else { '' }
    $eq = $KeyValue.IndexOf('=')
    $out = Set-IniText $text $Section $KeyValue.Substring(0, $eq) $KeyValue.Substring($eq + 1)
    $script:Incoming = New-TempPath 'incoming'
    [IO.File]::WriteAllBytes($script:Incoming, $Latin1.GetBytes($out))
    Move-Over $script:Incoming $path
    $script:Incoming = $null
    Set-FileRecord $Rel (Get-Sha $path)
}

function Invoke-Step($Step) {
    switch -CaseSensitive ($Step.Kind) {
        'install' {
            Copy-Into $Step.A $Step.B
            Set-FileRecord $Step.B (Get-Sha (Get-FullPath $Step.B))
        }
        'keep' { Set-FileRecord $Step.A (Get-Sha (Get-FullPath $Step.A)) }
        'backup' { Backup-File $Step.A }
        'rename' { Rename-Entry $Step.A $Step.B }
        'setini' { Set-IniKey $Step.A $Step.B $Step.C }
        'remove' {
            [IO.File]::Delete((Get-FullPath $Step.A))
            Remove-FileRecord $Step.A
            $script:Removed.Add($Step.A)
        }
        'restore' {
            $f = $Step.A.Split([char[]]"`t", 3)
            New-ParentDir (Get-FullPath $f[1])
            [IO.File]::Move((Get-FullPath $f[2]), (Get-FullPath $f[1]))
            Remove-Line $script:Backups $Step.A
        }
        'unrename' {
            $f = $Step.A.Split([char[]]"`t", 3)
            New-ParentDir (Get-FullPath $f[1])
            [IO.File]::Move((Get-FullPath $f[2]), (Get-FullPath $f[1]))
            Remove-Line $script:Renames $Step.A
        }
        'forget-file' { Remove-FileRecord $Step.A }
        'forget-backup' { Remove-Line $script:Backups $Step.A }
        'forget-rename' { Remove-Line $script:Renames $Step.A }
        default { Stop-Run 1 "internal: unknown step $($Step.Kind)" }
    }
}

function Show-Plan {
    foreach ($step in $script:Plan) { Write-Line "[plan] $($step.Text)" }
    if ($script:Plan.Count -eq 0) { Write-Line '[plan] nothing to do' }
}

function Invoke-Plan([string]$RunningStatus) {
    $n = 0
    foreach ($step in $script:Plan) {
        $tag = if ($step.Kind -ceq 'keep' -or $step.Kind.StartsWith('forget-')) { '[=]' } else { '[+]' }
        Write-Line "$tag $($step.Text)"
        Invoke-Step $step
        Write-Receipt $RunningStatus
        $n++
        if ($env:RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP -eq "$n") {
            Write-ErrorLine "[-] RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP=${n}: stopping here, as a failed step would"
            Stop-Run 1
        }
    }
}

function Remove-EmptyDirs([string]$Root) {
    if (-not [IO.Directory]::Exists($Root)) { return }
    foreach ($dir in @([IO.Directory]::GetDirectories($Root))) { Remove-EmptyDirs $dir }
    if (@([IO.Directory]::GetFileSystemEntries($Root)).Count -eq 0) { [IO.Directory]::Delete($Root) }
}

function Test-ModelInPlace {
    if ([IO.File]::Exists((Get-FullPath $ModelName))) {
        Write-Line "[=] ${ModelName}: present, left as it is"
    } else {
        Write-Line "[!] $ModelName is not in this folder. Neural Rendering needs NVIDIA's model there; this installer never ships or copies it, so put your own copy beside the game's executable."
    }
}

# ------------------------------------------------------------------------------------------------------------
# Uninstall
# ------------------------------------------------------------------------------------------------------------

function Invoke-Uninstall {
    $script:RunMode = 'uninstall'
    Write-Line "[+] Uninstalling from: $($script:Game)"
    if (-not [IO.File]::Exists($script:ReceiptPath)) {
        Stop-Run 1 ("no receipt at $($script:ReceiptPath). Only what a receipt records is ever removed, so nothing was " +
            'touched; an install made by hand (or before receipts existed) has to be undone by hand.')
    }
    Read-Receipt
    $rels = New-Object 'Collections.Generic.List[string]'
    foreach ($n in $ProxyNames) { $rels.Add($n) }
    foreach ($rel in $script:FileSha.Keys) { $rels.Add($rel) }
    foreach ($line in @($script:Backups) + @($script:Renames)) {
        $f = $line.Split([char[]]"`t", 3)
        $rels.Add($f[1]); $rels.Add($f[2])
    }
    Assert-NotRunning $rels.ToArray()

    $gone = @{}
    $back = @{}
    $existsAfter = {
        param([string]$Rel)
        if ($back.Contains($Rel)) { return $true }
        if ($gone.Contains($Rel)) { return $false }
        return (Test-Entry $Rel)
    }

    # Files first: ours come out only while they are still what was written.
    foreach ($rel in @($script:FileSha.Keys)) {
        if (Test-ModelPath $rel) {
            Write-Line "[!] ${rel}: NVIDIA's model is never removed by this script; left in place"
        } elseif (-not (Test-Entry $rel)) {
            Add-Plan forget-file $rel '' '' "${rel}: already gone"
        } elseif ((Test-RegularFile $rel) -and (Get-Sha (Get-FullPath $rel)) -eq $script:FileSha[$rel]) {
            Add-Plan remove $rel '' '' "remove $rel"
            $gone[$rel] = $true
        } else {
            Write-Line "[!] ${rel}: left in place, it changed since it was installed"
        }
    }
    # Then what was set aside, newest first, so the state just before the last install comes back.
    for ($i = $script:Backups.Count - 1; $i -ge 0; $i--) {
        $line = $script:Backups[$i]
        $f = $line.Split([char[]]"`t", 3)
        $rel = $f[1]
        $copy = $f[2]
        if ((Test-ModelPath $rel) -or (Test-ModelPath $copy)) {
            Write-Line "[!] ${rel}: NVIDIA's model is never moved by this script; left as it is"
        } elseif (-not (Test-Entry $copy)) {
            Add-Plan forget-backup $line '' '' "${rel}: its copy $copy is gone, nothing to restore"
        } elseif (& $existsAfter $rel) {
            Write-Line "[!] ${rel}: not restored, a file is still there; the earlier one stays at $copy"
        } else {
            Add-Plan restore $line '' '' "restore $rel from $copy"
            $back[$rel] = $true
        }
    }
    # Then renames, newest first.
    for ($i = $script:Renames.Count - 1; $i -ge 0; $i--) {
        $line = $script:Renames[$i]
        $f = $line.Split([char[]]"`t", 3)
        $sha = $f[0]
        $from = $f[1]
        $to = $f[2]
        if ((Test-ModelPath $from) -or (Test-ModelPath $to)) {
            Write-Line "[!] ${to}: NVIDIA's model is never moved by this script; left as it is"
        } elseif (& $existsAfter $from) {
            if (Test-Entry $to) {
                Write-Line "[!] ${to}: not renamed back, $from is in the way"
            } else {
                Add-Plan forget-rename $line '' '' "${from}: already back in place"
            }
        } elseif ((Test-RegularFile $to) -and (Get-Sha (Get-FullPath $to)) -eq $sha) {
            Add-Plan unrename $line '' '' "rename $to back to $from"
            $back[$from] = $true
            $gone[$to] = $true
        } elseif (-not (Test-Entry $to)) {
            Add-Plan forget-rename $line '' '' "${to}: gone, nothing to rename back to $from"
        } else {
            Write-Line "[!] ${to}: not renamed back to $from, it changed since"
        }
    }

    if ($DryRun) {
        Show-Plan
        Write-Line '[=] Dry run: nothing was written.'
        return
    }

    $script:ReceiptOpen = $true
    Invoke-Plan 'uninstall-incomplete'
    $script:ReceiptOpen = $false

    # Folders the install created (OptiScaler\ and below) go when the last file in them has gone.
    foreach ($rel in $script:Removed) {
        $dir = [IO.Path]::GetDirectoryName((Get-FullPath $rel))
        while ($dir.StartsWith($script:Game + $Sep, $PathCompare) -and [IO.Directory]::Exists($dir) -and
            @([IO.Directory]::GetFileSystemEntries($dir)).Count -eq 0) {
            [IO.Directory]::Delete($dir)
            $dir = [IO.Path]::GetDirectoryName($dir)
        }
    }
    Remove-EmptyDirs ([IO.Path]::Combine($script:StateDir, 'backup'))
    if ($script:FileSha.Count + $script:Backups.Count + $script:Renames.Count -eq 0) {
        [IO.File]::Delete($script:ReceiptPath)
        try { [IO.Directory]::Delete($script:StateDir) } catch { Write-Line "[!] $($script:StateDir) was left: it holds files no receipt records" }
        Write-Line ''
        Write-Line "[ok] Uninstalled: nothing this script recorded is left in $($script:Game)"
    } else {
        Write-Receipt 'uninstall-incomplete'
        Write-Line ''
        Write-Line '[!] Uninstalled what could be; the notes above say what was left and why.'
        Write-Line "    $($script:ReceiptPath) still lists it."
    }
}

# ------------------------------------------------------------------------------------------------------------
# Install
# ------------------------------------------------------------------------------------------------------------

function Invoke-Deploy {
    $src = if ($SourceDir) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($SourceDir) } else { $PSScriptRoot }
    $src = [IO.Path]::GetFullPath($src)
    $srcOpti = [IO.Path]::Combine($src, 'OptiScaler.dll')
    $srcFwd = [IO.Path]::Combine($src, $ForwarderName)
    if (-not [IO.File]::Exists($srcOpti)) {
        Stop-Run 1 "$srcOpti not found. Run this from the folder the release zip was extracted to, or pass -SourceDir."
    }
    $shaOpti = Get-Sha $srcOpti
    if (-not (Test-Marker $srcOpti $MarkOptiScaler)) {
        Write-Line "[!] $srcOpti does not contain '$MarkOptiScaler'; later installs will not recognise it as OptiScaler, and the second-proxy check depends on that string"
    }
    if (-not [IO.File]::Exists($srcFwd)) { Stop-Run 1 "$srcFwd not found. Extract the whole release zip, then run this again." }
    $shaFwd = Get-Sha $srcFwd
    $support = @(Get-SupportFiles $src)

    Write-Line "[+] Installing OptiScaler & DLSS-NR to: $($script:Game)"
    Write-Line "[+] Using proxy filename: $($script:Proxy)"
    if (@([IO.Directory]::GetFiles($script:Game, '*.exe')).Count -eq 0) {
        Write-Line "[!] There is no .exe in this folder. OptiScaler goes beside the game's executable (Unreal Engine: <Game>\Binaries\Win64)."
    }

    Read-Receipt
    $rels = New-Object 'Collections.Generic.List[string]'
    foreach ($n in $ProxyNames + @('OptiScaler.dll', $ForwarderName, $IniName, 'ReShade64.dll', 'd3d12-ReShade64.dll', 'libxell.dll', "$SupportDir/libxell.dll")) { $rels.Add($n) }
    foreach ($pair in $support) { $rels.Add($pair.Rel) }
    foreach ($rel in $script:FileSha.Keys) { $rels.Add($rel) }
    Assert-NotRunning $rels.ToArray()
    if ($script:OldStatus -ceq 'partial') {
        Write-Line '[!] The last install here stopped part-way; its records are kept and this one carries on from them'
    }
    Select-BackupDir

    # 1. OptiScaler as the proxy DLL, and as OptiScaler.dll beside it
    Add-DllPlan $script:Proxy $srcOpti $MarkOptiScaler $shaOpti $true
    Add-DllPlan 'OptiScaler.dll' $srcOpti $MarkOptiScaler $shaOpti $false
    # 2. The DLSS-NR forwarder
    Add-DllPlan $ForwarderName $srcFwd $MarkForwarder $shaFwd $false
    # 3. The OptiScaler\ support folder
    Add-SupportPlan $support
    # 4. No second OptiScaler under another proxy name
    Test-OtherProxies
    # 5. OptiScaler.ini: installed if absent, otherwise kept; edited only for -KeepReShade
    Add-IniPlan ([IO.Path]::Combine($src, $IniName))
    # 6. libxell
    Add-LibxellPlan

    Stop-IfRefused

    if ($DryRun) {
        Show-Plan
    } else {
        if (-not [IO.Directory]::Exists($script:StateDir)) { [void][IO.Directory]::CreateDirectory($script:StateDir) }
        Add-Proxy $script:Proxy
        $script:ReceiptOpen = $true
        Write-Receipt 'partial'
        Invoke-Plan 'partial'
        Write-Receipt 'complete'
        $script:ReceiptOpen = $false
    }

    Test-ModelInPlace
    if ($DryRun) {
        Write-Line '[=] Dry run: nothing was written.'
        return
    }

    Write-Line ''
    Write-Line '[ok] Installed.'
    Write-Line '    Files this install owns:'
    foreach ($rel in $script:FileSha.Keys) { Write-Line "      - $(Get-FullPath $rel)" }
    if ($script:ReshadeRoute) { Write-Line '    ReShade is kept beside it and loaded by OptiScaler ([Plugins] LoadReshade=true).' }
    Write-Line "    Receipt: $($script:ReceiptPath)"
    Write-Line "    Undo: install-windows.cmd -Uninstall -GameDir `"$($script:Game)`""
    Write-Line '    Neural Rendering stays off until [DlssNr] Enabled=true in OptiScaler.ini (change the line that is there).'
}

# ------------------------------------------------------------------------------------------------------------
# Main
# ------------------------------------------------------------------------------------------------------------

function Invoke-Main {
    if ($Help) { Show-Usage; return }
    if (-not $GameDir -and -not [Console]::IsInputRedirected -and [Environment]::UserInteractive) {
        Show-Usage
        $GameDir = (Read-Host "Game folder (the one with the game's .exe)").Trim().Trim('"')
    }
    if (-not $GameDir) {
        Show-Usage
        Stop-Run 1 'no game folder given (-GameDir)'
    }
    if ($Uninstall -and $ScriptParams.ContainsKey('ProxyName')) {
        Stop-Run 1 '-Uninstall takes only the game folder; the proxy name comes from the receipt'
    }
    $canonical = @($ProxyNames | Where-Object { $_ -ieq $ProxyName })
    if ($canonical.Count -eq 0) {
        Stop-Run 1 "$ProxyName is not a name this installer uses; pick one of $($ProxyNames -join ', ')"
    }
    $script:Proxy = $canonical[0]

    $full = [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($GameDir))
    if ($full.Length -gt [IO.Path]::GetPathRoot($full).Length) { $full = $full.TrimEnd([char[]]'\/') }
    if (-not [IO.Directory]::Exists($full)) { Stop-Run 1 "Target directory does not exist: $full" }
    $script:Game = $full
    $script:StateDir = [IO.Path]::Combine($full, '.retoque')
    $script:ReceiptPath = [IO.Path]::Combine($script:StateDir, 'receipt.txt')

    if ($Uninstall) { Invoke-Uninstall } else { Invoke-Deploy }
}

$exitCode = 1
try {
    Invoke-Main
    $exitCode = 0
} catch {
    if ("$($_.Exception.Message)" -ceq 'retoque-stop') {
        $exitCode = $script:StopCode
    } else {
        $e = Get-Inner $_.Exception
        Write-ErrorLine "[-] Error: $($e.Message)"
        if ($e -is [UnauthorizedAccessException]) {
            Write-ErrorLine '    Windows refused access. If the game is under Program Files, run this from an administrator prompt.'
        }
        $exitCode = 1
    }
} finally {
    if ($script:Incoming -and [IO.File]::Exists($script:Incoming)) {
        try { [IO.File]::Delete($script:Incoming) } catch { Write-ErrorLine "[!] could not remove $($script:Incoming)" }
    }
    if ($exitCode -ne 0 -and $script:ReceiptOpen) {
        if ($script:RunMode -eq 'deploy') {
            try { Write-Receipt 'partial' } catch { Write-ErrorLine '[!] the receipt could not be rewritten' }
            Write-ErrorLine ("[-] Install stopped part-way (exit $exitCode). Everything it did is in $($script:ReceiptPath) " +
                '(status partial); -Uninstall takes it back out, or run the install again.')
        } else {
            try { Write-Receipt 'uninstall-incomplete' } catch { Write-ErrorLine '[!] the receipt could not be rewritten' }
            Write-ErrorLine "[-] Uninstall stopped part-way (exit $exitCode). $($script:ReceiptPath) lists what is left."
        }
    }
}
exit $exitCode
