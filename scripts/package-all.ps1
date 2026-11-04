# Builds every PureSheen plugin (the suite and the single-module plugins of installer\products.csv) and packages each:
#   dist\<Name>-Setup-<version>.exe      the installer
#   dist\<Name>-<version>-Manual.zip     the .vst3 folder, the .clap file and "How to install.txt", for people who would
#                                        rather copy plain files than run an exe
# <Name> is the plugin's name with hyphens: PureSheen-EQ-Setup-0.8.0.exe. Both come from the static-runtime Release build
# in build-release, never from scripts\build.bat: the people these go to need no Visual C++ redistributable. The script
# fails if a packaged binary imports it.
#
#   powershell -ExecutionPolicy Bypass -File scripts\package-all.ps1
#     -Only eq,tape    only these plugins (their ids in products.csv; "suite" is the suite)
#     -SkipBuild       package what is already built
#     -TestRoot <dir>  installers that install below <dir> without admin rights (files named ...-TEST.exe; no zip)
param(
    [string[]] $Only,
    [switch] $SkipBuild,
    [string] $TestRoot
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build-release"
$distDir = Join-Path $root "dist"
$payloadRoot = Join-Path $root "installer\payload"

function Fail ([string] $message) {
    Write-Host "FAILED: $message" -ForegroundColor Red
    exit 1
}

# Native tools report through their exit code.
function Run ([string] $what, [scriptblock] $command) {
    & $command
    if ($LASTEXITCODE -ne 0) { Fail "$what (exit code $LASTEXITCODE)" }
}

#------------------------------------------------------------------------------
# Tools
$iscc = @(
    (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
    (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
    (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe")
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $iscc) { Fail "Inno Setup 6 not found. Install it from https://jrsoftware.org/isdl.php" }

if (-not $env:VSCMD_VER) {
    $vcvars = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $vcvars) { Fail "Visual Studio (vcvars64.bat) not found" }

    # Take over the environment vcvars sets up: cl, link and dumpbin.
    foreach ($line in (cmd /c "`"$vcvars`" >nul && set")) {
        $split = $line.IndexOf("=")
        if ($split -gt 0) { Set-Item -LiteralPath ("env:" + $line.Substring(0, $split)) -Value $line.Substring($split + 1) }
    }
}

#------------------------------------------------------------------------------
# What to package
$version = (Select-String -LiteralPath (Join-Path $root "CMakeLists.txt") -Pattern '^project\(PureSheen VERSION ([0-9.]+)').Matches[0].Groups[1].Value
if (-not $version) { Fail "no version in CMakeLists.txt" }

$products = Import-Csv -LiteralPath (Join-Path $root "installer\products.csv")
if ($Only) {
    $Only = $Only | ForEach-Object { $_ -split "," }
    foreach ($id in $Only) {
        if (-not ($products | Where-Object { $_.id -eq $id })) { Fail "'$id' is not in installer\products.csv" }
    }
    $products = $products | Where-Object { $Only -contains $_.id }
}

function Target-Of ($product) { if ($product.id -eq "suite") { "PureSheen" } else { "PureSheen_" + $product.id } }

#------------------------------------------------------------------------------
# Build
if (-not $SkipBuild) {
    Run "configure" { cmake -S $root -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded }
    $targets = @()
    foreach ($p in $products) {
        $t = Target-Of $p
        $targets += "${t}_VST3", "${t}_CLAP"
        if ($p.id -eq "suite") { $targets += "${t}_Standalone" }
    }
    Run "build" { cmake --build $buildDir --target $targets }
}

#------------------------------------------------------------------------------
# The binaries, checked
$failedRuntime = @()
foreach ($p in $products) {
    $art = Join-Path $buildDir ((Target-Of $p) + "_artefacts\Release")
    $files = @(
        (Join-Path $art ("VST3\" + $p.name + ".vst3\Contents\x86_64-win\" + $p.name + ".vst3")),
        (Join-Path $art ("CLAP\" + $p.name + ".clap"))
    )
    if ($p.id -eq "suite") { $files += (Join-Path $art ("Standalone\" + $p.name + ".exe")) }
    foreach ($f in $files) {
        if (-not (Test-Path -LiteralPath $f)) { Fail "not built: $f" }
        $imports = dumpbin /dependents $f
        if ($LASTEXITCODE -ne 0) { Fail "dumpbin on $f" }
        if ($imports -match "MSVCP140|VCRUNTIME140") { $failedRuntime += $f }
    }
}
if ($failedRuntime) { Fail ("these need the Visual C++ runtime:`n  " + ($failedRuntime -join "`n  ")) }
Write-Host "Runtime check passed: no packaged binary imports MSVCP140 or VCRUNTIME140."

#------------------------------------------------------------------------------
# Plain-language text. The same words go in the installer's last page and in the zip, where it matters most.
function Quick-Start-Text ($p) {
    $presets = if ($p.id -eq "dither") { "Put it last, after every other plugin, and use it only when you export at a lower bit depth." }
               else { "Your presets are shared with PureSheen Dynamics: a preset saved for this module in`none shows up in the other." }
    @"
$($p.name.ToUpper()) - QUICK START
$('=' * ($p.name.Length + 14))

$($p.name) is $($p.description). It is one module of PureSheen Dynamics as a
plugin of its own.

In FL Studio:
1. Options > Manage plugins > "Find installed plugins" (or "Start scan").
2. Open the Mixer, select a track and add $($p.name) to a free effect slot.
   Pick the VST3 version.

$presets

The "?" button in the top-right corner of the plugin explains everything.
"@
}

function How-To-Install-Text ($p) {
    @"
$($p.name) $version - install by hand
$('=' * ($p.name.Length + $version.Length + 19))

These are plain files. Nothing here runs by itself, and you do not need anything
else on the PC.

First: close FL Studio. If Windows blocked the zip you downloaded, right-click the
zip, choose Properties, tick "Unblock", click OK, and unpack it again.

VST3 (use this one in FL Studio)
Copy the whole folder "$($p.name).vst3" (the folder, not just the file inside it)
to one of these:

  A. C:\Program Files\Common Files\VST3
     FL Studio looks there by itself. Windows asks for administrator rights.

  B. Any folder you like, with no administrator rights needed. Then add that folder
     in FL Studio: Options > Manage plugins > Plugin search paths.

Then in FL Studio: Options > Manage plugins > "Find installed plugins" (or "Start
scan"). The plugin appears as $($p.name).

CLAP (only if your DAW uses CLAP)
Copy "$($p.name).clap" to C:\Program Files\Common Files\CLAP (administrator rights
again), or to a folder your DAW scans.

To remove it, delete what you copied.
"@
}

function Write-Text ([string] $path, [string] $text) {
    $text = ($text -replace "`r?`n", "`r`n") + "`r`n"
    if ($text -match "[^\x00-\x7F]") { Fail "non-ASCII text for $path" }
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.ASCIIEncoding))
}

#------------------------------------------------------------------------------
# Package
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
$extraIscc = @()
if ($TestRoot) { $extraIscc += "/DTestRoot=$TestRoot" }

foreach ($p in $products) {
    $name = $p.name
    $fileName = $name -replace " ", "-"
    $art = Join-Path $buildDir ((Target-Of $p) + "_artefacts\Release")
    $vst3 = Join-Path $art "VST3\$name.vst3"
    $clap = Join-Path $art "CLAP\$name.clap"
    Write-Host "Packaging $name ..."

    # The installer
    if ($p.id -eq "suite") {
        if (Test-Path -LiteralPath $payloadRoot) {
            Get-ChildItem -LiteralPath $payloadRoot -Directory | Where-Object { @("VST3", "CLAP", "Standalone") -contains $_.Name } |
                Remove-Item -Recurse -Force
        }
        foreach ($d in "VST3", "CLAP", "Standalone") { New-Item -ItemType Directory -Force -Path (Join-Path $payloadRoot $d) | Out-Null }
        Copy-Item -LiteralPath $vst3 -Destination (Join-Path $payloadRoot "VST3") -Recurse
        Copy-Item -LiteralPath $clap -Destination (Join-Path $payloadRoot "CLAP")
        Copy-Item -LiteralPath (Join-Path $art "Standalone\$name.exe") -Destination (Join-Path $payloadRoot "Standalone")
        Run "installer of $name" { & $iscc /Q "/DAppVersion=$version" @extraIscc (Join-Path $root "installer\PureSheen.iss") }
    }
    else {
        $payload = Join-Path $payloadRoot $p.id
        if (Test-Path -LiteralPath $payload) { Remove-Item -LiteralPath $payload -Recurse -Force }
        foreach ($d in "VST3", "CLAP") { New-Item -ItemType Directory -Force -Path (Join-Path $payload $d) | Out-Null }
        Copy-Item -LiteralPath $vst3 -Destination (Join-Path $payload "VST3") -Recurse
        Copy-Item -LiteralPath $clap -Destination (Join-Path $payload "CLAP")
        Write-Text (Join-Path $payload "QuickStart.txt") (Quick-Start-Text $p)
        Run "installer of $name" {
            & $iscc /Q "/DAppName=$name" "/DAppVersion=$version" "/DAppGuid=$($p.installer_guid)" "/DPayload=payload\$($p.id)" `
                "/DOutputName=$fileName-Setup-$version" "/DDescription=$($p.description)" @extraIscc (Join-Path $root "installer\PureSheenSingle.iss")
        }
    }
    if ($TestRoot) { continue }
    if (-not (Test-Path -LiteralPath (Join-Path $distDir "$fileName-Setup-$version.exe"))) { Fail "no installer for $name" }

    # The zip: entry names with forward slashes, so every unpacker keeps the folder.
    $zipPath = Join-Path $distDir "$fileName-$version-Manual.zip"
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    $textPath = Join-Path $env:TEMP ("puresheen-howto-" + [guid]::NewGuid().ToString() + ".txt")
    Write-Text $textPath (How-To-Install-Text $p)
    $zip = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in Get-ChildItem -LiteralPath $vst3 -Recurse -File) {
            $relative = $f.FullName.Substring($vst3.Length).TrimStart("\") -replace "\\", "/"
            [void] [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f.FullName, "$name.vst3/$relative", [System.IO.Compression.CompressionLevel]::Optimal)
        }
        [void] [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $clap, "$name.clap", [System.IO.Compression.CompressionLevel]::Optimal)
        [void] [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $textPath, "How to install.txt", [System.IO.Compression.CompressionLevel]::Optimal)
    }
    finally {
        $zip.Dispose()
        Remove-Item -LiteralPath $textPath -Force
    }
}

if (-not $TestRoot) {
    Write-Host ""
    Write-Host "Ready in $distDir :"
    foreach ($p in $products) {
        $fileName = $p.name -replace " ", "-"
        foreach ($f in "$fileName-Setup-$version.exe", "$fileName-$version-Manual.zip") {
            $item = Get-Item -LiteralPath (Join-Path $distDir $f)
            Write-Host ("  {0,-50} {1,6:N1} MB" -f $f, ($item.Length / 1MB))
        }
    }
}
