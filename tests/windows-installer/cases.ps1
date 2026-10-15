# tools/install-windows.ps1 against fake game folders, in PowerShell 7 on Linux. run.py stages this file with
# tools/install-windows.ps1, tools/deploy-to-game.sh and OptiScaler.ini under one root and runs it there,
# normally in a container; the root is the only argument.
#
# The fake DLLs are a few bytes carrying the strings both scripts recognise builds by, never a real binary.
# Every case runs the real scripts as child processes and checks exit status, files, receipt and output.
param([Parameter(Mandatory = $true)] [string]$Root)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$Installer = Join-Path $Root 'tools/install-windows.ps1'
$LinuxScript = Join-Path $Root 'tools/deploy-to-game.sh'
$ShippedIni = Join-Path $Root 'OptiScaler.ini'
$Pwsh = (Get-Process -Id $PID).Path
$Ascii = [Text.Encoding]::ASCII

# Exit codes both scripts document.
$RefusedRunning = 3
$RefusedForeign = 4
$RefusedSecondProxy = 5

# The markers mirror a real build: OptiScaler carries CheckWorkingMode's line and also names ReShade64.dll and
# the forwarder's exports, which is exactly what must not make it look like ReShade.
$PE = [byte[]](@(0x4D, 0x5A, 0x90, 0x00) + @([byte]0) * 60)
function New-Opti([string]$Tag) { , [byte[]]($PE + $Ascii.GetBytes("OptiScaler working as %s`0ReShade64.dll`0dlssnr_call_create`0$Tag")) }
function New-Forwarder([string]$Tag) { , [byte[]]($PE + $Ascii.GetBytes("dlssnr_call_create`0dlssnr_abi_version`0$Tag")) }
$ReShade = [byte[]]($PE + $Ascii.GetBytes("Initializing crosire's ReShade version '%s'`0"))
$Foreign = [byte[]]($PE + $Ascii.GetBytes("some other dxgi wrapper`0"))
# CRLF, the key twice in [Plugins] in other spellings, and the same key in another section.
$UserIni = "[Log]`r`nLogLevel=2`r`n`r`n[Plugins]`r`n; LoadReshade=false in a comment`r`nloadreshade = auto`r`nLoadAsiPlugins=auto`r`nLoadReShade=false`r`n`r`n[Other]`r`nLoadReshade=false`r`n"

$script:Last = $null

function Expect([bool]$Condition, [string]$Message) { if (-not $Condition) { throw "expected: $Message" } }

function Get-Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

function Invoke-Tool([string]$File, [string[]]$Arguments, [hashtable]$Extra = @{}) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $File
    foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    foreach ($k in @($psi.Environment.Keys)) { if ($k.StartsWith('RETOQUE_DEPLOY_')) { [void]$psi.Environment.Remove($k) } }
    foreach ($k in $Extra.Keys) { $psi.Environment[$k] = $Extra[$k] }
    $p = [Diagnostics.Process]::Start($psi)
    $p.StandardInput.Close()
    $out = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(120000)) { $p.Kill(); throw "timed out: $File $Arguments" }
    $p.WaitForExit()
    $script:Last = [pscustomobject]@{ Code = $p.ExitCode; Out = $out.Result; Err = $err.Result; Cmd = "$File $($Arguments -join ' ')" }
    return $script:Last
}

# The installer as a user runs it. Every line it prints to stdout starts with a tag or an indent, so a stray
# line is output leaking from a function (a .NET call whose result nobody discarded), and fails the case.
function Invoke-Installer([string]$Game, [string[]]$Arguments = @(), [string]$Src = '', [hashtable]$Extra = @{}, [string]$Script = $Installer, [switch]$Usage) {
    $a = @('-NoProfile', '-NonInteractive', '-File', $Script)
    if ($Game) { $a += @('-GameDir', $Game) }
    if ($Src) { $a += @('-SourceDir', $Src) }
    $a += $Arguments
    $r = Invoke-Tool $Pwsh $a $Extra
    if (-not $Usage) {
        $stray = @($r.Out.Split("`n") | Where-Object { $_ -ne '' -and $_ -notmatch '^(\[|\s)' })
        Expect ($stray.Count -eq 0) "no stray stdout lines, got: $($stray -join ' | ')"
    }
    return $r
}

function Invoke-Linux([string]$Game, [string[]]$Arguments = @(), [string]$Src = '', [hashtable]$Extra = @{}) {
    $a = @($LinuxScript)
    if ($Src) { $a += @('--source-dir', $Src) }
    $a += $Arguments
    $a += $Game
    return Invoke-Tool 'bash' $a $Extra
}

function Install-Ok([string]$Game, [string[]]$Arguments = @(), [string]$Src = '') {
    $r = Invoke-Installer $Game $Arguments -Src $Src
    Expect ($r.Code -eq 0) "install $($Arguments -join ' ') to exit 0, got $($r.Code)"
    return $r
}

function Uninstall-Ok([string]$Game) {
    $r = Invoke-Installer $Game @('-Uninstall')
    Expect ($r.Code -eq 0) "uninstall to exit 0, got $($r.Code)"
    return $r
}

function Write-Files([string]$Dir, $Files) {
    [void][IO.Directory]::CreateDirectory($Dir)
    foreach ($rel in @($Files.Keys)) {
        $path = Join-Path $Dir $rel
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
        $data = $Files[$rel]
        if ($data -is [string]) { [IO.File]::WriteAllText($path, $data) } else { [IO.File]::WriteAllBytes($path, [byte[]]$data) }
    }
}

# A release as the zip extracts it: the two DLLs, the shipped ini, and the OptiScaler\ support folder.
function New-Src([string]$Tmp, [string]$Tag, [switch]$NoSupport) {
    $src = Join-Path $Tmp "src-$Tag"
    Write-Files $src ([ordered]@{ 'OptiScaler.dll' = (New-Opti $Tag); 'nvngx.dll_dlssnr.dll' = (New-Forwarder $Tag) })
    [IO.File]::Copy($ShippedIni, (Join-Path $src 'OptiScaler.ini'))
    if (-not $NoSupport) {
        Write-Files $src ([ordered]@{
                'OptiScaler/libxess.dll'                    = "xess $Tag"
                'OptiScaler/libxell.dll'                    = "xell $Tag"
                'OptiScaler/D3D12_OptiScaler/D3D12Core.dll' = "agility $Tag"
            })
    }
    return $src
}

# Brackets and spaces in every game path: the cmdlets' -Path reads "[x]" as a wildcard.
function New-Game([string]$Tmp, $Files = @{}, [string]$Name = 'game [x] dir') {
    $game = Join-Path $Tmp $Name
    Write-Files $game $Files
    return $game
}

# Every entry under $Dir, hidden ones included: 'dir' or the content hash. Ordinal keys: on Linux DXGI.DLL and
# dxgi.dll are two files.
function Get-Snapshot([string]$Dir) {
    $snap = New-Object 'Collections.Generic.SortedDictionary[string,string]' ([StringComparer]::Ordinal)
    if (Test-Path -LiteralPath $Dir) {
        foreach ($item in @(Get-ChildItem -LiteralPath $Dir -Recurse -Force)) {
            $rel = $item.FullName.Substring($Dir.Length + 1)
            $snap[$rel] = if ($item.PSIsContainer) { 'dir' } else { 'file ' + (Get-Sha $item.FullName) }
        }
    }
    return , $snap
}

function Get-SnapshotDiff($Before, $After) {
    $diff = @()
    foreach ($k in $Before.Keys) {
        if (-not $After.ContainsKey($k)) { $diff += "-$k" } elseif ($After[$k] -cne $Before[$k]) { $diff += "~$k" }
    }
    foreach ($k in $After.Keys) { if (-not $Before.ContainsKey($k)) { $diff += "+$k" } }
    return , $diff
}

function Expect-Unchanged($Before, [string]$Game, [string]$What) {
    $diff = Get-SnapshotDiff $Before (Get-Snapshot $Game)
    Expect ($diff.Count -eq 0) "${What}: the folder unchanged, got $($diff -join ', ')"
}

function Read-ReceiptFile([string]$Game) {
    $path = Join-Path $Game '.retoque/receipt.txt'
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $bytes = [IO.File]::ReadAllBytes($path)
    $r = [pscustomobject]@{
        Status  = $null
        Files   = (New-Object 'Collections.Generic.Dictionary[string,string]' ([StringComparer]::Ordinal))
        Backups = New-Object 'Collections.Generic.List[object]'
        Renames = New-Object 'Collections.Generic.List[object]'
        Proxies = New-Object 'Collections.Generic.List[string]'
        Bytes   = $bytes
        Text    = [Text.Encoding]::UTF8.GetString($bytes)
    }
    foreach ($line in $r.Text.Split("`n")) {
        if ($line -eq '' -or $line.StartsWith('#')) { continue }
        $f = $line.Split("`t")
        switch -CaseSensitive ($f[0]) {
            'status' { $r.Status = $f[1] }
            'proxy' { $r.Proxies.Add($f[1]) }
            'file' { $r.Files[$f[2]] = $f[1] }
            'backup' { $r.Backups.Add(@($f[1], $f[2], $f[3])) }
            'rename' { $r.Renames.Add(@($f[1], $f[2], $f[3])) }
        }
    }
    return $r
}

# No temp file left in .retoque, nothing set aside beside the files.
function Expect-CleanStateDir([string]$Game) {
    $state = Join-Path $Game '.retoque'
    if (Test-Path -LiteralPath $state) {
        $stray = @(Get-ChildItem -LiteralPath $state -Force | Where-Object { $_.Name -match '^(incoming|receipt)\.' -and $_.Name -ne 'receipt.txt' })
        Expect ($stray.Count -eq 0) "no temp files in .retoque, got $(($stray | ForEach-Object Name) -join ', ')"
    }
    $siblings = @(Get-ChildItem -LiteralPath $Game -Force | Where-Object { $_.Name -match '\.(bak|orig|old)$' })
    Expect ($siblings.Count -eq 0) "no sibling backups, got $(($siblings | ForEach-Object Name) -join ', ')"
}

function Get-PluginsLines([string]$Text) {
    $section = $null
    $hits = @()
    foreach ($line in $Text.Split("`n")) {
        $s = $line.Trim()
        if ($s.StartsWith('[') -and $s.EndsWith(']')) { $section = $s.Substring(1, $s.Length - 2).Trim().ToLowerInvariant() }
        elseif ($section -eq 'plugins' -and $s.Contains('=') -and -not ($s.StartsWith(';') -or $s.StartsWith('#'))) {
            if ($s.Split('=', 2)[0].Trim().ToLowerInvariant() -eq 'loadreshade') { $hits += $line.TrimEnd("`r") }
        }
    }
    return , $hits
}

function Read-Bytes([string]$Path) { , [IO.File]::ReadAllBytes($Path) }

function Test-BytesEqual($A, $B) { [Linq.Enumerable]::SequenceEqual([byte[]]$A, [byte[]]$B) }

$Cases = New-Object 'Collections.Generic.List[object]'
function Add-Case([string]$Name, [scriptblock]$Body) { $Cases.Add([pscustomobject]@{ Name = $Name; Body = $Body }) }

# --------------------------------------------------------------------------------------------------------------

Add-Case 'scripts_parse_and_use_no_powershell7_only_syntax' {
    foreach ($file in @($Installer, $PSCommandPath)) {
        $tokens = $null
        $errors = $null
        $ast = [Management.Automation.Language.Parser]::ParseFile($file, [ref]$tokens, [ref]$errors)
        Expect ($errors.Count -eq 0) "$file parses, got: $($errors | ForEach-Object { $_.ToString() })"
        if ($file -ne $Installer) { continue }
        # The installer must also run on Windows PowerShell 5.1, which has none of these.
        $seven = @($ast.FindAll({ param($n)
                    $n.GetType().Name -in @('TernaryExpressionAst', 'PipelineChainAst') }, $true))
        Expect ($seven.Count -eq 0) "no ternary or && / || chains, got: $($seven | ForEach-Object { $_.Extent.Text })"
        $ops = @($tokens | Where-Object { $_.Kind.ToString() -in @('QuestionQuestion', 'QuestionQuestionEquals', 'QuestionDot', 'QuestionLBracket') })
        Expect ($ops.Count -eq 0) "no ?? or ?. operators, got: $($ops | ForEach-Object { $_.Text })"
        $vars = @($ast.FindAll({ param($n) $n -is [Management.Automation.Language.VariableExpressionAst] -and
                    $n.VariablePath.UserPath -in @('IsWindows', 'IsLinux', 'IsMacOS', 'IsCoreCLR') }, $true))
        Expect ($vars.Count -eq 0) 'no $IsWindows and friends (undefined in 5.1, an error under strict mode)'
        $joins = @($ast.FindAll({ param($n) $n -is [Management.Automation.Language.CommandAst] -and
                    $n.GetCommandName() -eq 'Join-Path' -and $n.CommandElements.Count -gt 3 }, $true))
        Expect ($joins.Count -eq 0) 'no Join-Path with more than one child (7 only)'
    }
}

Add-Case 'clean_install' {
    param($tmp)
    $src = New-Src $tmp 'v1'
    $game = New-Game $tmp
    $r = Install-Ok $game -Src $src
    Expect (Test-BytesEqual (Read-Bytes (Join-Path $game 'dxgi.dll')) (New-Opti 'v1')) 'dxgi.dll is the source build'
    Expect (Test-BytesEqual (Read-Bytes (Join-Path $game 'OptiScaler.dll')) (New-Opti 'v1')) 'OptiScaler.dll is the source build'
    Expect (Test-BytesEqual (Read-Bytes (Join-Path $game 'nvngx.dll_dlssnr.dll')) (New-Forwarder 'v1')) 'the forwarder is the source one'
    Expect ((Get-Sha (Join-Path $game 'OptiScaler.ini')) -eq (Get-Sha $ShippedIni)) 'the ini is the shipped one'
    foreach ($rel in @('OptiScaler/libxess.dll', 'OptiScaler/libxell.dll', 'OptiScaler/D3D12_OptiScaler/D3D12Core.dll')) {
        Expect ((Get-Sha (Join-Path $game $rel)) -eq (Get-Sha (Join-Path $src $rel))) "$rel installed from the release"
    }
    $rc = Read-ReceiptFile $game
    Expect ($null -ne $rc -and $rc.Status -eq 'complete') "receipt status complete, got $($rc.Status)"
    Expect (($rc.Proxies -join ',') -eq 'dxgi.dll') "receipt proxies dxgi.dll, got $($rc.Proxies -join ',')"
    $want = @('dxgi.dll', 'OptiScaler.dll', 'nvngx.dll_dlssnr.dll', 'OptiScaler/D3D12_OptiScaler/D3D12Core.dll',
        'OptiScaler/libxell.dll', 'OptiScaler/libxess.dll', 'OptiScaler.ini')
    Expect ((@($rc.Files.Keys) -join ',') -eq ($want -join ',')) "receipt files in install order, got $(@($rc.Files.Keys) -join ',')"
    foreach ($rel in $rc.Files.Keys) { Expect ((Get-Sha (Join-Path $game $rel)) -eq $rc.Files[$rel]) "receipt hash of $rel matches the file" }
    Expect ($rc.Backups.Count -eq 0 -and $rc.Renames.Count -eq 0) 'a clean install set nothing aside'
    Expect (-not (Test-Path -LiteralPath (Join-Path $game '.retoque/backup'))) 'a clean install made no backup directory'
    # The format the Linux script reads: no byte-order mark, LF only, version 1 first among the records.
    Expect ($rc.Bytes[0] -ne 0xEF -and $rc.Text.IndexOf("`r") -lt 0) 'receipt without BOM or CR'
    $records = @($rc.Text.Split("`n") | Where-Object { $_ -ne '' -and -not $_.StartsWith('#') })
    Expect ($records[0] -ceq "version`t1" -and $records[1] -ceq "status`tcomplete" -and $records[2] -cmatch "^updated`t\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$") "receipt header records, got $($records[0..2] -join ' | ')"
    Expect ($r.Out.Contains('nvngx_dlssnr.dll is not in this folder')) 'a missing model is reported'
    Expect-CleanStateDir $game
}

Add-Case 'positional_proxy_and_bad_names' {
    param($tmp)
    $src = New-Src $tmp 'v1'
    $game = New-Game $tmp
    $r = Invoke-Tool $Pwsh @('-NoProfile', '-NonInteractive', '-File', $Installer, $game, 'WINMM.DLL', '-SourceDir', $src)
    Expect ($r.Code -eq 0) "positional install exits 0, got $($r.Code)"
    Expect (Test-Path -LiteralPath (Join-Path $game 'winmm.dll')) 'winmm.dll installed'
    Expect (-not (Test-Path -LiteralPath (Join-Path $game 'dxgi.dll'))) 'no dxgi.dll when the proxy was winmm.dll'
    Expect (((Read-ReceiptFile $game).Proxies -join ',') -ceq 'winmm.dll') 'receipt names winmm.dll, in its canonical spelling'
    $before = Get-Snapshot $game
    foreach ($bad in @('dxgi', 'OptiScaler.asi', '..\dxgi.dll')) {
        $r = Invoke-Installer $game @('-ProxyName', $bad) -Src $src
        Expect ($r.Code -eq 1) "proxy name '$bad' exits 1, got $($r.Code)"
    }
    $r = Invoke-Installer $game @('-Uninstall', '-ProxyName', 'dxgi.dll')
    Expect ($r.Code -eq 1 -and $r.Err.Contains('proxy name comes from the receipt')) '-Uninstall with -ProxyName exits 1'
    $r = Invoke-Installer (Join-Path $tmp 'nowhere') @() -Src $src
    Expect ($r.Code -eq 1) 'a missing game folder exits 1'
    Expect-Unchanged $before $game 'rejected arguments'
}

Add-Case 'usage_without_a_folder' {
    param($tmp)
    $r = Invoke-Installer '' @() -Usage
    Expect ($r.Code -eq 1 -and $r.Out.Contains('Usage:') -and $r.Err.Contains('no game folder')) 'no -GameDir prints usage and exits 1'
    $r = Invoke-Installer '' @('-Help') -Usage
    Expect ($r.Code -eq 0 -and $r.Out.Contains('Usage:')) '-Help prints usage and exits 0'
}

Add-Case 'reinstall_over_own_build' {
    param($tmp)
    $game = New-Game $tmp
    Install-Ok $game -Src (New-Src $tmp 'v1') | Out-Null
    $ini = Join-Path $game 'OptiScaler.ini'
    [IO.File]::WriteAllText($ini, [IO.File]::ReadAllText($ini).Replace('LogToFile=auto', 'LogToFile=true'))
    $userIni = Get-Sha $ini
    $r = Install-Ok $game -Src (New-Src $tmp 'v2')
    Expect (Test-BytesEqual (Read-Bytes (Join-Path $game 'dxgi.dll')) (New-Opti 'v2')) 'dxgi.dll replaced by v2'
    Expect (Test-BytesEqual (Read-Bytes (Join-Path $game 'nvngx.dll_dlssnr.dll')) (New-Forwarder 'v2')) 'forwarder replaced by v2'
    Expect ((Get-Content -LiteralPath (Join-Path $game 'OptiScaler/libxess.dll') -Raw) -eq 'xess v2') 'support file replaced by v2'
    Expect ((Get-Sha $ini) -eq $userIni) 'the user-edited ini preserved'
    $rc = Read-ReceiptFile $game
    Expect ($rc.Status -eq 'complete' -and $rc.Files['dxgi.dll'] -eq (Get-Sha (Join-Path $game 'dxgi.dll'))) 'receipt holds the v2 hash'
    Expect ($rc.Backups.Count -eq 0) 'our own earlier build was not backed up as if foreign'
    Expect ($r.Out.Contains('replace dxgi.dll (installed here earlier, unchanged since)')) 'the replace is explained'
    Expect-CleanStateDir $game
}

Add-Case 'reinstall_identical_writes_nothing' {
    param($tmp)
    $src = New-Src $tmp 'v1'
    $game = New-Game $tmp
    Install-Ok $game -Src $src | Out-Null
    $dll = Join-Path $game 'dxgi.dll'
    $old = [DateTime]::new(2001, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
    [IO.File]::SetLastWriteTimeUtc($dll, $old)
    $r = Install-Ok $game -Src $src
    Expect ([IO.File]::GetLastWriteTimeUtc($dll) -eq $old) 'an identical build was not rewritten'
    Expect ($r.Out.Contains('dxgi.dll: already this build')) 'an "already this build" note'
}

Add-Case 'earlier_build_without_receipt_backed_up_and_restored' {
    param($tmp)
    # What a hand install leaves: OptiScaler builds, an older support file, and no receipt.
    $game = New-Game $tmp ([ordered]@{ 'dxgi.dll' = (New-Opti 'old'); 'OptiScaler.dll' = (New-Opti 'old')
            'nvngx.dll_dlssnr.dll' = (New-Forwarder 'old'); 'OptiScaler/libxess.dll' = 'xess old'; 'game.exe' = $PE
        })
    $before = Get-Snapshot $game
    Install-Ok $game -Src (New-Src $tmp 'v1') | Out-Null
    $rc = Read-ReceiptFile $game
    $backed = @($rc.Backups | ForEach-Object { $_[1] } | Sort-Object)
    Expect (($backed -join ',') -eq 'dxgi.dll,nvngx.dll_dlssnr.dll,OptiScaler.dll,OptiScaler/libxess.dll') "backups, got $($backed -join ',')"
    foreach ($b in $rc.Backups) {
        Expect ($b[2].StartsWith('.retoque/backup/')) "backup inside .retoque/backup, got $($b[2])"
        Expect ((Get-Sha (Join-Path $game $b[2])) -eq $b[0] -and ('file ' + $b[0]) -eq $before[$b[1].Replace('/', [IO.Path]::DirectorySeparatorChar)]) "the backup of $($b[1]) holds what was there"
    }
    Expect-CleanStateDir $game
    Uninstall-Ok $game | Out-Null
    Expect-Unchanged $before $game 'uninstall restores the earlier build'
}

Add-Case 'foreign_dxgi_refused' {
    param($tmp)
    $game = New-Game $tmp @{ 'dxgi.dll' = $Foreign }
    $before = Get-Snapshot $game
    $src = New-Src $tmp 'v1'
    $r = Invoke-Installer $game @() -Src $src
    Expect ($r.Code -eq $RefusedForeign) "exit $RefusedForeign, got $($r.Code)"
    Expect ($r.Err.Contains('dxgi.dll is not ours') -and $r.Err.Contains('-ReplaceForeign')) 'the refusal names the file and the flag'
    Expect-Unchanged $before $game 'a refused install'
    $r = Invoke-Installer $game @('-KeepReShade') -Src $src
    Expect ($r.Code -eq $RefusedForeign -and $r.Err.Contains('not ReShade')) '-KeepReShade does not let a non-ReShade dxgi.dll through'
    $r = Invoke-Installer $game @('-DryRun') -Src $src
    Expect ($r.Code -eq $RefusedForeign) 'a dry run refuses with the same code'
    Expect-Unchanged $before $game 'refused runs'
}

Add-Case 'replace_foreign_then_uninstall' {
    param($tmp)
    $game = New-Game $tmp ([ordered]@{ 'dxgi.dll' = $Foreign; 'game.exe' = $PE })
    $before = Get-Snapshot $game
    Install-Ok $game @('-ReplaceForeign') -Src (New-Src $tmp 'v1') | Out-Null
    $rc = Read-ReceiptFile $game
    Expect ($rc.Backups.Count -eq 1 -and $rc.Backups[0][1] -eq 'dxgi.dll') 'one backup, of dxgi.dll'
    Expect (Test-BytesEqual (Read-Bytes (Join-Path $game $rc.Backups[0][2])) $Foreign) 'the backup holds the original'
    Uninstall-Ok $game | Out-Null
    Expect-Unchanged $before $game 'uninstall after -ReplaceForeign'
}

Add-Case 'reshade_refused_without_flag' {
    param($tmp)
    $game = New-Game $tmp @{ 'dxgi.dll' = $ReShade }
    $before = Get-Snapshot $game
    $r = Invoke-Installer $game @() -Src (New-Src $tmp 'v1')
    Expect ($r.Code -eq $RefusedForeign -and $r.Err.Contains('-KeepReShade')) 'ReShade refused, -KeepReShade offered'
    Expect-Unchanged $before $game 'a refused install'
}

Add-Case 'keep_reshade_with_shipped_ini' {
    param($tmp)
    $game = New-Game $tmp @{ 'dxgi.dll' = $ReShade }
    $before = Get-Snapshot $game
    Install-Ok $game @('-KeepReShade') -Src (New-Src $tmp 'v1') | Out-Null
    Expect (Test-BytesEqual (Read-Bytes (Join-Path $game 'ReShade64.dll')) $ReShade) 'ReShade64.dll is the original ReShade'
    Expect (Test-BytesEqual (Read-Bytes (Join-Path $game 'dxgi.dll')) (New-Opti 'v1')) 'dxgi.dll is ours'
    $shipped = [IO.File]::ReadAllText($ShippedIni).Split("`n")
    $edited = [IO.File]::ReadAllText((Join-Path $game 'OptiScaler.ini')).Split("`n")
    Expect ((Get-PluginsLines ([IO.File]::ReadAllText($ShippedIni))).Count -eq 1) 'the shipped ini has one [Plugins] LoadReshade'
    Expect ($shipped.Count -eq $edited.Count) "line count kept ($($shipped.Count)), got $($edited.Count): a key was inserted"
    $changed = @(for ($i = 0; $i -lt $shipped.Count; $i++) { if ($shipped[$i] -cne $edited[$i]) { $edited[$i] } })
    Expect ($changed.Count -eq 1 -and $changed[0].Trim() -ceq 'LoadReshade=true') "one line changed to LoadReshade=true, got $($changed -join ' | ')"
    $rc = Read-ReceiptFile $game
    Expect ($rc.Renames.Count -eq 1 -and $rc.Renames[0][1] -eq 'dxgi.dll' -and $rc.Renames[0][2] -eq 'ReShade64.dll') 'the rename is recorded'
    Expect ($rc.Files['OptiScaler.ini'] -eq (Get-Sha (Join-Path $game 'OptiScaler.ini'))) 'the receipt holds the edited ini hash'
    # A second install keeps it that way and adds nothing.
    Install-Ok $game @('-KeepReShade') -Src (New-Src $tmp 'v2') | Out-Null
    $lines = Get-PluginsLines ([IO.File]::ReadAllText((Join-Path $game 'OptiScaler.ini')))
    Expect ($lines.Count -eq 1 -and $lines[0] -ceq 'LoadReshade=true') 'a re-install left the key alone'
    Expect ((Read-ReceiptFile $game).Renames.Count -eq 1) 'no second rename recorded'
    Uninstall-Ok $game | Out-Null
    Expect-Unchanged $before $game 'uninstall puts ReShade back as dxgi.dll alone'
}

Add-Case 'keep_reshade_edits_user_ini_in_place' {
    param($tmp)
    $game = New-Game $tmp ([ordered]@{ 'dxgi.dll' = $ReShade; 'OptiScaler.ini' = $UserIni })
    $before = Get-Snapshot $game
    Install-Ok $game @('-KeepReShade') -Src (New-Src $tmp 'v1') | Out-Null
    $text = [IO.File]::ReadAllText((Join-Path $game 'OptiScaler.ini'))
    Expect ($text.Split("`n").Count -eq $UserIni.Split("`n").Count) 'line count kept'
    Expect (($text.Split("`n").Count - 1) -eq ([regex]::Matches($text, "`r`n")).Count) 'CRLF line ends kept'
    $plugins = Get-PluginsLines $text
    Expect ($plugins.Count -eq 2 -and @($plugins | Where-Object { $_.Split('=', 2)[1].Trim() -ne 'true' }).Count -eq 0) "both [Plugins] lines true, got $($plugins -join ' | ')"
    Expect ($text.Contains("loadreshade =true`r`n") -and $text.Contains("LoadReShade=true`r`n")) 'each line keeps its own spelling'
    Expect ($text.Contains("[Other]`r`nLoadReshade=false")) 'the key in another section untouched'
    Expect ($text.Contains('; LoadReshade=false in a comment')) 'the comment untouched'
    $iniBackups = @((Read-ReceiptFile $game).Backups | Where-Object { $_[1] -eq 'OptiScaler.ini' })
    Expect ($iniBackups.Count -eq 1 -and [IO.File]::ReadAllText((Join-Path $game $iniBackups[0][2])) -ceq $UserIni) 'the user ini backed up before the edit'
    Uninstall-Ok $game | Out-Null
    Expect-Unchanged $before $game 'uninstall restores the user ini and ReShade'
}

Add-Case 'keep_reshade_adds_a_missing_key_once' {
    param($tmp)
    $src = New-Src $tmp 'v1'
    $game = New-Game $tmp ([ordered]@{ 'dxgi.dll' = $ReShade; 'OptiScaler.ini' = "[Plugins]`nLoadAsiPlugins=auto`n" })
    Install-Ok $game @('-KeepReShade') -Src $src | Out-Null
    $got = [IO.File]::ReadAllText((Join-Path $game 'OptiScaler.ini'))
    Expect ($got -ceq "[Plugins]`nLoadReshade=true`nLoadAsiPlugins=auto`n") "key added under the header, got $got"
    $game = New-Game $tmp ([ordered]@{ 'dxgi.dll' = $ReShade; 'OptiScaler.ini' = "[Log]`nLogLevel=2" }) 'second [x]'
    Install-Ok $game @('-KeepReShade') -Src $src | Out-Null
    $got = [IO.File]::ReadAllText((Join-Path $game 'OptiScaler.ini'))
    Expect ($got -ceq "[Log]`nLogLevel=2`n`n[Plugins]`nLoadReshade=true`n") "section appended, got $got"
}

Add-Case 'keep_reshade_refused_when_slot_taken' {
    param($tmp)
    $src = New-Src $tmp 'v1'
    foreach ($taken in @('ReShade64.dll', 'd3d12-ReShade64.dll')) {
        $game = New-Game $tmp ([ordered]@{ 'dxgi.dll' = $ReShade; $taken = 'other reshade' }) "game $taken"
        $before = Get-Snapshot $game
        $r = Invoke-Installer $game @('-KeepReShade') -Src $src
        Expect ($r.Code -eq $RefusedForeign -and $r.Err.Contains($taken)) "${taken}: refused, named"
        Expect-Unchanged $before $game "${taken}: a refused install"
    }
}

Add-Case 'second_proxy_refused' {
    param($tmp)
    $src = New-Src $tmp 'v1'
    $game = New-Game $tmp @{ 'WINMM.DLL' = (New-Opti 'elsewhere') }
    $before = Get-Snapshot $game
    $r = Invoke-Installer $game @() -Src $src
    Expect ($r.Code -eq $RefusedSecondProxy) "exit $RefusedSecondProxy, got $($r.Code)"
    Expect ($r.Err.Contains('WINMM.DLL') -and $r.Err.Contains('-AllowSecondProxy')) 'the refusal names the file and the flag'
    Expect-Unchanged $before $game 'a refused install'
    $r = Install-Ok $game @('-AllowSecondProxy') -Src $src
    Expect ($r.Out.Contains('also holds an OptiScaler build')) 'a warning when allowed'
    # The game's own winmm.dll (not OptiScaler) is no reason to refuse.
    $game = New-Game $tmp @{ 'winmm.dll' = [byte[]]($PE + $Ascii.GetBytes('system winmm')) } 'own [winmm]'
    Install-Ok $game -Src $src | Out-Null
}

Add-Case 'case_variant_refused_on_a_case_sensitive_file_system' {
    param($tmp)
    $game = New-Game $tmp @{ 'DXGI.DLL' = (New-Opti 'old') }
    $before = Get-Snapshot $game
    $r = Invoke-Installer $game @() -Src (New-Src $tmp 'v1')
    Expect ($r.Code -eq $RefusedForeign -and $r.Err.Contains('DXGI.DLL')) 'a case variant refused, named'
    Expect-Unchanged $before $game 'a refused install'
}

# Linux semantics standing in for Windows: .NET reads a Linux process's modules from /proc/<pid>/maps, so a
# copy of sleep started from the game folder is a process with a module (its executable) loaded from there.
Add-Case 'running_game_refused_by_its_modules' {
    param($tmp)
    $src = New-Src $tmp 'v1'
    $game = New-Game $tmp
    $exe = Join-Path $game 'game.exe'
    [IO.File]::Copy((Get-Command sleep).Source, $exe)
    & chmod 755 $exe
    Install-Ok $game -Src $src | Out-Null
    $before = Get-Snapshot $game
    $src2 = New-Src $tmp 'v2'
    $proc = Start-Process -FilePath $exe -ArgumentList '300' -PassThru
    try {
        Start-Sleep -Milliseconds 300
        foreach ($run in @(@{ Args = @(); What = 'install' }, @{ Args = @('-Uninstall'); What = 'uninstall' }, @{ Args = @('-DryRun'); What = 'dry run' })) {
            $s = if ($run.What -eq 'uninstall') { '' } else { $src2 }
            $r = Invoke-Installer $game $run.Args -Src $s
            Expect ($r.Code -eq $RefusedRunning) "$($run.What) exits $RefusedRunning, got $($r.Code)"
            Expect ($r.Err.Contains("pid $($proc.Id)") -and $r.Err.Contains('game.exe loaded')) "$($run.What): the refusal names the pid and the module"
            Expect-Unchanged $before $game "$($run.What) while the game ran"
        }
    } finally {
        $proc.Kill()
        $proc.WaitForExit()
    }
    Install-Ok $game -Src (New-Src $tmp 'v3') | Out-Null
    Expect (Test-BytesEqual (Read-Bytes (Join-Path $game 'dxgi.dll')) (New-Opti 'v3')) 'the install after the game exited went through'
}

# The second check, as far as Linux can show it: .NET emulates FileShare.None with flock, so a handle opened
# here with no sharing makes the installer's exclusive open fail exactly as a loaded DLL makes it fail on
# Windows (a sharing violation, an IOException). What Windows itself does with a mapped image is not tested.
Add-Case 'file_in_use_refused_by_the_exclusive_open' {
    param($tmp)
    $src = New-Src $tmp 'v1'
    $game = New-Game $tmp
    Install-Ok $game -Src $src | Out-Null
    $before = Get-Snapshot $game
    $src2 = New-Src $tmp 'v2'
    $held = [IO.File]::Open((Join-Path $game 'dxgi.dll'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    try {
        foreach ($args2 in @(@(), @('-Uninstall'), @('-Uninstall', '-DryRun'), @('-DryRun'))) {
            $s = if ($args2 -contains '-Uninstall') { '' } else { $src2 }
            $r = Invoke-Installer $game $args2 -Src $s
            Expect ($r.Code -eq $RefusedRunning) "$($args2 -join ' '): exit $RefusedRunning, got $($r.Code)"
            Expect ($r.Err.Contains('dxgi.dll is open in another process') -and $r.Err.Contains('Nothing was written')) "$($args2 -join ' '): the refusal names the file"
        }
    } finally {
        $held.Dispose()
    }
    # Compared once the handle is gone: under the emulation this process cannot read what it holds either.
    Expect-Unchanged $before $game 'four refused runs while dxgi.dll was held'
    Uninstall-Ok $game | Out-Null
}

Add-Case 'dry_run_writes_nothing' {
    param($tmp)
    $src = New-Src $tmp 'v1'
    $fresh = New-Game $tmp
    $before = Get-Snapshot $fresh
    $r = Invoke-Installer $fresh @('-DryRun') -Src $src
    Expect ($r.Code -eq 0 -and $r.Out.Contains('[plan] install OptiScaler.dll as dxgi.dll') -and $r.Out.Contains('[plan] install OptiScaler/libxess.dll')) 'a dry run prints the plan'
    Expect ($r.Out.Contains('Dry run: nothing was written')) 'a dry run says so'
    Expect-Unchanged $before $fresh 'a dry run into an empty folder'

    $game = New-Game $tmp @{ 'dxgi.dll' = $ReShade } 'reshade [x]'
    $before = Get-Snapshot $game
    $r = Invoke-Installer $game @('-DryRun', '-KeepReShade') -Src $src
    Expect ($r.Code -eq 0 -and $r.Out.Contains('rename dxgi.dll (ReShade) to ReShade64.dll') -and $r.Out.Contains('LoadReshade=true')) 'the Keep-ReShade dry run shows the rename and the ini edit'
    Expect-Unchanged $before $game 'a Keep-ReShade dry run'

    Install-Ok $game @('-KeepReShade') -Src $src | Out-Null
    $before = Get-Snapshot $game
    $r = Invoke-Installer $game @('-Uninstall', '-DryRun')
    Expect ($r.Code -eq 0 -and $r.Out.Contains('[plan] remove dxgi.dll') -and $r.Out.Contains('[plan] rename ReShade64.dll back to dxgi.dll')) 'an uninstall dry run prints its plan'
    Expect-Unchanged $before $game 'an uninstall dry run'
}

Add-Case 'partial_receipt_on_failure' {
    param($tmp)
    $src = New-Src $tmp 'v1'
    $game = New-Game $tmp
    $r = Invoke-Installer $game @() -Src $src -Extra @{ RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP = '2' }
    Expect ($r.Code -eq 1) "the injected failure exits 1, got $($r.Code)"
    Expect ($r.Err.Contains('status partial')) 'a partial-receipt message'
    $rc = Read-ReceiptFile $game
    Expect ($null -ne $rc -and $rc.Status -eq 'partial') "receipt status partial, got $($rc.Status)"
    Expect ((@($rc.Files.Keys) -join ',') -eq 'dxgi.dll,OptiScaler.dll') "the partial receipt lists exactly the two files written, got $(@($rc.Files.Keys) -join ',')"
    Expect (-not (Test-Path -LiteralPath (Join-Path $game 'nvngx.dll_dlssnr.dll'))) 'no step ran after the failure'
    Expect-CleanStateDir $game
    Uninstall-Ok $game | Out-Null
    Expect ((Get-Snapshot $game).Count -eq 0) "uninstall from a partial receipt empties the folder, left $(@((Get-Snapshot $game).Keys) -join ', ')"
    # And an install after a partial one finishes the job.
    Invoke-Installer $game @() -Src $src -Extra @{ RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP = '1' } | Out-Null
    $r = Install-Ok $game -Src $src
    Expect ($r.Out.Contains('stopped part-way; its records are kept')) 'the partial state is reported'
    Expect ((Read-ReceiptFile $game).Status -eq 'complete') 'the install after a partial one completes'
}

Add-Case 'uninstall_leaves_changed_files' {
    param($tmp)
    $game = New-Game $tmp
    Install-Ok $game -Src (New-Src $tmp 'v1') | Out-Null
    $ini = Join-Path $game 'OptiScaler.ini'
    [IO.File]::WriteAllText($ini, [IO.File]::ReadAllText($ini).Replace('LogToFile=auto', 'LogToFile=true'))
    $edited = Get-Sha $ini
    $r = Uninstall-Ok $game
    Expect ((Get-Sha $ini) -eq $edited) 'a user-edited ini neither removed nor changed'
    Expect ($r.Out.Contains('OptiScaler.ini: left in place')) 'a note about the file left in place'
    foreach ($rel in @('dxgi.dll', 'OptiScaler.dll', 'nvngx.dll_dlssnr.dll', 'OptiScaler')) {
        Expect (-not (Test-Path -LiteralPath (Join-Path $game $rel))) "$rel removed"
    }
    $rc = Read-ReceiptFile $game
    Expect ($null -ne $rc -and $rc.Status -eq 'uninstall-incomplete' -and (@($rc.Files.Keys) -join ',') -eq 'OptiScaler.ini') 'the receipt keeps only the ini, uninstall-incomplete'
}

Add-Case 'uninstall_needs_a_receipt_and_refuses_unsafe_paths' {
    param($tmp)
    $game = New-Game $tmp @{ 'dxgi.dll' = (New-Opti 'old') }
    $before = Get-Snapshot $game
    $r = Invoke-Installer $game @('-Uninstall')
    Expect ($r.Code -eq 1) "no receipt exits 1, got $($r.Code)"
    Expect-Unchanged $before $game 'an uninstall without a receipt'
    $victim = Join-Path $tmp 'victim.txt'
    [IO.File]::WriteAllText($victim, 'x')
    $sha = Get-Sha $victim
    [void][IO.Directory]::CreateDirectory((Join-Path $game '.retoque'))
    foreach ($bad in @('../victim.txt', '..\victim.txt', '/etc/passwd', 'C:/victim.txt', 'a/.. /victim.txt', 'a/./b', 'x:stream')) {
        [IO.File]::WriteAllText((Join-Path $game '.retoque/receipt.txt'), "version`t1`nstatus`tcomplete`nfile`t$sha`t$bad`n")
        $r = Invoke-Installer $game @('-Uninstall')
        Expect ($r.Code -eq 1 -and $r.Err.Contains('unsafe path')) "receipt path '$bad' refused, got exit $($r.Code)"
        Expect (Test-Path -LiteralPath $victim) "receipt path '$bad' not followed"
    }
    [IO.File]::WriteAllText((Join-Path $game '.retoque/receipt.txt'), "version`t2`n")
    Expect ((Invoke-Installer $game @('-Uninstall')).Code -eq 1) 'a version 2 receipt refused'
    [IO.File]::WriteAllText((Join-Path $game '.retoque/receipt.txt'), "version`t1`nwhatever`tx`n")
    Expect ((Invoke-Installer $game @('-Uninstall')).Code -eq 1) 'an unknown record refused'
}

Add-Case 'libxell_follows_the_game' {
    param($tmp)
    $src = New-Src $tmp 'v1'
    # The game ships its own libxell and an older one of ours is in OptiScaler\: ours is not installed and the
    # old one is renamed, never deleted.
    $game = New-Game $tmp ([ordered]@{ 'libxell.dll' = 'the game libxell'; 'OptiScaler/libxell.dll' = 'our old libxell' })
    $before = Get-Snapshot $game
    $r = Install-Ok $game -Src $src
    Expect ($r.Out.Contains('the game ships its own, so OptiScaler/libxell.dll is not installed')) 'ours not installed'
    Expect (-not (Test-Path -LiteralPath (Join-Path $game 'OptiScaler/libxell.dll')) -and (Test-Path -LiteralPath (Join-Path $game 'OptiScaler/libxell.dll.disabled'))) 'the old one renamed to .disabled'
    Expect (@((Read-ReceiptFile $game).Renames | Where-Object { $_[1] -eq 'OptiScaler/libxell.dll' -and $_[2] -eq 'OptiScaler/libxell.dll.disabled' }).Count -eq 1) 'the rename is recorded'
    Uninstall-Ok $game | Out-Null
    Expect-Unchanged $before $game 'uninstall undoes the libxell rename and removes the support files'
    # The game ships the very same libxell: ours goes in, nothing is renamed.
    $game = New-Game $tmp @{ 'libxell.dll' = 'xell v1' } 'same [xell]'
    $r = Install-Ok $game -Src $src
    Expect ((Test-Path -LiteralPath (Join-Path $game 'OptiScaler/libxell.dll')) -and $r.Out.Contains("the game's copy is ours")) 'an identical libxell is left beside ours'
}

Add-Case 'model_is_never_touched' {
    param($tmp)
    $game = New-Game $tmp @{ 'nvngx_dlssnr.dll' = 'the NVIDIA model' }
    $model = Join-Path $game 'nvngx_dlssnr.dll'
    $sha = Get-Sha $model
    $r = Install-Ok $game -Src (New-Src $tmp 'v1')
    Expect ($r.Out.Contains('nvngx_dlssnr.dll: present, left as it is') -and -not $r.Out.Contains('is not in this folder')) 'a present model reported, no warning'
    Uninstall-Ok $game | Out-Null
    Expect ((Get-Sha $model) -eq $sha) 'install and uninstall leave the model alone'
    # Even a receipt that lists it does not get it removed.
    [void][IO.Directory]::CreateDirectory((Join-Path $game '.retoque'))
    [IO.File]::WriteAllText((Join-Path $game '.retoque/receipt.txt'), "version`t1`nstatus`tcomplete`nfile`t$sha`tnvngx_dlssnr.dll`n")
    $r = Uninstall-Ok $game
    Expect ((Test-Path -LiteralPath $model) -and $r.Out.Contains('never removed by this script')) 'a receipt cannot get the model removed'
    $src2 = New-Src $tmp 'v2'
    [IO.File]::WriteAllText((Join-Path $src2 'OptiScaler/nvngx_dlssnr.dll'), 'a model someone put in the release')
    $game2 = New-Game $tmp @{} 'second [x]'
    $r = Install-Ok $game2 -Src $src2
    Expect (-not (Test-Path -LiteralPath (Join-Path $game2 'OptiScaler/nvngx_dlssnr.dll')) -and $r.Out.Contains('is never copied')) 'a model in the release is not copied'
}

Add-Case 'installer_run_from_the_extracted_release_in_the_game_folder' {
    param($tmp)
    # The zip says "extract all files to the game folder": then the script's own folder is the game folder.
    $game = New-Game $tmp @{ 'game.exe' = $PE }
    $src = New-Src $tmp 'v1'
    foreach ($file in [IO.Directory]::GetFiles($src, '*', [IO.SearchOption]::AllDirectories)) {
        $to = Join-Path $game $file.Substring($src.Length + 1)
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($to))
        [IO.File]::Copy($file, $to)
    }
    $script = Join-Path $game 'install-windows.ps1'
    [IO.File]::Copy($Installer, $script)
    $r = Invoke-Installer $game @() -Script $script
    Expect ($r.Code -eq 0) "install exits 0, got $($r.Code)"
    Expect (Test-BytesEqual (Read-Bytes (Join-Path $game 'dxgi.dll')) (New-Opti 'v1')) 'dxgi.dll is OptiScaler.dll'
    Expect ($r.Out.Contains('OptiScaler.dll: already this build') -and $r.Out.Contains('Keeping the OptiScaler.ini')) 'the extracted files kept as they are'
    $rc = Read-ReceiptFile $game
    Expect (-not $rc.Files.ContainsKey('OptiScaler.ini')) 'the extracted ini is not claimed'
    Uninstall-Ok $game | Out-Null
    $left = @((Get-Snapshot $game).Keys)
    Expect (($left -join ',') -ceq 'OptiScaler.ini,game.exe,install-windows.ps1') "uninstall leaves the game, the script and the ini, got $($left -join ',')"
}

# ---- Receipts across the two scripts ---------------------------------------------------------------------

# One game folder with everything the receipt records: a rename (ReShade), backups (a foreign forwarder, the
# user's ini), an ini edit, and the libxell rename.
function New-MixedGame([string]$Tmp, [string]$Name) {
    New-Game $Tmp ([ordered]@{ 'dxgi.dll' = $ReShade; 'nvngx.dll_dlssnr.dll' = $Foreign; 'OptiScaler.ini' = $UserIni
            'libxell.dll' = 'the game libxell'; 'OptiScaler/libxell.dll' = 'our old libxell'; 'game.exe' = $PE
        }) $Name
}

Add-Case 'linux_receipt_uninstalled_by_windows' {
    param($tmp)
    $src = New-Src $tmp 'v1' -NoSupport
    $game = New-MixedGame $tmp 'game [linux]'
    $before = Get-Snapshot $game
    $r = Invoke-Linux $game @('--keep-reshade', '--replace-foreign') -Src $src
    Expect ($r.Code -eq 0) "deploy-to-game.sh exits 0, got $($r.Code)"
    $rc = Read-ReceiptFile $game
    Expect ($rc.Renames.Count -eq 2 -and $rc.Backups.Count -eq 2 -and $rc.Files.Count -eq 4) "the Linux receipt records 2 renames, 2 backups, 4 files; got $($rc.Renames.Count), $($rc.Backups.Count), $($rc.Files.Count)"
    $r = Invoke-Installer $game @('-Uninstall', '-DryRun')
    Expect ($r.Code -eq 0 -and $r.Out.Contains('[plan] rename ReShade64.dll back to dxgi.dll') -and $r.Out.Contains('[plan] restore nvngx.dll_dlssnr.dll')) 'the Windows dry run reads the Linux receipt'
    Uninstall-Ok $game | Out-Null
    Expect-Unchanged $before $game 'a Linux deploy uninstalled by the Windows script'
    # And a partial Linux receipt.
    $game = New-Game $tmp @{} 'partial [linux]'
    $r = Invoke-Linux $game @() -Src $src -Extra @{ RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP = '2' }
    Expect ($r.Code -ne 0 -and (Read-ReceiptFile $game).Status -eq 'partial') 'a partial Linux receipt'
    Uninstall-Ok $game | Out-Null
    Expect ((Get-Snapshot $game).Count -eq 0) 'the Windows uninstall empties it'
}

Add-Case 'windows_receipt_uninstalled_by_linux' {
    param($tmp)
    $src = New-Src $tmp 'v1' -NoSupport
    $game = New-MixedGame $tmp 'game [windows]'
    $before = Get-Snapshot $game
    Install-Ok $game @('-KeepReShade', '-ReplaceForeign') -Src $src | Out-Null
    $r = Invoke-Linux $game @('--uninstall')
    Expect ($r.Code -eq 0) "deploy-to-game.sh --uninstall exits 0, got $($r.Code)"
    Expect-Unchanged $before $game 'a Windows install uninstalled by the Linux script'
    # With the support folder: the Linux script removes the files but, creating no folders itself, leaves the
    # ones the Windows install made, empty. That is the only difference.
    $game = New-Game $tmp @{ 'game.exe' = $PE } 'support [windows]'
    $before = Get-Snapshot $game
    Install-Ok $game -Src (New-Src $tmp 'v2') | Out-Null
    $r = Invoke-Linux $game @('--uninstall')
    Expect ($r.Code -eq 0) "deploy-to-game.sh --uninstall exits 0, got $($r.Code)"
    $diff = Get-SnapshotDiff $before (Get-Snapshot $game)
    Expect (($diff -join ',') -ceq '+OptiScaler,+OptiScaler/D3D12_OptiScaler') "only the empty support folders left, got $($diff -join ',')"
    # And a partial Windows receipt.
    $game = New-Game $tmp @{} 'partial [windows]'
    $r = Invoke-Installer $game @() -Src $src -Extra @{ RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP = '2' }
    Expect ($r.Code -eq 1 -and (Read-ReceiptFile $game).Status -eq 'partial') 'a partial Windows receipt'
    $r = Invoke-Linux $game @('--uninstall')
    Expect ($r.Code -eq 0 -and (Get-Snapshot $game).Count -eq 0) 'the Linux uninstall empties it'
}

Add-Case 'both_scripts_write_the_same_receipt_and_files' {
    param($tmp)
    $src = New-Src $tmp 'v1' -NoSupport
    $linux = New-MixedGame $tmp 'same [linux]'
    $windows = New-MixedGame $tmp 'same [windows]'
    Expect ((Invoke-Linux $linux @('--keep-reshade', '--replace-foreign') -Src $src).Code -eq 0) 'the Linux deploy'
    Install-Ok $windows @('-KeepReShade', '-ReplaceForeign') -Src $src | Out-Null
    $norm = {
        param($game)
        @((Read-ReceiptFile $game).Text.Split("`n") | Where-Object { $_ -ne '' -and -not $_.StartsWith('#') -and -not $_.StartsWith("updated`t") } |
            ForEach-Object { $_ -replace '\.retoque/backup/[0-9TZ-]+/', '.retoque/backup/STAMP/' })
    }
    $a = & $norm $linux
    $b = & $norm $windows
    Expect (($a -join "`n") -ceq ($b -join "`n")) "identical receipts.`n  linux:   $($a -join ' | ')`n  windows: $($b -join ' | ')"
    $snap = {
        param($game)
        $s = Get-Snapshot $game
        @($s.Keys | Where-Object { $_ -notmatch 'receipt\.txt$' } | ForEach-Object { ($_ -replace 'backup.[0-9TZ-]+', 'backup/STAMP') + ' ' + $s[$_] })
    }
    $a = & $snap $linux
    $b = & $snap $windows
    Expect (($a -join "`n") -ceq ($b -join "`n")) "identical folders (the edited ini byte for byte).`n  linux:   $($a -join ' | ')`n  windows: $($b -join ' | ')"
}

Add-Case 'each_script_reinstalls_over_the_others_receipt' {
    param($tmp)
    $game = New-Game $tmp @{ 'game.exe' = $PE }
    Expect ((Invoke-Linux $game @() -Src (New-Src $tmp 'v1' -NoSupport)).Code -eq 0) 'the Linux deploy'
    $r = Install-Ok $game -Src (New-Src $tmp 'v2' -NoSupport)
    Expect ($r.Out.Contains('replace dxgi.dll (installed here earlier') -and (Read-ReceiptFile $game).Backups.Count -eq 0) 'the Windows install takes the Linux files as ours'
    $r = Invoke-Linux $game @() -Src (New-Src $tmp 'v3' -NoSupport)
    Expect ($r.Code -eq 0 -and $r.Out.Contains('replace dxgi.dll (deployed here earlier')) 'the Linux deploy takes the Windows files as its own'
    Expect ((Read-ReceiptFile $game).Backups.Count -eq 0) 'nothing backed up either way'
    Uninstall-Ok $game | Out-Null
    Expect ((@((Get-Snapshot $game).Keys) -join ',') -ceq 'game.exe') 'one uninstall takes it all out'
}

# --------------------------------------------------------------------------------------------------------------

$failed = @()
foreach ($c in $Cases) {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('winst-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    [void][IO.Directory]::CreateDirectory($tmp)
    $script:Last = $null
    try {
        & $c.Body $tmp
        Write-Host "PASS: $($c.Name)"
    } catch {
        $failed += $c.Name
        Write-Host "FAIL: $($c.Name): $($_.Exception.Message)"
        Write-Host "  at $($_.InvocationInfo.ScriptLineNumber): $($_.InvocationInfo.Line.Trim())"
        if ($null -ne $script:Last) {
            Write-Host "  last command: $($script:Last.Cmd) (exit $($script:Last.Code))"
            foreach ($line in @(($script:Last.Out + $script:Last.Err).Split("`n") | Select-Object -Last 25)) { Write-Host "    $line" }
        }
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
Write-Host "$($Cases.Count - $failed.Count) passed, $($failed.Count) failed"
exit $(if ($failed.Count -gt 0) { 1 } else { 0 })
