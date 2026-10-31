<#
Builds fp_native.exe on Windows 11 with Visual Studio's clang-cl. One command, from the repo root:

    powershell -ExecutionPolicy Bypass -File tools\windows\build_vs.ps1

Steps: check Visual Studio and its components, load the VS developer environment, fetch SDL2/zlib/glslang/Vulkan headers
with vcpkg into build\deps, configure the CMake preset win-clang-cl, build MyGameRecomp, collect the result in
dist\fat-princess-windows-vs. Log: build\vs-build.log. On an ARM64 PC (Snapdragon) the same command builds a native
ARM64 exe: preset win-clang-cl-arm64, result in dist\fat-princess-windows-arm64-vs.

  -Clean        delete build\win-clang-cl first
  -SkipDeps     do not run vcpkg (libraries already installed by an earlier run)
  -Config       Release (default) or RelWithDebInfo (adds PDB files for the Visual Studio debugger)
  -GameFrom     folder of an existing test install whose game\ and elf\ get copied next to the new exe
                (default: C:\Apps\fat-princess-windows if it exists)
  -Yes          install missing Visual Studio components without asking
  -OpenInVS     open the repo folder in Visual Studio when the build is done
#>
param(
    [switch]$Clean,
    [switch]$SkipDeps,
    [ValidateSet('Release', 'RelWithDebInfo')][string]$Config = 'Release',
    [string]$GameFrom = '',
    [switch]$Yes,
    [switch]$OpenInVS
)

$ErrorActionPreference = 'Stop'
$arm64 = ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') -or ($env:PROCESSOR_ARCHITEW6432 -eq 'ARM64')
$arch = if ($arm64) { 'arm64' } else { 'x64' }
$preset = if ($arm64) { 'win-clang-cl-arm64' } else { 'win-clang-cl' }
$toolsId = if ($arm64) { 'Microsoft.VisualStudio.Component.VC.Tools.ARM64' } else { 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force -Path (Join-Path $root 'build') | Out-Null
$log = Join-Path $root 'build\vs-build.log'
Set-Content -Path $log -Value ("build_vs.ps1 " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))

function Say($text) {
    Write-Host $text
    Add-Content -Path $log -Value $text
}

function Fail($text) {
    Say ''
    Say ('FAILED: ' + $text)
    Say ('Log: ' + $log)
    exit 1
}

# Runs a native command, mirrors its output to the console and the log, returns its exit code.
function Run($exe, $argList) {
    Say ('> ' + $exe + ' ' + ($argList -join ' '))
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $exe @argList 2>&1 | ForEach-Object {
        $line = $_.ToString()
        Write-Host $line
        Add-Content -Path $log -Value $line
    }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return $code
}

Say '== Checking the repo'
if (-not (Test-Path (Join-Path $root 'third_party\ps3recomp\CMakeLists.txt'))) {
    Fail 'third_party\ps3recomp is empty. The framework is a nested repo: run  git submodule update --init  or copy the folder from the Linux machine.'
}
foreach ($d in @('private\lift\game', 'private\lift\spu', 'private\lift\overlays')) {
    $p = Join-Path $root $d
    $n = 0
    if (Test-Path $p) { $n = @(Get-ChildItem -Path $p -Recurse -Include *.cpp, *.c -ErrorAction SilentlyContinue).Count }
    if ($n -eq 0) {
        Fail ("$d is missing or empty. The lifted game code is not in git. Copy the whole private\lift folder from testhost (/mnt/projects/fat-princess-recomp/private/lift) to $root\private\lift and run this again.")
    }
    Say ("  $d : $n source files")
}
if (-not (Select-String -Path (Join-Path $root 'private\lift\game\ppu_recomp.h') -Pattern 'extern "C" unsigned char g_fp_patch' -Quiet)) {
    Fail 'private\lift\game\ppu_recomp.h is the unpatched header (g_fp_patch without extern "C"). Copy private\lift again from testhost, or run python tools\lift_patches.py.'
}

Say '== Checking Visual Studio'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    Fail 'Visual Studio is not installed (vswhere.exe not found). Install Visual Studio 2022 Community: https://visualstudio.microsoft.com/downloads/  and tick "Desktop development with C++".'
}
$vsPath = (& $vswhere -latest -products '*' -requires $toolsId -property installationPath | Select-Object -First 1)
if (-not $vsPath) {
    $vsAny = (& $vswhere -latest -products '*' -property installationPath | Select-Object -First 1)
    if (-not $vsAny) { Fail 'vswhere finds no Visual Studio installation.' }
    $vsPath = $vsAny
}
Say ("  Visual Studio: $vsPath")

$components = @(
    @{ Id = $toolsId; Name = ('MSVC ' + $arch + ' build tools') },
    @{ Id = 'Microsoft.VisualStudio.Component.VC.Llvm.Clang'; Name = 'C++ Clang compiler for Windows' },
    @{ Id = 'Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset'; Name = 'MSBuild support for LLVM (clang-cl) toolset' },
    @{ Id = 'Microsoft.VisualStudio.Component.VC.CMake.Project'; Name = 'C++ CMake tools for Windows' },
    @{ Id = 'Microsoft.VisualStudio.Component.Windows11SDK.22621'; Name = 'Windows 11 SDK (22621)' }
)
$missing = @()
foreach ($c in $components) {
    $found = (& $vswhere -latest -products '*' -requires $c.Id -property installationPath | Select-Object -First 1)
    if (-not $found) { $missing += $c }
}
# any Windows 10 or 11 SDK is good enough
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
if ($missing.Count -gt 0 -and (Test-Path $kits)) {
    if (@(Get-ChildItem -Path $kits -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'um\windows.h') }).Count -gt 0) {
        $missing = @($missing | Where-Object { $_.Id -notlike '*Windows11SDK*' })
    }
}
if ($missing.Count -gt 0) {
    Say '  Missing Visual Studio components:'
    foreach ($m in $missing) { Say ('    ' + $m.Id + '  (' + $m.Name + ')') }
    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
    $addArgs = @('modify', '--installPath', ('"' + $vsPath + '"'))
    foreach ($m in $missing) { $addArgs += @('--add', $m.Id) }
    $addArgs += @('--passive', '--norestart')
    Say ('  Command that installs them (needs an administrator prompt): "' + $installer + '" ' + ($addArgs -join ' '))
    $go = [bool]$Yes
    if (-not $go) {
        $answer = Read-Host '  Install them now? (y/n)'
        $go = ($answer -eq 'y' -or $answer -eq 'Y')
    }
    if (-not $go) { Fail 'Components missing. Run the command above (or Visual Studio Installer > Modify > tick them) and start this script again.' }
    $p = Start-Process -FilePath $installer -ArgumentList $addArgs -Verb RunAs -Wait -PassThru
    Say ('  Installer exit code ' + $p.ExitCode)
    if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) { Fail 'The Visual Studio Installer did not finish. Close Visual Studio and try again, or install the components by hand.' }
    foreach ($m in $missing) {
        if ($m.Id -like '*Windows11SDK*') { continue }
        $found = (& $vswhere -latest -products '*' -requires $m.Id -property installationPath | Select-Object -First 1)
        if (-not $found) { Fail ('Component still missing after install: ' + $m.Id) }
    }
}

Say ('== Loading the Visual Studio ' + $arch + ' developer environment')
$vcvars = Join-Path $vsPath ('VC\Auxiliary\Build\' + $(if ($arm64) { 'vcvarsarm64.bat' } else { 'vcvars64.bat' }))
if (-not (Test-Path $vcvars)) { Fail ("$(Split-Path -Leaf $vcvars) not found under $vsPath") }
$tmpCmd = Join-Path $env:TEMP 'fp_vcvars.cmd'
Set-Content -Path $tmpCmd -Encoding ASCII -Value @('@echo off', ('call "' + $vcvars + '" >nul 2>&1'), 'set')
$envDump = & cmd.exe /c $tmpCmd
Remove-Item -Force $tmpCmd -ErrorAction SilentlyContinue
foreach ($line in $envDump) {
    $i = $line.IndexOf('=')
    if ($i -gt 0) { Set-Item -Path ('Env:' + $line.Substring(0, $i)) -Value $line.Substring($i + 1) }
}
$extra = @(
    (Join-Path $vsPath ('VC\Tools\Llvm\' + $(if ($arm64) { 'ARM64' } else { 'x64' }) + '\bin')),
    (Join-Path $vsPath 'VC\Tools\Llvm\bin'),
    (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'),
    (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja')
)
foreach ($e in $extra) {
    # Llvm\bin holds the 32-bit x86 clang-cl; prepended after Llvm\ARM64\bin it would win on an ARM64 PC.
    if ($arm64 -and $e -eq (Join-Path $vsPath 'VC\Tools\Llvm\bin')) { continue }
    if (Test-Path $e) { $env:PATH = $e + ';' + $env:PATH }
}
foreach ($tool in @('clang-cl', 'cmake', 'ninja')) {
    $cmd = Get-Command $tool -ErrorAction SilentlyContinue
    if (-not $cmd) { Fail ("$tool not found in the Visual Studio environment. The component list above should have covered it: check Visual Studio Installer > Modify.") }
    Say ('  ' + $tool + ': ' + $cmd.Source)
}

Say '== Libraries (vcpkg)'
$depsDir = Join-Path $root 'build\deps'
$vcpkgRoot = Join-Path $depsDir 'vcpkg'
$triplet = $arch + '-windows-static-md'
$env:VCPKG_ROOT = $vcpkgRoot
$env:VCPKG_DISABLE_METRICS = '1'
if (-not $SkipDeps) {
    New-Item -ItemType Directory -Force -Path $depsDir | Out-Null
    if (-not (Test-Path (Join-Path $vcpkgRoot 'bootstrap-vcpkg.bat'))) {
        $git = Get-Command git -ErrorAction SilentlyContinue
        if ($git) {
            $code = Run 'git' @('clone', '--depth', '1', 'https://github.com/microsoft/vcpkg.git', $vcpkgRoot)
            if ($code -ne 0) { Fail 'git clone of vcpkg failed. Check the internet connection.' }
        } else {
            Say '  git not found, downloading the vcpkg zip instead'
            $zip = Join-Path $depsDir 'vcpkg.zip'
            Invoke-WebRequest -Uri 'https://github.com/microsoft/vcpkg/archive/refs/heads/master.zip' -OutFile $zip
            Expand-Archive -Path $zip -DestinationPath $depsDir -Force
            Rename-Item -Path (Join-Path $depsDir 'vcpkg-master') -NewName 'vcpkg'
            Remove-Item $zip
        }
    }
    if (-not (Test-Path (Join-Path $vcpkgRoot 'vcpkg.exe'))) {
        $code = Run (Join-Path $vcpkgRoot 'bootstrap-vcpkg.bat') @('-disableMetrics')
        if ($code -ne 0) { Fail 'vcpkg bootstrap failed.' }
    }
    $code = Run (Join-Path $vcpkgRoot 'vcpkg.exe') @('install', 'sdl2', 'zlib', 'glslang', 'vulkan-headers', ('--triplet=' + $triplet), '--clean-after-build')
    if ($code -ne 0) { Fail 'vcpkg could not install the libraries (sdl2 zlib glslang vulkan-headers). The first error above says which one.' }
} else {
    Say '  -SkipDeps: using whatever is already in build\deps\vcpkg\installed'
}
$vkInclude = Join-Path $vcpkgRoot ('installed\' + $triplet + '\include')
if (-not (Test-Path (Join-Path $vkInclude 'vulkan\vulkan.h'))) { Fail ("vulkan\vulkan.h not found in $vkInclude. Run again without -SkipDeps.") }
if ($env:VULKAN_SDK) {
    Say ('  Vulkan SDK found (' + $env:VULKAN_SDK + '): FP_VK_VALIDATION=1 works at run time.')
} else {
    Say '  Vulkan SDK not installed. Not needed to build or run. Optional, for the validation layer: winget install KhronosGroup.VulkanSDK'
}

Say '== Configure'
$buildDir = Join-Path $root ('build\' + $preset)
if ($Clean -and (Test-Path $buildDir)) {
    Say ('  -Clean: removing build\' + $preset)
    Remove-Item -Recurse -Force $buildDir
}
$code = Run 'cmake' @('--preset', $preset, ('-DCMAKE_BUILD_TYPE=' + $Config), ('-DPS3RECOMP_VULKAN_INCLUDE_DIR=' + $vkInclude))
if ($code -ne 0) { Fail 'CMake configure failed. The first "CMake Error" above says what is missing.' }

Say '== Build (several minutes; the lifted game is a few hundred source files)'
(Get-Process -Id $PID).PriorityClass = 'BelowNormal'
$jobs = [Math]::Max(1, [Environment]::ProcessorCount - 1)
$code = Run 'cmake' @('--build', $buildDir, '--target', 'MyGameRecomp', '--parallel', $jobs, '--', '-k', '0')
if ($code -ne 0) { Fail 'Build failed. Search the log for "error:" (build\vs-build.log). Send that file back.' }
$exe = Get-ChildItem -Path $buildDir -Filter 'fp_native.exe' -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $exe) { Fail ('The build finished but fp_native.exe was not found under build\' + $preset + '.') }
Say ('  ' + $exe.FullName + '  ' + [Math]::Round($exe.Length / 1MB) + ' MB')

Say '== Collecting dist\fat-princess-windows-vs'
$out = Join-Path $root $(if ($arm64) { 'dist\fat-princess-windows-arm64-vs' } else { 'dist\fat-princess-windows-vs' })
New-Item -ItemType Directory -Force -Path $out | Out-Null
Copy-Item -Force $exe.FullName $out
$pdb = [System.IO.Path]::ChangeExtension($exe.FullName, '.pdb')
if (Test-Path $pdb) { Copy-Item -Force $pdb $out }
# run.bat and README come from tools\make_package.py so there is one copy of the text.
$pkg = Join-Path $root 'tools\make_package.py'
$py = if (Test-Path $pkg) { Get-Content -Raw -Path $pkg } else { '' }
$m = [regex]::Match($py, "WINDOWS_RUN = r'''(.*?)'''", 'Singleline')
if ($m.Success) {
    $bat = $m.Groups[1].Value.Replace("`r`n", "`n").Replace("`n", "`r`n")
    [System.IO.File]::WriteAllText((Join-Path $out 'run.bat'), $bat, [System.Text.Encoding]::ASCII)
} else {
    Say '  WARNING: WINDOWS_RUN not found in tools\make_package.py, run.bat not written'
}
$m = [regex]::Match($py, 'README = """(.*?)"""', 'Singleline')
if ($m.Success) {
    $readme = $m.Groups[1].Value.Replace('{run}', 'run.bat').Replace('{where}', 'next to run.bat')
    [System.IO.File]::WriteAllText((Join-Path $out 'README.txt'), $readme, [System.Text.Encoding]::ASCII)
}
$ini = Join-Path $out 'settings.ini'
if (-not (Test-Path $ini)) {
    Set-Content -Path $ini -Encoding ASCII -Value @('fps_cap=AUTO', 'online=ON', 'server=127.0.0.1:36100', 'name=WinVS', 'p2p_port=3658')
}
if (-not $GameFrom -and (Test-Path 'C:\Apps\fat-princess-windows\game')) { $GameFrom = 'C:\Apps\fat-princess-windows' }
if ($GameFrom -and (Test-Path (Join-Path $GameFrom 'game'))) {
    Say ("  copying game\ and elf\ from $GameFrom")
    foreach ($sub in @('game', 'elf')) {
        if (Test-Path (Join-Path $GameFrom $sub)) {
            & robocopy (Join-Path $GameFrom $sub) (Join-Path $out $sub) /E /NFL /NDL /NJH /NJS /NP | Out-Null
        }
    }
}

Say ''
Say 'SUCCESS'
Say ('  Folder: ' + $out)
if (-not (Test-Path (Join-Path $out 'game'))) {
    Say '  Next: copy the game\ and elf\ folders from your existing test install (the dist\fat-princess-windows folder) into that folder.'
}
Say '  Then run run.bat. Send back play.log, the shots folder, vulkaninfo.txt and sysinfo.txt.'
if ($Config -eq 'RelWithDebInfo') {
    Say '  PDB files are next to the exe: in Visual Studio use Debug > Attach to Process, or open the exe with File > Open > Project/Solution.'
}
Say ('  Log: ' + $log)

if ($OpenInVS) {
    $devenv = Join-Path $vsPath 'Common7\IDE\devenv.exe'
    if (Test-Path $devenv) { Start-Process -FilePath $devenv -ArgumentList ('"' + $root + '"') }
}
