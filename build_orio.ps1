<#
  Compilation de Orio sous Windows (Visual Studio 2022 / Build Tools).

  Usage :
    powershell -ExecutionPolicy Bypass -File build_orio.ps1 -Step deps   # dépendances (long)
    powershell -ExecutionPolicy Bypass -File build_orio.ps1 -Step app    # application
    powershell -ExecutionPolicy Bypass -File build_orio.ps1 -Step all
    powershell -ExecutionPolicy Bypass -File build_orio.ps1 -Step app -NoInstall   # compile sans toucher
                                                        # à install_dir (Orio peut rester ouvert)
    powershell -ExecutionPolicy Bypass -File build_orio.ps1 -Step app -Reconfigure # force la reconfiguration CMake

  Build incrémental : un dossier build\ déjà configuré n'est pas reconfiguré (sinon SLIC3R_BUILD_TIME,
  inclus par l'en-tête précompilé, change et tout est recompilé). MSBuild relance CMake de lui-même quand
  un CMakeLists.txt change ; -Reconfigure sert après un changement d'options ou de dépendances.

  Résultats :
    dep_win_new\      dépendances installées
    build\            dossier de compilation de l'application
    install_dir\      Orio prêt à lancer (orio.exe)
  Journaux : deps\build\*.log et build\*.log (UTF-8)
#>
param(
    [ValidateSet('deps', 'app', 'all')]
    [string]$Step = 'all',
    # Compiler l'application sans l'installer dans install_dir (fichiers verrouillés si elle est ouverte).
    [switch]$NoInstall,
    # Reconfigurer CMake même si build\ est déjà configuré (options ou dépendances changées).
    [switch]$Reconfigure
)

$ErrorActionPreference = 'Stop'
$root      = $PSScriptRoot
$cmake     = 'C:\Program Files\CMake\bin\cmake.exe'
$perlBin   = Join-Path $root 'tools\strawberry\perl\bin'
$pkgConfig = Join-Path $root 'tools\pkgconf\pkg-config.exe'
$depsDest  = Join-Path $root 'dep_win_new'
$winSdk    = 'C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0'

if (-not (Test-Path $cmake)) { throw "CMake introuvable : $cmake" }

# PATH propre : pas de chaîne MinGW (Strawberry c\bin, Git mingw64) ni de pkg-config,
# sinon CMake peut trouver des bibliothèques MinGW (zlib, iconv…) et les mélanger au build MSVC.
$cleanPath = ($env:PATH -split ';' | Where-Object { $_ -and ($_ -notmatch 'Strawberry|\\Git\\mingw64|\\Git\\usr|pkgconf') }) -join ';'
$env:PATH = "$(Split-Path $cmake);$cleanPath"
if (Test-Path $perlBin) { $env:PATH = "$perlBin;$env:PATH" }

function Invoke-Logged([string]$log, [string[]]$arguments) {
    "`n=== $(Get-Date -Format s) : cmake $($arguments -join ' ')" | Out-File -Append -Encoding utf8 $log
    # Sous Windows PowerShell 5.1, chaque ligne de stderr d'un exécutable devient une erreur :
    # on repasse en 'Continue' le temps de l'appel pour ne pas s'arrêter sur un simple avertissement.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $cmake @arguments 2>&1 | ForEach-Object { "$_" } | Out-File -Append -Encoding utf8 $log
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code -ne 0) { throw "Échec (code $code), voir $log" }
}

$msbuildArgs = @('--', '/m', '/nr:false', '/v:minimal')

if ($Step -in 'deps', 'all') {
    if (-not (Test-Path (Join-Path $perlBin 'perl.exe'))) { throw "Perl portable introuvable dans $perlBin (nécessaire pour OpenSSL)" }
    $build = Join-Path $root 'deps\build'
    New-Item -ItemType Directory -Force $build | Out-Null
    $log = Join-Path $build 'deps.log'
    $t0 = Get-Date
    Invoke-Logged $log @('-S', (Join-Path $root 'deps'), '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64',
        "-DDESTDIR=$($depsDest -replace '\\','/')", '-DCMAKE_BUILD_TYPE=Release', '-DDEP_DEBUG=OFF',
        "-DDEP_DOWNLOAD_DIR=$((Join-Path $root 'deps\build_downloads') -replace '\\','/')")
    Invoke-Logged $log (@('--build', $build, '--config', 'Release') + $msbuildArgs)
    "=== Dépendances terminées en $([int]((Get-Date) - $t0).TotalMinutes) min" | Out-File -Append -Encoding utf8 $log
}

if ($Step -in 'app', 'all') {
    # pkg-config sert à trouver FFmpeg (src/slic3r/CMakeLists.txt) ; il ne cherche que dans nos dépendances.
    if (-not (Test-Path $pkgConfig)) { throw "pkg-config introuvable : $pkgConfig" }
    $env:PKG_CONFIG_LIBDIR = "$(($depsDest -replace '\\','/'))/usr/local/lib/pkgconfig"
    $build = Join-Path $root 'build'
    New-Item -ItemType Directory -Force $build | Out-Null
    $log = Join-Path $build 'app.log'
    $t0 = Get-Date
    # Reconfigurer réécrit SLIC3R_BUILD_TIME (heure à la seconde) dans libslic3r_version.h, inclus par
    # l'en-tête précompilé : tout serait recompilé (~20 min). Un build\ déjà configuré est donc réutilisé
    # tel quel ; MSBuild relance CMake de lui-même si un CMakeLists.txt change (projet ZERO_CHECK).
    $configure = $Reconfigure -or -not ((Test-Path (Join-Path $build 'CMakeCache.txt')) -and (Test-Path (Join-Path $build 'ALL_BUILD.vcxproj')))
    $mode = if ($configure) { 'configuration CMake' } else { 'configuration CMake conservee, build incremental' }
    "`n=== $(Get-Date -Format s) : application ($mode)" | Out-File -Append -Encoding utf8 $log
    if ($configure) {
        Invoke-Logged $log @('-S', $root, '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64',
            '-DBBL_RELEASE_TO_PUBLIC=1', '-DBBL_INTERNAL_TESTING=0',
            "-DPKG_CONFIG_EXECUTABLE=$($pkgConfig -replace '\\','/')",
            "-DCMAKE_PREFIX_PATH=$(($depsDest -replace '\\','/'))/usr/local",
            "-DCMAKE_INSTALL_PREFIX=$((Join-Path $root 'install_dir') -replace '\\','/')",
            '-DCMAKE_BUILD_TYPE=Release', "-DWIN10SDK_PATH=$winSdk")
    }
    $target = if ($NoInstall) { 'ALL_BUILD' } else { 'install' }
    Invoke-Logged $log (@('--build', $build, '--target', $target, '--config', 'Release') + $msbuildArgs)
    "=== Application terminée en $([int]((Get-Date) - $t0).TotalMinutes) min" | Out-File -Append -Encoding utf8 $log
}
