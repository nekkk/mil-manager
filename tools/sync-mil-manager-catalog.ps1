param(
    [string]$CatalogRepoPath = (Join-Path $PSScriptRoot "..\..\mil-manager-catalog"),
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

function Copy-RequiredFile {
    param(
        [string]$SourcePath,
        [string]$TargetPath
    )

    Assert-PathExists -Path $SourcePath -Description "Arquivo de origem"
    $targetDir = Split-Path -Path $TargetPath -Parent
    if ($targetDir) {
        New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
    }
    Copy-Item -Path $SourcePath -Destination $TargetPath -Force
}

$resolvedSourceRepo = (Resolve-Path $SourceRepoPath).Path
$resolvedCatalogRepo = (Resolve-Path $CatalogRepoPath).Path

Assert-PathExists -Path (Join-Path $resolvedCatalogRepo ".git") -Description "Repositório do catálogo"

$filesToCopy = @(
    @{
        Source = (Join-Path $resolvedSourceRepo "tools\generate-index.py")
        Target = (Join-Path $resolvedCatalogRepo "tools\generate-index.py")
    },
    @{
        Source = (Join-Path $resolvedSourceRepo "tools\generate-cheats-index.py")
        Target = (Join-Path $resolvedCatalogRepo "tools\generate-cheats-index.py")
    },
    @{
        Source = (Join-Path $resolvedSourceRepo "tools\prepare-pages-site.py")
        Target = (Join-Path $resolvedCatalogRepo "tools\prepare-pages-site.py")
    },
    @{
        Source = (Join-Path $resolvedSourceRepo "tools\requirements-catalog.txt")
        Target = (Join-Path $resolvedCatalogRepo "requirements.txt")
    },
    @{
        Source = (Join-Path $resolvedSourceRepo "site-src\admin\index.html")
        Target = (Join-Path $resolvedCatalogRepo "site-src\admin\index.html")
    },
    @{
        Source = (Join-Path $resolvedSourceRepo "site-src\admin\app.js")
        Target = (Join-Path $resolvedCatalogRepo "site-src\admin\app.js")
    },
    @{
        Source = (Join-Path $resolvedSourceRepo "catalog-source\cheats-sources.json")
        Target = (Join-Path $resolvedCatalogRepo "catalog-source\cheats-sources.json")
    }
)

Write-Step "Sincronizando arquivos compartilhados para o repositório do catálogo"
foreach ($file in $filesToCopy) {
    Copy-RequiredFile -SourcePath $file.Source -TargetPath $file.Target
    Write-Host ("  copiado: " + (Resolve-Path $file.Target).Path.Replace($resolvedCatalogRepo + "\", "")) -ForegroundColor DarkGray
}

Write-Step "Regenerando índice e site do catálogo"
Push-Location $resolvedCatalogRepo
try {
    & python ".\tools\generate-index.py"
    if ($LASTEXITCODE -ne 0) {
        throw "Falha ao gerar o índice do catálogo."
    }

    & python ".\tools\generate-cheats-index.py"
    if ($LASTEXITCODE -ne 0) {
        throw "Falha ao gerar o Ã­ndice de cheats."
    }

    & python ".\tools\prepare-pages-site.py"
    if ($LASTEXITCODE -ne 0) {
        throw "Falha ao preparar o site do catálogo."
    }

    $cachePath = Join-Path $resolvedCatalogRepo ".cache"
    if (Test-Path $cachePath) {
        Remove-Item $cachePath -Recurse -Force -ErrorAction SilentlyContinue
    }

    Write-Step "Status atual do repositório do catálogo"
    & git status --short
    if ($LASTEXITCODE -ne 0) {
        throw "Falha ao consultar git status."
    }

    if ($Commit -or $Push) {
        $finalCommitMessage = $CommitMessage
        if ([string]::IsNullOrWhiteSpace($finalCommitMessage)) {
            $finalCommitMessage = "Sync catalog tooling and admin"
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
Write-Host "Sincronização concluída." -ForegroundColor Green
Write-Host "Repositório do catálogo: $resolvedCatalogRepo"
Write-Host ""
Write-Host "Uso rápido:" -ForegroundColor Cyan
Write-Host "  powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-catalog.ps1"
Write-Host "  powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-catalog.ps1 -Commit"
Write-Host "  powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-catalog.ps1 -Commit -Push"
