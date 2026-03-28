param(
    [string]$DeliveryRepoPath = (Join-Path $PSScriptRoot "..\..\mil-manager-delivery"),
    [string]$SourceRepoPath = (Join-Path $PSScriptRoot ".."),
    [string]$CommitMessage = "",
    [switch]$Commit,
    [switch]$Push
)

$ErrorActionPreference = "Stop"

function Write-Step {
    param([string]$Message)
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Assert-PathExists {
    param(
        [string]$Path,
        [string]$Description
    )

    if (-not (Test-Path $Path)) {
        throw "$Description não encontrado: $Path"
    }
}

function Clear-DirectoryContents {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
        return
    }

    Get-ChildItem -LiteralPath $Path -Force | Where-Object { $_.Name -ne ".git" } | Remove-Item -Recurse -Force
}

$resolvedSourceRepo = (Resolve-Path $SourceRepoPath).Path
$resolvedDeliveryRepo = (Resolve-Path $DeliveryRepoPath).Path
$sitePath = Join-Path $resolvedSourceRepo "site"

Assert-PathExists -Path (Join-Path $resolvedDeliveryRepo ".git") -Description "Repositório do delivery"
Assert-PathExists -Path $sitePath -Description "Saída sanitizada do site"

Write-Step "Sincronizando site sanitizado para o repositório mil-manager-delivery"
Clear-DirectoryContents -Path $resolvedDeliveryRepo
Copy-Item -Path (Join-Path $sitePath "*") -Destination $resolvedDeliveryRepo -Recurse -Force

Push-Location $resolvedDeliveryRepo
try {
    Write-Step "Status atual do repositório do delivery"
    & git status --short
    if ($LASTEXITCODE -ne 0) {
        throw "Falha ao consultar git status."
    }

    if ($Commit -or $Push) {
        $finalCommitMessage = $CommitMessage
        if ([string]::IsNullOrWhiteSpace($finalCommitMessage)) {
            $finalCommitMessage = "Publish sanitized delivery artifacts"
        }

        Write-Step "Adicionando alterações ao git"
        & git add .
        if ($LASTEXITCODE -ne 0) {
            throw "Falha ao executar git add."
        }

        $pendingChanges = (& git status --short)
        if ($LASTEXITCODE -ne 0) {
            throw "Falha ao verificar alterações pendentes."
        }

        if (-not [string]::IsNullOrWhiteSpace(($pendingChanges | Out-String))) {
            Write-Step "Criando commit"
            & git commit -m $finalCommitMessage
            if ($LASTEXITCODE -ne 0) {
                throw "Falha ao criar commit."
            }
        } else {
            Write-Host "Nenhuma alteração nova para commit." -ForegroundColor Yellow
        }
    }

    if ($Push) {
        Write-Step "Sincronizando com origin/main antes do push"
        & git fetch origin main
        if ($LASTEXITCODE -ne 0) {
            throw "Falha ao executar git fetch."
        }

        & git rebase origin/main
        if ($LASTEXITCODE -ne 0) {
            throw "Falha ao executar git rebase origin/main."
        }

        Write-Step "Enviando para origin/main"
        & git push origin main
        if ($LASTEXITCODE -ne 0) {
            throw "Falha ao executar git push."
        }
    }
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "Publicação do delivery concluída." -ForegroundColor Green
Write-Host "Repositório do delivery: $resolvedDeliveryRepo"
Write-Host ""
Write-Host "Uso rápido:" -ForegroundColor Cyan
Write-Host "  powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1"
Write-Host "  powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1 -Commit"
Write-Host "  powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1 -Commit -Push"
