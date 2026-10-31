# Archon Mode - product pack build (v1.0.0).
#   1. compiles native/archon_native.c -> archon_native.dll (MSVC, static CRT, /Brepro)
#   2. embeds the DLL as src/script/archon/native/bin_<version>.lua (3-digit \ddd escapes; stale blobs removed)
#   3. structure-checks every Lua file under src/ (tools/lua_check.py)
#   4. builds out/archon_mode.pack with rpfm_cli and lists it
#   5. with -ExeCopy: runs the offline signature test (patches A to G) against a copy of Warhammer3.exe
#   6. assembles out/workshop/: pack, thumbnail, description and upload steps, ready to upload
# Never writes into the game install.
# Usage: pwsh -NoProfile -File build.ps1 [-Python <python>] [-RpfmCli <rpfm_cli>] [-Vcvars <vcvars64.bat>] [-ExeCopy <Warhammer3.exe copy>]
param(
  [string]$Python  = 'python',
  [string]$RpfmCli = 'rpfm_cli',
  [string]$Vcvars  = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
  [string]$ExeCopy = ''
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$py   = $Python
$cli  = $RpfmCli
$exe  = $ExeCopy

$version = '1.0.0'
$vtag    = $version -replace '\.', '_'
$module  = "archon_native_$vtag"
$native  = Join-Path $here 'native'
$bld     = Join-Path $native 'build'
$dll     = Join-Path $bld 'archon_native.dll'
$src     = Join-Path $here 'src'
$binlua  = Join-Path $src "script\archon\native\bin_$vtag.lua"
$out     = Join-Path $here 'out'
$pack    = Join-Path $out 'archon_mode.pack'

foreach ($p in @($py, $cli)) {
  if (-not (Get-Command $p -ErrorAction SilentlyContinue)) { throw "required tool not found: $p" }
}
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }
if ($exe -and -not (Test-Path $exe)) { throw "exe copy not found: $exe" }
New-Item -ItemType Directory -Force $bld | Out-Null
New-Item -ItemType Directory -Force $out | Out-Null
New-Item -ItemType Directory -Force (Split-Path $binlua) | Out-Null

# the C source and the boot loader must agree on the version
$csrc = Get-Content -Raw (Join-Path $native 'archon_native.c')
if ($csrc -notmatch "#define ARCHON_VERSION\s+`"$([regex]::Escape($version))`"") { throw "archon_native.c ARCHON_VERSION is not $version" }
$boot = Get-Content -Raw (Join-Path $src 'script\_lib\mod\!archon_mode_native_boot.lua')
if ($boot -notmatch "bin_$vtag" -or $boot -notmatch "archon_native_$vtag\.dll") { throw "boot loader does not reference bin_$vtag / archon_native_$vtag.dll" }

Write-Host '=== 1. compile archon_native.dll ==='
$cl = "cl /nologo /O2 /MT /W3 /DNDEBUG /LD /Brepro `"$($native)\archon_native.c`" " +
      "/Fe:`"$dll`" /Fo:`"$bld\archon_native.obj`" /link /NXCOMPAT /DYNAMICBASE advapi32.lib"
$cmd = "call `"$vcvars`" >nul && cd /d `"$native`" && $cl"
cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }
if (-not (Test-Path $dll)) { throw "DLL not produced" }
$sz = (Get-Item $dll).Length
$sha = (Get-FileHash -Algorithm SHA256 $dll).Hash.ToLower()
Write-Host "    built $dll  ($sz bytes, sha256=$sha)"
$expCheck = cmd.exe /c "call `"$vcvars`" >nul && dumpbin /exports `"$dll`""
if (($expCheck -join "`n") -notmatch 'luaopen_archon_native') { throw 'DLL does not export luaopen_archon_native' }
Write-Host '    export luaopen_archon_native: present'
$imports = cmd.exe /c "call `"$vcvars`" >nul && dumpbin /imports `"$dll`"" | Select-String -Pattern '^\s+\S+\.dll$' | ForEach-Object { $_.Line.Trim() }
Write-Host "    imports: $($imports -join ', ')"

Write-Host '=== 2. embed DLL as Lua blob ==='
Get-ChildItem (Split-Path $binlua) -Filter 'bin_*.lua' | Where-Object { $_.FullName -ne $binlua } | ForEach-Object {
  Write-Host "    removing stale blob $($_.Name)"; Remove-Item $_.FullName -Force }
& $py -I (Join-Path $here 'tools\dll_to_lua.py') $dll $binlua $module $version
if ($LASTEXITCODE -ne 0) { throw 'dll_to_lua failed' }
# guard the escape fix: every non-printable byte must be a 3-digit \ddd escape
$blob = (Get-Content -Raw $binlua).Replace('\\', '')     # drop escaped backslashes first
if ($blob -match '\\\d{1,2}(?!\d)') { throw 'blob contains a 1- or 2-digit \d escape (escape fix lost)' }

Write-Host '=== 3. Lua structure check ==='
& $py -I (Join-Path $here 'tools\lua_check.py') $src
if ($LASTEXITCODE -ne 0) { throw 'Lua structure check FAILED' }

Write-Host '=== 4. build pack ==='
if (Test-Path $pack) { Remove-Item $pack -Force }
& $cli -g warhammer_3 pack create -p $pack
if ($LASTEXITCODE -ne 0) { throw 'pack create failed' }
& $cli -g warhammer_3 pack add -p $pack -F "$($src)\script;script"
if ($LASTEXITCODE -ne 0) { throw 'pack add failed' }
# 0.5.5: src/ui holds our own tiny layout(s) (ui/archon/marker.twui.xml, the lobby marker image)
if (Test-Path (Join-Path $src 'ui')) {
  & $cli -g warhammer_3 pack add -p $pack -F "$($src)\ui;ui"
  if ($LASTEXITCODE -ne 0) { throw 'pack add (ui) failed' }
}
Write-Host "--- contents of $pack"
$listing = & $cli -g warhammer_3 pack list -p $pack
if ($LASTEXITCODE -ne 0) { throw 'pack list failed' }
$listing | ForEach-Object { Write-Host $_ }
$expected = @()
foreach ($top in @('script', 'ui')) {
  $dir = Join-Path $src $top
  if (Test-Path $dir) {
    $expected += Get-ChildItem -Recurse -File $dir | ForEach-Object {
      "$top/" + ($_.FullName.Substring($dir.Length + 1) -replace '\\', '/') }
  }
}
foreach ($e in $expected) {
  if (-not ($listing | Where-Object { ($_ -replace '\\', '/').Trim() -eq $e })) { throw "pack is missing $e" }
}
Write-Host "    all $($expected.Count) source files present in the pack ($((Get-Item $pack).Length) bytes)"

Write-Host '=== 5. offline signature test against the exe COPY ==='
if ($exe) {
  & $py -I (Join-Path $here 'tools\offline_scan_test.py') $exe
  if ($LASTEXITCODE -ne 0) { throw 'offline signature test FAILED' }
} else {
  Write-Host '    skipped: no -ExeCopy given'
}

Write-Host '=== 6. assemble the Workshop upload folder ==='
$ws    = Join-Path $here 'workshop'
$wsout = Join-Path $here 'out\workshop'
$thumb = Join-Path $ws 'archon_mode.png'
foreach ($p in @($thumb, (Join-Path $ws 'workshop_description.txt'), (Join-Path $ws 'UPLOAD.md'))) {
  if (-not (Test-Path $p)) { throw "required Workshop file not found: $p" }
}
if ((Get-Item $thumb).Length -ge 1MB) { throw 'thumbnail is 1 MB or more: the Workshop refuses it' }
if ($listing | Where-Object { $_ -match 'enable_console_logging' }) { throw 'the debug logging flag must not ship' }
if (Test-Path $wsout) { Remove-Item $wsout -Recurse -Force }
New-Item -ItemType Directory -Force $wsout | Out-Null
Copy-Item $pack, $thumb, (Join-Path $ws 'workshop_description.txt'), (Join-Path $ws 'UPLOAD.md') $wsout
Get-ChildItem $wsout | ForEach-Object { Write-Host "    $($_.Name)  $($_.Length) bytes" }

Write-Host ''
Write-Host "BUILD OK. pack=$pack  dll=$dll (sha256=$sha)"
