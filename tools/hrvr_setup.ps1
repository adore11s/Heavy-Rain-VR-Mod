# ===========================================================================
#  HEAVY RAIN - VR MOD : installer / uninstaller
#
#  Called by the two .cmd files in the HR_VR_MOD folder:
#    "Install VR mod.cmd"    ->  -Mode Install    (once: dinput8.dll next to HeavyRain.exe, display mode Windowed)
#    "Uninstall VR mod.cmd"  ->  -Mode Uninstall  (the mod's own files to the Recycle Bin, display mode put back)
#
#  The mod is a single file, dinput8.dll, next to HeavyRain.exe. Windows loads it into the game whichever way the game
#  is started (Steam, Epic, GOG, the exe) - nothing is injected and no game file is replaced or patched on disk. The mod
#  reads hrvr.ini and writes its hrvr_logs folder next to HeavyRain.exe.
#
#  The one game setting changed (owner 2026-10-29, like the God of War VR mod's installer): the game's graphics
#  setting Display mode -> Windowed, user_setting.ini [GRAPHIC_SECTION] ScreenMode=2. Decompiled 2026-10-29: the
#  game's own mode getter 0x140a6f8c0 returns 0 for a full-screen swap chain, else 2 when the window has a title bar
#  (WS_CAPTION) and 1 when it has none; its setter 0x140a6b420 makes 0 exclusive full screen, 1 the borderless
#  screen-sized window and 2 the window with a title bar sized to the picture. The VR mod needs a window (the headset
#  picture size and the small desktop window both resize it). The original value is recorded (settings_changed.txt)
#  and the uninstaller puts it back while it is still the one this set.
#
#  The uninstaller NEVER deletes anything for good (only its own settings_changed.txt record): everything it removes
#  goes to the Recycle Bin, and only from a fixed list of the mod's own files - never an .exe, never a folder holding one, never the game folder itself or
#  anything outside it.
#
#  As few key presses as possible (playbook 28.6): choices take ONE key (no Enter); only the uninstaller's YES is typed;
#  a clean run closes by itself after 10 s (any key closes it at once); the window waits for a key only when there is
#  something to read (a [!] warning, an [X] error, a setting to change by hand). Everything that can stop the run is
#  checked before any question. hrvr.ini is not shipped: the mod writes its built-in defaults when there is none.
#
#  Kept ASCII only: Windows PowerShell 5.1 reads a script without a BOM in the ANSI code page.
# ===========================================================================
param([ValidateSet('Install', 'Uninstall')][string]$Mode = 'Install')

$ErrorActionPreference = 'Stop'
$ModDir = Split-Path -Parent $PSScriptRoot
$Files = Join-Path $ModDir 'files'
$StateGame = Join-Path $ModDir 'game_dir.txt'           # where the mod was installed (for the uninstaller)
$StateSettings = Join-Path $ModDir 'settings_changed.txt' # user_setting.ini values this changed: KEY=original|set
$Section = 'GRAPHIC_SECTION'
$Want = [ordered]@{ 'ScreenMode' = '2' }                  # 2 = Windowed (see above)
$Manual = @{ 'ScreenMode' = 'Display mode: Windowed' }

$script:NeedRead = $false  # something the player should read before the window closes
function Say([string]$t) { Write-Host "   $t" }
function Warn([string]$t) { Write-Host "   [!] $t" -ForegroundColor Yellow; $script:NeedRead = $true }
function Bad([string]$t) { Write-Host "   [X] $t" -ForegroundColor Red; $script:NeedRead = $true }

# 28.6: a clean run closes by itself (10 s, any key closes at once); with something to read it waits for any key.
function Done([int]$code) {
    Write-Host ''
    $console = $true
    try { [void][Console]::KeyAvailable } catch { $console = $false }
    if (-not $console) {  # no console (input redirected): nothing to wait for
        exit $code
    }
    if ($script:NeedRead -or $code -ne 0) {
        Write-Host '   Press any key to close'
        [void][Console]::ReadKey($true)
        exit $code
    }
    for ($i = 10; $i -gt 0; $i--) {
        Write-Host -NoNewline ("`r   Closing in {0,2} s - press any key to close now " -f $i)
        for ($t = 0; $t -lt 10; $t++) {
            if ([Console]::KeyAvailable) { [void][Console]::ReadKey($true); Write-Host ''; exit $code }
            Start-Sleep -Milliseconds 100
        }
    }
    Write-Host ''
    exit $code
}

# 28.6: a choice takes one key - only the keys offered count, the key taken is shown; without a console a typed answer.
function Read-Key([string]$question, [string[]]$keys) {
    Write-Host -NoNewline "   $question "
    try {
        while ($true) {
            $k = [Console]::ReadKey($true)
            $c = ([string]$k.KeyChar).ToUpperInvariant()
            if ($keys -contains $c) { Write-Host $c; return $c }
        }
    } catch {
        while ($true) {
            $c = ([string](Read-Host)).Trim().ToUpperInvariant()
            if ($keys -contains $c) { return $c }
        }
    }
}

function Banner([string]$title) {
    Write-Host ''
    Write-Host '  ================================================================'
    Write-Host "    $title"
    Write-Host '  ================================================================'
    Write-Host ''
}

# ---- the game folder ---------------------------------------------------------------------------------------------
function Test-GameDir([string]$d) { return $d -and (Test-Path -LiteralPath (Join-Path $d 'HeavyRain.exe')) }

function Find-GameDir {
    # 0. the mod's files were copied straight into the game folder: that folder (never another install)
    if (Test-GameDir $ModDir) { return (Resolve-Path -LiteralPath $ModDir).Path }
    # 1. this HR_VR_MOD folder sits inside the game folder (the normal way, see README)
    $parent = Split-Path -Parent $ModDir
    if (Test-GameDir $parent) { return (Resolve-Path -LiteralPath $parent).Path }
    # 2. where the last install went
    if (Test-Path -LiteralPath $StateGame) {
        $d = (Get-Content -LiteralPath $StateGame -Raw).Trim()
        if (Test-GameDir $d) { return (Resolve-Path -LiteralPath $d).Path }
    }
    # 3. Steam libraries
    $cands = @()
    try {
        $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath
        if ($steam) {
            $libs = @($steam)
            $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
            if (Test-Path -LiteralPath $vdf) {
                foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                    $libs += ($m.Groups[1].Value -replace '\\\\', '\')
                }
            }
            foreach ($l in ($libs | Select-Object -Unique)) { $cands += (Join-Path $l 'steamapps\common\Heavy Rain') }
        }
    } catch { }
    # 4. Epic Games Launcher installs
    $man = Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests'
    if (Test-Path -LiteralPath $man) {
        foreach ($f in Get-ChildItem -LiteralPath $man -Filter '*.item' -ErrorAction SilentlyContinue) {
            try { $j = Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json } catch { continue }
            if ($j.InstallLocation) { $cands += $j.InstallLocation }
        }
    }
    # 5. GOG (GOG Galaxy and GOG's offline installers record every game under GOG.com\Games: gameName, path)
    foreach ($root in @('HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games', 'HKLM:\SOFTWARE\GOG.com\Games')) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        foreach ($k in Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue) {
            try { $p = Get-ItemProperty -LiteralPath $k.PSPath -ErrorAction Stop } catch { continue }
            if ($p.gameName -like '*Heavy Rain*' -and $p.path) { $cands += $p.path }
        }
    }
    $cands += 'C:\GOG Games\Heavy Rain'
    if (${env:ProgramFiles(x86)}) { $cands += (Join-Path ${env:ProgramFiles(x86)} 'GOG Galaxy\Games\Heavy Rain') }
    foreach ($c in $cands) { if (Test-GameDir $c) { return (Resolve-Path -LiteralPath $c).Path } }
    # 6. ask
    Warn 'Could not find Heavy Rain by itself.'
    Say 'Easiest fix: put this whole HR_VR_MOD folder INTO your Heavy Rain folder'
    Say '(the one that contains HeavyRain.exe) and run this again.'
    Write-Host ''
    $d = (Read-Host '   Or paste the Heavy Rain folder here and press Enter').Trim().Trim('"')
    if (Test-GameDir $d) { return (Resolve-Path -LiteralPath $d).Path }
    return $null
}

function Test-GameRunning { return [bool](Get-Process -Name 'HeavyRain' -ErrorAction SilentlyContinue) }

# ---- dinput8.dll -------------------------------------------------------------------------------------------------
# Ours = an HRVR build: its first log line "hrvr <version> linked ..." (0.28.0 on; 28.1) or, older builds,
# "HRVR <version> built ..." is in the file. Anything else named dinput8.dll belongs to another mod / tool and is
# never overwritten or removed.
function Test-OurDll([string]$path) {
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path))
    return ($text.Contains('hrvr ') -and $text.Contains(' linked ')) -or ($text.Contains('HRVR ') -and $text.Contains('built'))
}

# ---- the Recycle Bin, with every check ---------------------------------------------------------------------------
Add-Type -AssemblyName Microsoft.VisualBasic

# Sends `path` to the Recycle Bin - never deletes for good. Refuses (and says so): anything outside the game folder,
# the game folder itself, an .exe, a folder that holds an .exe anywhere inside it.
function Send-ToRecycleBin([string]$path, [string]$game) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    $full = [IO.Path]::GetFullPath($path).TrimEnd('\')
    $g = [IO.Path]::GetFullPath($game).TrimEnd('\')
    if ($full -ieq $g -or -not $full.StartsWith($g + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Warn "Not removed (not a file inside the game folder): $full"
        return $false
    }
    if (Test-Path -LiteralPath $full -PathType Leaf) {
        if ([IO.Path]::GetExtension($full) -ieq '.exe') { Warn "Not removed (a program file): $full"; return $false }
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile($full, 'OnlyErrorDialogs', 'SendToRecycleBin')
    } else {
        $exe = Get-ChildItem -LiteralPath $full -Recurse -Force -File -Filter '*.exe' -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($exe) { Warn "Not removed (it holds a program file, $($exe.Name)): $full"; return $false }
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($full, 'OnlyErrorDialogs', 'SendToRecycleBin')
    }
    return $true
}

# ---- user_setting.ini --------------------------------------------------------------------------------------------
function Read-Recorded {
    $rec = [ordered]@{}
    if (Test-Path -LiteralPath $StateSettings) {
        foreach ($l in Get-Content -LiteralPath $StateSettings) {
            $i = $l.IndexOf('=')
            if ($i -gt 0) { $rec[$l.Substring(0, $i)] = $l.Substring($i + 1) }
        }
    }
    return $rec
}

function Read-Ini([string]$path) {
    $raw = [IO.File]::ReadAllText($path)
    $nl = "`n"; if ($raw.Contains("`r`n")) { $nl = "`r`n" }
    return @{ Lines = [System.Collections.ArrayList]@($raw -split "`r?`n"); NewLine = $nl }
}

function Write-Ini([string]$path, $ini) {
    $text = ($ini.Lines -join $ini.NewLine)
    [IO.File]::WriteAllText($path, $text, (New-Object Text.ASCIIEncoding))  # plain ASCII, like the game's own file
}

# The line index of KEY inside [GRAPHIC_SECTION] (-1: not there); $sectionLine gets the section header's index.
function Find-Key($lines, [string]$key, [ref]$sectionLine) {
    $in = $false
    $sectionLine.Value = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $l = $lines[$i].Trim()
        if ($l -match '^\[(.+)\]$') {
            $in = $Matches[1] -ieq $Section
            if ($in) { $sectionLine.Value = $i }
            continue
        }
        if ($in -and $l -match ('^' + [regex]::Escape($key) + '\s*=')) { return $i }
    }
    return -1
}

function Get-Value($lines, [string]$key) {
    $s = -1
    $i = Find-Key $lines $key ([ref]$s)
    if ($i -lt 0) { return $null }
    return ($lines[$i] -replace '^[^=]*=', '').Trim()
}

# Sets KEY=value in [GRAPHIC_SECTION] (adding the key, or the section, when missing); returns the old value or ''.
function Set-Value($lines, [string]$key, [string]$value) {
    $s = -1
    $i = Find-Key $lines $key ([ref]$s)
    if ($i -ge 0) {
        $old = ($lines[$i] -replace '^[^=]*=', '').Trim()
        $lines[$i] = "$key=$value"
        return $old
    }
    if ($s -lt 0) {
        [void]$lines.Add('')
        [void]$lines.Add("[$Section]")
        [void]$lines.Add("$key=$value")
    } else {
        $lines.Insert($s + 1, "$key=$value")
    }
    return ''
}

function Set-VrGameSettings([string]$game) {
    $ini = Join-Path $game 'user_setting.ini'
    $failed = @()
    if (-not (Test-Path -LiteralPath $ini)) {
        # 0.36.15: no settings file yet (the game writes it on its first run). The mod itself starts the game in a
        # window (dinput8.dll: user_setting.ini ScreenMode=2 before the game reads it, and Windowed when the game
        # loads its defaults; hrvr.ini force_windowed), so nothing is left for the player to do.
        Say 'Game settings: Windowed is set by the mod itself at the first start (no user_setting.ini yet).'
        return
    } else {
        try {
            if (-not (Test-Path -LiteralPath ($ini + '.vrmod-backup'))) { Copy-Item -LiteralPath $ini -Destination ($ini + '.vrmod-backup') }
            $data = Read-Ini $ini
            $rec = Read-Recorded
            foreach ($k in $Want.Keys) {
                $old = Set-Value $data.Lines $k $Want[$k]
                # the first original is kept (a second install must not record the mod's own value as yours)
                $orig = $old
                if ($rec.Contains($k)) { $orig = ($rec[$k] -split '\|')[0] }
                $rec[$k] = "$orig|$($Want[$k])"
            }
            Write-Ini $ini $data
            $out = foreach ($k in $rec.Keys) { "$k=$($rec[$k])" }
            Set-Content -LiteralPath $StateSettings -Value $out -Encoding ASCII
            # read back: the file really holds the values now
            $check = Read-Ini $ini
            foreach ($k in $Want.Keys) {
                if ((Get-Value $check.Lines $k) -ne $Want[$k]) { $failed += $k }
            }
        } catch {
            Warn "The game settings could not be changed ($($_.Exception.Message))."
            $failed = @($Want.Keys)
        }
    }
    # 28.6: what was set, in ONE line
    $set = @(foreach ($k in $Want.Keys) { if ($failed -notcontains $k) { $Manual[$k] } })
    if ($set.Count -gt 0) { Say ("Game settings set for VR: " + ($set -join ', ')) }
    if ($failed.Count -gt 0) {
        Write-Host ''
        Warn 'These game settings could NOT be changed by the installer.'
        Warn 'Please set them yourself in the game (Options -> Graphics):'
        foreach ($k in $failed) { Warn "   - $($Manual[$k])" }
    }
}

function Restore-Settings([string]$game) {
    $ini = Join-Path $game 'user_setting.ini'
    $rec = Read-Recorded
    if ($rec.Count -eq 0 -or -not (Test-Path -LiteralPath $ini)) { return }
    $data = Read-Ini $ini
    $restored = @()
    foreach ($k in $rec.Keys) {
        $parts = $rec[$k] -split '\|'
        $orig = $parts[0]
        $set = $(if ($parts.Count -gt 1) { $parts[1] } else { $null })
        $cur = Get-Value $data.Lines $k
        if ($null -ne $cur -and $null -ne $set -and $cur -eq $set -and $orig -ne '') {  # still what the installer set
            [void](Set-Value $data.Lines $k $orig)
            $restored += "$k=$orig"
        } else {
            Say "user_setting.ini: $k left at $cur (you changed it after the install)"
        }
    }
    Write-Ini $ini $data
    if ($restored.Count -gt 0) { Say ("Game settings put back: " + ($restored -join ', ')) }
    [void](Send-ToRecycleBin ($ini + '.vrmod-backup') $game)
    # the installer's own record (in this folder, which need not be inside the game folder)
    Remove-Item -LiteralPath $StateSettings -Force -ErrorAction SilentlyContinue
}

# =================================================================================================================
if ($Mode -eq 'Install') {
    Banner 'HEAVY RAIN VR MOD  -  INSTALL'
    $payload = Join-Path $Files 'dinput8.dll'
    if (-not (Test-Path -LiteralPath $payload)) { Bad 'files\dinput8.dll is missing - unzip the mod again.'; Done 1 }
    $game = Find-GameDir
    if (-not $game) { Bad 'No Heavy Rain folder found - nothing was changed.'; Done 1 }
    Say "Game folder: $game"
    if (Test-GameRunning) { Warn 'Heavy Rain is running. Close it, then run this again.'; Done 1 }
    Set-Content -LiteralPath $StateGame -Value $game -Encoding UTF8

    $dst = Join-Path $game 'dinput8.dll'
    if (Test-Path -LiteralPath $dst) {
        if (-not (Test-OurDll $dst)) {
            Bad 'There is already a dinput8.dll in the game folder that is NOT this mod'
            Say '(another mod or tool). It was left alone. Move it somewhere else if you want'
            Say 'this VR mod instead, then run this again.'
            Done 1
        }
        if ((Get-FileHash -LiteralPath $dst).Hash -eq (Get-FileHash -LiteralPath $payload).Hash) {
            Say 'dinput8.dll: already installed and up to date'
        } else {
            Copy-Item -LiteralPath $payload -Destination $dst -Force
            Say 'dinput8.dll: updated to this release'
        }
    } else {
        Copy-Item -LiteralPath $payload -Destination $dst
        Say 'dinput8.dll: installed'
    }
    try { Unblock-File -LiteralPath $dst } catch { }

    # (hrvr.ini: the mod writes its built-in defaults at the first start when there is none; yours is never touched)

    Set-VrGameSettings $game

    Write-Host ''
    Say 'Done. Start Heavy Rain as usual with the headset on - VR starts by itself.'
    Say 'To remove the mod: "Uninstall VR mod.cmd" in this folder.'
    Done 0
}

if ($Mode -eq 'Uninstall') {
    Banner 'HEAVY RAIN VR MOD  -  UNINSTALL'
    $game = Find-GameDir
    if (-not $game) { Bad 'No Heavy Rain folder found - nothing was changed.'; Done 1 }
    Say "Game folder: $game"
    # 28.6: what stops the run is checked before any question
    if (Test-GameRunning) { Warn 'Heavy Rain is running - close it first, then run this again.'; Done 1 }
    Write-Host ''
    Say 'This will move to the Recycle Bin (nothing is deleted for good):'
    Say '  - dinput8.dll (the mod) from the game folder'
    Say '  - if you want: hrvr.ini (your VR settings) and the hrvr_logs folder'
    Say '  - this HR_VR_MOD folder'
    Say 'and put the display mode back to what it was before the install.'
    Say 'Your saves, HeavyRain.exe and every other game file are left alone.'
    Write-Host ''
    $a = Read-Host '   Type YES and press Enter to uninstall (anything else cancels)'
    if ($a -ne 'YES' -and $a -ne 'yes') { Say 'Cancelled. Nothing was changed.'; Done 0 }
    Write-Host ''

    $dll = Join-Path $game 'dinput8.dll'
    if (Test-Path -LiteralPath $dll) {
        if (Test-OurDll $dll) {
            if (Send-ToRecycleBin $dll $game) { Say 'dinput8.dll (the mod) -> Recycle Bin' }
        } else { Warn 'dinput8.dll in the game folder is not this mod - left alone' }
    } else { Say 'dinput8.dll: not installed' }

    Restore-Settings $game

    $k = Read-Key 'Also move hrvr.ini (your VR settings) and the hrvr_logs folder to the Recycle Bin? Press Y or N:' @('Y', 'N')
    if ($k -eq 'Y') {
        $moved = @()
        foreach ($f in @('hrvr.ini', 'hrvr.ini.tmp', 'hrvr_logs', 'hrvr_cmd.txt', 'hrvr_cmd.ack')) {
            if (Send-ToRecycleBin (Join-Path $game $f) $game) { $moved += $f }
        }
        if ($moved.Count -gt 0) { Say ("To the Recycle Bin: " + ($moved -join ', ')) }
    } else {
        Say 'hrvr.ini and hrvr_logs kept'
    }
    # last, this release folder itself - only when it really is the mod's folder (its setup script and uninstaller
    # inside, no program), inside the game folder (Send-ToRecycleBin checks that and the .exe rule again)
    $isMod = (Test-Path -LiteralPath (Join-Path $ModDir 'tools\hrvr_setup.ps1')) -and
             (Test-Path -LiteralPath (Join-Path $ModDir 'Uninstall VR mod.cmd')) -and -not (Test-GameDir $ModDir)
    if ($isMod -and (Test-Path -LiteralPath (Join-Path $ModDir 'source'))) {
        # a working copy / source download (it holds the mod's source): never moved
        Say 'The HR_VR_MOD folder was left in place (it holds the mod source).'
    } elseif ($isMod -and (Send-ToRecycleBin $ModDir $game)) { Say 'The HR_VR_MOD folder -> Recycle Bin' }
    Write-Host ''
    Say 'Done. Heavy Rain starts normally on your monitor again.'
    Done 0
}
