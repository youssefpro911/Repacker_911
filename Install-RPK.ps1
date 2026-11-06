[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$ProjectRoot = $PSScriptRoot
$BuildRoot = Join-Path $env:LOCALAPPDATA 'RPK-Build'
$BuildDir = Join-Path $BuildRoot 'build-windows-x64'
$VcpkgRoot = Join-Path $BuildRoot 'vcpkg'
$InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\RPK'
$LogPath = Join-Path $env:TEMP 'RPK-Setup.log'

Start-Transcript -Path $LogPath -Append | Out-Null

function Write-Step([string]$Message) {
    Write-Host "`n==> $Message" -ForegroundColor Cyan
}

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    Write-Host ('> ' + $Program + ' ' + ($Arguments -join ' ')) -ForegroundColor DarkGray
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE: $Program"
    }
}

try {
    Write-Host 'RPK Windows setup' -ForegroundColor Green
    Write-Host 'This setup prepares dependencies, builds RPK automatically, and installs the GUI and CLI for your account.'
    Write-Host "Project: $ProjectRoot"
    Write-Host "Log:     $LogPath"

    $cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if (-not $cmakeCommand) {
        throw 'CMake was not found. Install CMake from https://cmake.org/download/ and run this setup again.'
    }
    $gitCommand = Get-Command git.exe -ErrorAction SilentlyContinue
    if (-not $gitCommand) {
        throw 'Git was not found. Install Git for Windows from https://git-scm.com/download/win and run this setup again.'
    }

    $vswhereCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
    )
    $vswhere = $vswhereCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $vswhere) {
        throw 'Visual Studio Installer (vswhere.exe) was not found. Install Visual Studio Community with the Desktop development with C++ workload and Windows 10/11 SDK.'
    }
    $vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $vsInstall) {
        throw 'No Visual Studio C++ x64 toolchain was found. Open Visual Studio Installer, select Modify, and install Desktop development with C++ plus a Windows 10/11 SDK.'
    }

    $cmakeHelp = (& cmake.exe --help 2>&1 | Out-String)
    $generatorMatches = [regex]::Matches($cmakeHelp, '(?m)^\s*(Visual Studio \d+ \d{4})\s*=')
    if ($generatorMatches.Count -eq 0) {
        throw 'This CMake installation does not list a Visual Studio generator. Update CMake from https://cmake.org/download/.'
    }
    $Generator = $generatorMatches[0].Groups[1].Value
    Write-Host "Detected Visual Studio: $vsInstall"
    Write-Host "Using CMake generator: $Generator"

    $env:VCPKG_ROOT = $VcpkgRoot
    New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
    if (-not (Test-Path (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat'))) {
        if (Test-Path $VcpkgRoot) {
            Remove-Item -LiteralPath $VcpkgRoot -Recurse -Force
        }
        Write-Step 'Downloading vcpkg'
        Invoke-Checked $gitCommand.Source @('clone', '--depth', '1', 'https://github.com/microsoft/vcpkg.git', $VcpkgRoot)
    }

    Write-Step 'Bootstrapping vcpkg'
    Invoke-Checked (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') @('-disableMetrics')

    Write-Step 'Installing required native libraries (first run downloads them)'
    Invoke-Checked (Join-Path $VcpkgRoot 'vcpkg.exe') @(
        'install',
        'liblzma:x64-windows',
        'openssl:x64-windows',
        'zlib:x64-windows',
        'zstd:x64-windows'
    )

    if (Test-Path $BuildDir) {
        Remove-Item -LiteralPath $BuildDir -Recurse -Force
    }
    Write-Step 'Configuring RPK'
    Invoke-Checked $cmakeCommand.Source @(
        '-S', $ProjectRoot,
        '-B', $BuildDir,
        '-G', $Generator,
        '-A', 'x64',
        "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $VcpkgRoot 'scripts/buildsystems/vcpkg.cmake')",
        '-DVCPKG_TARGET_TRIPLET=x64-windows',
        '-DVCPKG_APPLOCAL_DEPS=ON'
    )

    Write-Step 'Compiling RPK automatically'
    Invoke-Checked $cmakeCommand.Source @('--build', $BuildDir, '--config', 'Release', '--parallel')

    Write-Step 'Running automated tests'
    Invoke-Checked $cmakeCommand.Source @('--build', $BuildDir, '--config', 'Release', '--target', 'rpk_tests')
    $ctestExe = Join-Path (Split-Path -Parent $cmakeCommand.Source) 'ctest.exe'
    if (-not (Test-Path $ctestExe)) { throw 'ctest.exe was not found beside cmake.exe. Reinstall CMake with testing tools.' }
    Invoke-Checked $ctestExe @('--test-dir', $BuildDir, '-C', 'Release', '--output-on-failure')

    $ReleaseDir = Join-Path $BuildDir 'Release'
    $GuiExe = Join-Path $ReleaseDir 'rpk_gui.exe'
    $CliExe = Join-Path $ReleaseDir 'rpk.exe'
    if (-not (Test-Path $GuiExe) -or -not (Test-Path $CliExe)) {
        throw "Build completed but the expected executables were not found in $ReleaseDir. Check the setup log."
    }

    Write-Step 'Installing RPK for the current Windows user'
    if (Test-Path $InstallDir) {
        Remove-Item -LiteralPath $InstallDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Copy-Item -LiteralPath $GuiExe -Destination $InstallDir
    Copy-Item -LiteralPath $CliExe -Destination $InstallDir
    Get-ChildItem -LiteralPath $ReleaseDir -Filter '*.dll' -File -ErrorAction SilentlyContinue |
        Copy-Item -Destination $InstallDir -Force
    Copy-Item -LiteralPath (Join-Path $ProjectRoot 'README.md') -Destination $InstallDir -ErrorAction SilentlyContinue

    $UninstallerPath = Join-Path $InstallDir 'Uninstall-RPK.ps1'
    @'
$ErrorActionPreference = 'SilentlyContinue'
$InstallDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Programs = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\RPK'
$DesktopLink = Join-Path ([Environment]::GetFolderPath('Desktop')) 'RPK.lnk'
$RegKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RPK'
Remove-Item -LiteralPath $Programs -Recurse -Force
Remove-Item -LiteralPath $DesktopLink -Force
Remove-Item -LiteralPath $RegKey -Recurse -Force
$CmdPath = Join-Path $env:TEMP ("RPK-Remove-" + [guid]::NewGuid().ToString('N') + '.cmd')
@"
@echo off
timeout /t 2 /nobreak >nul
rmdir /s /q "$InstallDir"
del "%~f0"
"@ | Set-Content -LiteralPath $CmdPath -Encoding ASCII
Start-Process -FilePath $env:ComSpec -ArgumentList "/c `"$CmdPath`"" -WindowStyle Hidden
'@ | Set-Content -LiteralPath $UninstallerPath -Encoding UTF8

    $StartMenuDir = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\RPK'
    New-Item -ItemType Directory -Force -Path $StartMenuDir | Out-Null
    $shell = New-Object -ComObject WScript.Shell
    $desktopShortcut = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'RPK.lnk'))
    $desktopShortcut.TargetPath = $GuiExe.Replace($ReleaseDir, $InstallDir)
    $desktopShortcut.WorkingDirectory = $InstallDir
    $desktopShortcut.IconLocation = "$($desktopShortcut.TargetPath),0"
    $desktopShortcut.Save()
    $menuShortcut = $shell.CreateShortcut((Join-Path $StartMenuDir 'RPK.lnk'))
    $menuShortcut.TargetPath = $desktopShortcut.TargetPath
    $menuShortcut.WorkingDirectory = $InstallDir
    $menuShortcut.IconLocation = "$($menuShortcut.TargetPath),0"
    $menuShortcut.Save()

    $UninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RPK'
    New-Item -Path $UninstallKey -Force | Out-Null
    New-ItemProperty -Path $UninstallKey -Name DisplayName -Value 'RPK' -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $UninstallKey -Name DisplayVersion -Value '0.4.0' -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $UninstallKey -Name Publisher -Value 'RPK Project' -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $UninstallKey -Name InstallLocation -Value $InstallDir -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $UninstallKey -Name DisplayIcon -Value (Join-Path $InstallDir 'rpk_gui.exe') -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $UninstallKey -Name UninstallString -Value "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$UninstallerPath`"" -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $UninstallKey -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $UninstallKey -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null

    Write-Host "`nRPK was installed to: $InstallDir" -ForegroundColor Green
    Write-Host 'A desktop shortcut and Start Menu entry were created.'
    Write-Host 'RPK will now launch.'
    Start-Process -FilePath (Join-Path $InstallDir 'rpk_gui.exe') -WorkingDirectory $InstallDir
}
catch {
    Write-Host "`nSETUP FAILED: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Diagnostic log: $LogPath" -ForegroundColor Yellow
    Stop-Transcript | Out-Null
    exit 1
}

Stop-Transcript | Out-Null
exit 0
