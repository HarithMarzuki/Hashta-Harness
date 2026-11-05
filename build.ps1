# Builds the panel with the MinGW-w64 toolchain already on this machine.
#   build.cmd                 build only
#   build.cmd -Shortcut       build and place a shortcut on the Desktop
# Output: build\hashta_ui.exe

param([switch]$Shortcut)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$gpp = 'C:\mingw64\bin\g++.exe'
$windres = 'C:\mingw64\bin\windres.exe'

foreach ($tool in @($gpp, $windres)) {
    if (-not (Test-Path $tool)) { throw "missing toolchain: $tool" }
}

$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null

# g++ refuses to write temporary files in the sandboxed system temp, so it
# gets one inside the workspace.
$tmp = Join-Path $root '.tmp\build'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$env:TMP = $tmp
$env:TEMP = $tmp

$resources = Join-Path $out 'resources.o'
# windres passes -I through to the preprocessor without quoting, so an
# absolute include path containing a space ("Local Agent") breaks it. Run it
# from the project root with relative arguments instead.
Push-Location $root
try {
    & $windres 'ui\resources.rc' -I 'ui' -O coff -o 'build\resources.o'
    if ($LASTEXITCODE -ne 0) { throw "windres failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}

$sources = @(
    'ui\main.cpp',
    'ui\app.cpp',
    'ui\render.cpp',
    'ui\chat.cpp',
    'ui\avatar.cpp',
    'ui\net.cpp'
) | ForEach-Object { Join-Path $root $_ }

$exe = Join-Path $out 'hashta_ui.exe'
& $gpp -std=c++20 -O2 -Wall -Wextra -Wno-cast-function-type `
    -DUNICODE -D_UNICODE -mwindows `
    -o $exe @sources $resources `
    -ld2d1 -ldwrite -lcomctl32 -lole32 -luuid -luser32 -lgdi32 -lwinhttp `
    -static-libgcc -static-libstdc++
if ($LASTEXITCODE -ne 0) { throw "g++ failed ($LASTEXITCODE)" }

$size = [math]::Round((Get-Item $exe).Length / 1KB, 1)
Write-Host "built $exe ($size KB)"

if ($Shortcut) {
    $desktop = [Environment]::GetFolderPath('Desktop')
    $link = Join-Path $desktop 'Hashta.lnk'
    try {
        $shell = New-Object -ComObject WScript.Shell
        $item = $shell.CreateShortcut($link)
        $item.TargetPath = $exe
        $item.WorkingDirectory = $root
        $item.IconLocation = "$exe,0"
        $item.Description = 'Hashta companion panel'
        $item.Save()
        Write-Host "shortcut: $link"
    } catch {
        # A sandboxed session cannot write to the Desktop; put it beside the
        # build instead of failing the whole build.
        $fallback = Join-Path $root 'Hashta.lnk'
        $item = $shell.CreateShortcut($fallback)
        $item.TargetPath = $exe
        $item.WorkingDirectory = $root
        $item.IconLocation = "$exe,0"
        $item.Description = 'Hashta companion panel'
        $item.Save()
        Write-Host "shortcut: $fallback (desktop was not writable)"
    }
}
