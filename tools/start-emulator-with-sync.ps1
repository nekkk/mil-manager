param(
    [string]$CatalogUrl = "https://SEU_USUARIO.github.io/SEU_REPOSITORIO/index.json",
    [string]$MegaFolderUrl = "",
    [string]$EmulatorRoot = "",
    [string]$EmulatorExe = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-DefaultEmulatorRoot {
    $candidates = @(
        (Join-Path $env:APPDATA "Ryujinx"),
        (Join-Path $env:APPDATA "eden")
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "Nao foi possivel localizar a raiz padrao do emulador. Informe -EmulatorRoot explicitamente."
}

function Resolve-EmulatorExecutable {
    param(
        [Parameter(Mandatory = $true)][string]$EmulatorRootPath,
        [string]$PreferredPath = ""
    )

    $candidates = @()
    if ($PreferredPath) {
        $candidates += $PreferredPath
    }
    $candidates += @(
        (Join-Path $EmulatorRootPath "Ryujinx.exe"),
        (Join-Path $EmulatorRootPath "eden.exe"),
        (Join-Path $EmulatorRootPath "Eden.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\\Ryujinx\\Ryujinx.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\\Eden\\eden.exe"),
        "C:\\Program Files\\Ryujinx\\Ryujinx.exe",
        "C:\\Program Files (x86)\\Ryujinx\\Ryujinx.exe",
        "C:\\Program Files\\Eden\\eden.exe",
        "C:\\Program Files (x86)\\Eden\\eden.exe"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }

    foreach ($commandName in @("Ryujinx.exe", "eden.exe", "Eden.exe")) {
        $command = Get-Command $commandName -ErrorAction SilentlyContinue
        if ($command) {
            return $command.Source
        }
    }

    throw "Nao foi possivel localizar o executavel do emulador. Informe -EmulatorExe explicitamente."
}

$resolvedRoot = if ($EmulatorRoot) { $EmulatorRoot } else { Resolve-DefaultEmulatorRoot }

$syncScript = Join-Path $PSScriptRoot "sync-emulator.ps1"
if (-not (Test-Path $syncScript)) {
    throw "Nao foi possivel localizar o script de sincronizacao: $syncScript"
}

if ($MegaFolderUrl) {
    & $syncScript -MegaFolderUrl $MegaFolderUrl -EmulatorRoot $resolvedRoot
} else {
    & $syncScript -CatalogUrl $CatalogUrl -EmulatorRoot $resolvedRoot
}

$resolvedExe = Resolve-EmulatorExecutable -EmulatorRootPath $resolvedRoot -PreferredPath $EmulatorExe
Start-Process -FilePath $resolvedExe -WorkingDirectory (Split-Path -Parent $resolvedExe)

Write-Host "Emulador iniciado com sincronizacao previa."
