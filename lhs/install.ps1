# LighthouseSync for SteamVR on Windows: installs or updates the driver and its dashboard from the latest release.
#
#   irm https://github.com/ronaldburns/LighthouseSync/releases/latest/download/install.ps1 | iex
#
# Uninstall (keeps your saved base stations and settings in %LOCALAPPDATA%\QuestLHSync):
#   & ([scriptblock]::Create((irm https://github.com/ronaldburns/LighthouseSync/releases/latest/download/install.ps1))) -Uninstall
#
# Installs where QuestLHSync's own installer does (%LOCALAPPDATA%\QuestLHSync\questlhsync), so it updates an existing
# QuestLHSync in place. The download is checked against the SHA-256 GitHub publishes for it. Files SteamVR has open are
# renamed aside (the driver deletes them at its next start), so SteamVR only has to restart, not close first, unless
# its "activateMultipleDrivers" setting still has to be turned on.
param(
  [switch]$Uninstall,
  [string]$Repo = "@REPO@",   # the release workflow writes the repository it was built from
  [string]$Version = ""        # a release tag, else the latest
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Windows PowerShell's progress bar slows downloads to a crawl
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072  # TLS 1.2
if ($Repo -like "@*") { $Repo = "ronaldburns/LighthouseSync" }

$Root = Join-Path $env:LOCALAPPDATA "QuestLHSync"
$Driver = Join-Path $Root "questlhsync"
$State = Join-Path $Root "installed.json"
$Ua = @{ "User-Agent" = "LighthouseSync-install"; "Accept" = "application/vnd.github+json" }

function Say($text, $color = "Gray") { Write-Host $text -ForegroundColor $color }
function Fail($text) { throw "LighthouseSync: $text" }

function VrPaths($key) {
  $f = Join-Path $env:LOCALAPPDATA "openvr\openvrpaths.vrpath"
  if (-not (Test-Path $f)) { return @() }
  $j = Get-Content $f -Raw | ConvertFrom-Json
  if ($j.$key) { return @($j.$key) } else { return @() }
}

function VrPathReg {
  foreach ($r in VrPaths "runtime") {
    $exe = Join-Path $r "bin\win64\vrpathreg.exe"
    if (Test-Path $exe) { return $exe }
  }
  Fail "SteamVR not found. Install it and run it once, then try again."
}

function Reg($action, $dir) {
  $out = & (VrPathReg) $action $dir 2>&1
  if ($LASTEXITCODE -ne 0) { Fail "vrpathreg $action failed ($LASTEXITCODE): $out" }
}

# .NET rather than Get-FileHash and Expand-Archive, which some PowerShell setups can't load
function Sha256($file) {
  $sha = [Security.Cryptography.SHA256]::Create()
  $fs = [IO.File]::OpenRead($file)
  try { return -join ($sha.ComputeHash($fs) | ForEach-Object { $_.ToString("X2") }) } finally { $fs.Dispose(); $sha.Dispose() }
}

function SteamVrRunning { [bool](Get-Process vrserver -ErrorAction SilentlyContinue) }

# Asks SteamVR to close like closing its window, then ends what's left after 15 s. Killed outright, Steam can go on
# thinking SteamVR runs and refuse to start it again.
function StopSteamVr {
  $names = "vrmonitor", "vrserver", "vrcompositor", "vrdashboard", "vrwebhelper", "vrstartup", "QuestLHSync"
  Get-Process vrmonitor -ErrorAction SilentlyContinue | ForEach-Object { [void]$_.CloseMainWindow() }
  for ($i = 0; $i -lt 150 -and (Get-Process $names -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 100 }
  Get-Process $names -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  for ($i = 0; $i -lt 50 -and (SteamVrRunning); $i++) { Start-Sleep -Milliseconds 100 }
  if (SteamVrRunning) { Fail "couldn't close SteamVR (vrserver.exe is still running)" }
}

# "steamvr"."activateMultipleDrivers" has to be true or SteamVR won't load the driver next to the headset's own.
# Edited as text so the rest of the file stays as SteamVR wrote it. Returns the file when it needs the change.
function SettingsFile {
  foreach ($dir in VrPaths "config") { return Join-Path $dir "steamvr.vrsettings" }
  return $null
}

function NeedsMultipleDrivers($f) {
  if (-not $f -or -not (Test-Path $f)) { return [bool]$f }
  return -not ((Get-Content $f -Raw) -match '"activateMultipleDrivers"\s*:\s*true')
}

function SetMultipleDrivers($f) {
  if (-not (Test-Path $f)) {
    New-Item -ItemType Directory -Force (Split-Path $f) | Out-Null
    $t = "{`n   `"steamvr`" : {`n      `"activateMultipleDrivers`" : true`n   }`n}`n"
  } else {
    $t = Get-Content $f -Raw
    if ($t -match '"activateMultipleDrivers"\s*:\s*false') {
      $t = $t -replace '("activateMultipleDrivers"\s*:\s*)false', '${1}true'
    } elseif ($t -match '"steamvr"\s*:\s*\{') {
      $m = [regex]::Match($t, '"steamvr"\s*:\s*\{')
      $at = $m.Index + $m.Length
      $empty = $t.Substring($at).TrimStart().StartsWith("}")
      $t = $t.Insert($at, "`n      `"activateMultipleDrivers`" : true" + $(if ($empty) { "`n   " } else { "," }))
    } else {
      $at = $t.IndexOf("{") + 1
      if ($at -le 0) { Fail "$f isn't a settings file SteamVR wrote" }
      $empty = $t.Substring($at).TrimStart().StartsWith("}")
      $t = $t.Insert($at, "`n   `"steamvr`" : {`n      `"activateMultipleDrivers`" : true`n   }" + $(if ($empty) { "`n" } else { "," }))
    }
  }
  [IO.File]::WriteAllText($f, $t, (New-Object Text.UTF8Encoding $false))
  Say "Turned on activateMultipleDrivers in $f"
}

# Moves each file of $from into $to; a file that's in use is renamed to <name>.lhs-old first (the driver deletes those)
function Swap($from, $to) {
  $base = (Get-Item -LiteralPath $from).FullName.TrimEnd("\")   # the children's FullName starts with this exact text
  foreach ($src in Get-ChildItem -LiteralPath $base -Recurse -File) {
    $dest = Join-Path $to $src.FullName.Substring($base.Length).TrimStart("\")
    New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
    if (Test-Path $dest) {
      try { Remove-Item $dest -Force } catch {
        $old = "$dest.lhs-old"
        for ($n = 2; Test-Path $old; $n++) { $old = "$dest.lhs-old$n" }
        Move-Item $dest $old -Force
      }
    }
    Move-Item $src.FullName $dest -Force
  }
}

function Install {
  $vrpathreg = VrPathReg   # SteamVR has to be there first
  if ($Version) { $api = "https://api.github.com/repos/$Repo/releases/tags/$Version" }
  else { $api = "https://api.github.com/repos/$Repo/releases/latest" }
  try { $rel = Invoke-RestMethod $api -Headers $Ua -UseBasicParsing }
  catch { Fail "couldn't read $Repo's releases from GitHub: $($_.Exception.Message)" }
  $asset = $rel.assets | Where-Object { $_.name -match '^(LighthouseSync|QuestLHSync)-steamvr-.*\.zip$' } | Select-Object -First 1
  if (-not $asset) { Fail "release $($rel.tag_name) has no SteamVR zip" }

  $want = $null
  if ($asset.digest -match '^sha256:([0-9a-fA-F]{64})$') { $want = $Matches[1] }
  else {
    $sums = $rel.assets | Where-Object { $_.name -eq "SHA256SUMS" } | Select-Object -First 1
    if ($sums) {
      $line = (Invoke-RestMethod $sums.browser_download_url -UseBasicParsing) -split "`n" | Where-Object { $_ -match "\s\*?$([regex]::Escape($asset.name))\s*$" }
      if ($line -match '^([0-9a-fA-F]{64})') { $want = $Matches[1] }
    }
  }
  if (-not $want) { Fail "release $($rel.tag_name) publishes no SHA-256 for $($asset.name), so it can't be checked" }

  $tmp = Join-Path ([IO.Path]::GetTempPath()) ("lighthousesync-" + [guid]::NewGuid().ToString("N"))
  New-Item -ItemType Directory $tmp | Out-Null
  try {
    Say "Downloading $($asset.name) ($([math]::Round($asset.size / 1MB, 1)) MB) from $Repo"
    $zip = Join-Path $tmp $asset.name
    Invoke-WebRequest $asset.browser_download_url -OutFile $zip -Headers @{ "User-Agent" = "LighthouseSync-install" } -UseBasicParsing
    $got = Sha256 $zip
    if ($got -ne $want.ToUpper()) { Fail "the download doesn't match its published SHA-256 (got $got, expected $want)" }
    Say "SHA-256 matches"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, (Join-Path $tmp "unpacked"))
    $new = Join-Path $tmp "unpacked\questlhsync"
    if (-not (Test-Path (Join-Path $new "driver.vrdrivermanifest")) -or -not (Test-Path (Join-Path $new "bin\win64\driver_questlhsync.dll"))) {
      Fail "$($asset.name) has no questlhsync driver folder"
    }

    $settings = SettingsFile
    $needs = NeedsMultipleDrivers $settings
    if ($needs -and (SteamVrRunning)) {
      Say "SteamVR has to close once so its activateMultipleDrivers setting can be turned on." Yellow
      $answer = Read-Host "Close SteamVR now? [Y/n]"
      if ($answer -and $answer -notmatch '^[yY]') { Fail "close SteamVR and run this again" }
      StopSteamVr
    }

    New-Item -ItemType Directory -Force $Driver | Out-Null
    Swap $new $Driver
    Reg adddriver $Driver
    if ($needs -and $settings) { SetMultipleDrivers $settings }
    elseif ($needs) { Say "Couldn't find SteamVR's config folder: set activateMultipleDrivers to true under steamvr in steamvr.vrsettings" Yellow }
    # SteamVR loads one copy of a driver: this one from now on
    foreach ($other in VrPaths "external_drivers") {
      if ((Test-Path (Join-Path $other "bin\win64\driver_questlhsync.dll")) -and
          ((Resolve-Path $other).Path.TrimEnd("\") -ne (Resolve-Path $Driver).Path.TrimEnd("\"))) {
        Reg removedriver $other
        Say "Unregistered the other copy in $other (its files are left as they are)"
      }
    }
    [IO.File]::WriteAllText($State, "{`"version`":`"$($rel.tag_name)`"}")
    Say "Installed LighthouseSync $($rel.tag_name) in $Driver" Green
    if (SteamVrRunning) { Say "Restart SteamVR to load it." Yellow } else { Say "Start SteamVR. The LighthouseSync window opens with it and updates itself from now on." }
  } finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
  }
}

function Remove {
  if (SteamVrRunning) {
    $answer = Read-Host "SteamVR is running and has the driver loaded. Close SteamVR now? [Y/n]"
    if ($answer -and $answer -notmatch '^[yY]') { Fail "close SteamVR and run this again" }
    StopSteamVr
  }
  if (Test-Path $Driver) {
    try { Reg removedriver $Driver } catch { Say $_.Exception.Message Yellow }
    Remove-Item $Driver -Recurse -Force
  }
  Remove-Item $State -Force -ErrorAction SilentlyContinue
  Say "LighthouseSync removed. Your saved base stations, settings and logs are still in $Root; delete that folder to remove them too." Green
}

if ($Uninstall) { Remove } else { Install }
