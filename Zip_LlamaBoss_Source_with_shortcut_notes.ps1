# Zip_LlamaBoss_Source.ps1
# Zips only .h and .cpp files, preserving folder structure.
# Compatible with Windows PowerShell 5.1.
# Desktop shortcut helper:
#   1. Keep this .ps1 file in the LlamaBoss source root.
#   2. Create a shortcut to Run_Zip_LlamaBoss_Source.cmd on your desktop.
#   3. The launcher runs:
#      powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Zip_LlamaBoss_Source.ps1
#

$SourceRoot = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$SourceRoot = (Resolve-Path $SourceRoot).Path.TrimEnd('\')

$TimeStamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ZipPath = Join-Path $SourceRoot "LlamaBoss_Source_h_cpp_$TimeStamp.zip"

# Folders to skip
$ExcludedDirNames = @(
    ".git",
    ".vs",
    "x64",
    "Debug",
    "Release",
    "build",
    "out",
    "packages",
    "installer_stage",
    "vcpkg_installed",
    "bin",
    "obj",
    "runtimes",
    "external",
    "third_party",
    "_deps",
    "CMakeFiles"
)

function Get-RelativePathCompat {
    param(
        [string]$BasePath,
        [string]$FullPath
    )

    $base = [System.IO.Path]::GetFullPath($BasePath).TrimEnd('\') + '\'
    $target = [System.IO.Path]::GetFullPath($FullPath)

    if ($target.StartsWith($base, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $target.Substring($base.Length)
    }

    return [System.IO.Path]::GetFileName($FullPath)
}

function Test-IsExcludedPath {
    param(
        [string]$FullPath
    )

    $relativePath = Get-RelativePathCompat -BasePath $SourceRoot -FullPath $FullPath
    $parts = $relativePath -split '[\\/]'

    foreach ($part in $parts) {
        foreach ($excluded in $ExcludedDirNames) {
            if ($part -ieq $excluded) {
                return $true
            }
        }
    }

    return $false
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

if (Test-Path $ZipPath) {
    Remove-Item $ZipPath -Force
}

$files = Get-ChildItem -Path $SourceRoot -Recurse -File -Force -ErrorAction SilentlyContinue |
    Where-Object {
        ($_.Extension -ieq ".cpp" -or $_.Extension -ieq ".h") -and
        -not (Test-IsExcludedPath -FullPath $_.FullName)
    }

$zip = [System.IO.Compression.ZipFile]::Open($ZipPath, "Create")

try {
    foreach ($file in $files) {
        $relativePath = Get-RelativePathCompat -BasePath $SourceRoot -FullPath $file.FullName
        $zipEntryPath = $relativePath.Replace('\', '/')

        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip,
            $file.FullName,
            $zipEntryPath,
            [System.IO.Compression.CompressionLevel]::Optimal
        ) | Out-Null
    }
}
finally {
    $zip.Dispose()
}

Write-Host ""
Write-Host "Created source ZIP:"
Write-Host $ZipPath
Write-Host ""
Write-Host "Files included:" $files.Count
Write-Host ""

if ($files.Count -gt 0) {
    Write-Host "Sample included files:"
    $files | Select-Object -First 15 | ForEach-Object {
        Write-Host " - $(Get-RelativePathCompat -BasePath $SourceRoot -FullPath $_.FullName)"
    }
}