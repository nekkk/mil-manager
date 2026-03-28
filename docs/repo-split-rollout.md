# Rollout da Separação de Repositórios

## Objetivo

Separar o projeto em:

1. `mil-manager`
   - público
   - código-fonte do app
   - releases do `mil_manager.nro`

2. `mil-manager-catalog`
   - privado
   - fontes do catálogo
   - painel administrativo
   - geradores e workflows internos

3. `mil-manager-delivery`
   - público
   - somente artefatos sanitizados consumidos pelo app

## Estado esperado após a etapa 3

- o app entende `assetId`, `assetType`, `relativePath`, `contentHash` e `deliveryBaseUrl`
- `index.json`, `cheats-index.json`, `cheats-summary.json` e `saves-index.json` públicos não carregam mais `downloadUrl`
- o cache local do app usa nomes opacos derivados de `assetId` ou `contentHash`
- o conteúdo publicado em `site/` já representa o futuro `mil-manager-delivery`

## Ordem recomendada

### 1. Consolidar `mil-manager`

Usar este repositório como fonte pública do app:

- manter `src/`, `include/`, `romfs/`, `docs/`, `tools/`
- manter os geradores compartilhados enquanto a separação ainda estiver em transição
- publicar releases do NRO só por aqui

### 2. Preparar `mil-manager-catalog`

Criar ou converter o repositório privado com:

- `catalog-source/`
- `site-src/admin/`
- `tools/generate-index.py`
- `tools/generate-cheats-index.py`
- `tools/generate-saves-index.py`
- `tools/prepare-pages-site.py`

Fluxo recomendado:

1. sincronizar a partir do `mil-manager`
2. gerar `dist/` e `site/`
3. publicar artefatos sanitizados no `mil-manager-delivery`

### 3. Preparar `mil-manager-delivery`

Criar repositório público vazio e publicar apenas a saída de `site/`.

Estrutura esperada:

- `index.json`
- `cheats-index.json`
- `cheats-summary.json`
- `saves-index.json`
- `thumbs/`
- `delivery/`

## Fluxo operacional local

### Catálogo privado

No checkout local do `mil-manager-catalog`:

```powershell
python tools\generate-index.py
python tools\generate-cheats-index.py
python tools\generate-saves-index.py
python tools\prepare-pages-site.py
```

### Publicação do delivery

No checkout local do `mil-manager`:

```powershell
powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1
powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1 -Commit
powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-delivery.ps1 -Commit -Push
```

## Migração prática sugerida

### Fase A. Sem mudar remotos ainda

- validar localmente `site/`
- validar o app apontando para o delivery sanitizado local

### Fase B. Criar `mil-manager-delivery`

- criar o repositório público
- clonar localmente ao lado dos demais
- publicar `site/` nele com o helper novo

### Fase C. Tornar `mil-manager-catalog` privado

- garantir que o app já não depende mais dele como endpoint público
- mover a publicação pública para `mil-manager-delivery`
- só então ajustar a visibilidade do catálogo

### Fase D. Ajustar o app para o endpoint final

- `catalog_url` padrão passa a apontar para o `mil-manager-delivery`
- manter compatibilidade com caches antigos

## Checklist de validação

1. O app instala tradução a partir de `relativePath`
2. O app instala trapaça a partir de `relativePath`
3. O app instala save a partir de `relativePath`
4. Nenhum JSON público contém `downloadUrl`
5. O cache local não usa nomes legíveis de pacote
6. O delivery público não contém código-fonte nem painel admin
