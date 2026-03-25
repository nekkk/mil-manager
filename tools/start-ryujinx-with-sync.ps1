param(
    [string]$CatalogUrl = "https://SEU_USUARIO.github.io/SEU_REPOSITORIO/index.json",
    [string]$MegaFolderUrl = "",
    [string]$RyujinxRoot = "$env:APPDATA\Ryujinx",
    [string]$RyujinxExe = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RyujinxExecutable {
    param(
        [Parameter(Mandatory = $true)][string]$RyujinxRootPath,
        [string]$PreferredPath = ""
    )

    $candidates = @()
    if ($PreferredPath) {
        $candidates += $PreferredPath
    }
    $candidates += @(
        (Join-Path $RyujinxRootPath "Ryujinx.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\\Ryujinx\\Ryujinx.exe"),
        "C:\\Program Files\\Ryujinx\\Ryujinx.exe",
        "C:\\Program Files (x86)\\Ryujinx\\Ryujinx.exe"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }

    $command = Get-Command "Ryujinx.exe" -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw "Nao foi possivel localizar o Ryujinx.exe. Informe -RyujinxExe explicitamente."
}

$syncScript = Join-Path $PSScriptRoot "sync-ryujinx.ps1"
if (-not (Test-Path $syncScript)) {
    throw "Nao foi possivel localizar o script de sincronizacao: $syncScript"
}

if ($MegaFolderUrl) {
    & $syncScript -MegaFolderUrl $MegaFolderUrl -RyujinxRoot $RyujinxRoot
} else {
    & $syncScript -CatalogUrl $CatalogUrl -RyujinxRoot $RyujinxRoot
}

$resolvedExe = Resolve-RyujinxExecutable -RyujinxRootPath $RyujinxRoot -PreferredPath $RyujinxExe
Start-Process -FilePath $resolvedExe -WorkingDirectory (Split-Path -Parent $resolvedExe)

Write-Host "Ryujinx iniciado com sincronizacao previa."
