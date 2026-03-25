# Gerenciador M.I.L.

Homebrew para Nintendo Switch focado em distribuir e instalar traducoes, dublagens, mods, cheats e saves a partir de um catalogo remoto.

## Build

```powershell
cmake -S . -B build-switch2 -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=C:/devkitPro/cmake/Switch.cmake -DDEVKITPRO=C:/devkitPro
cmake --build build-switch2
```

Saida principal:

- [mil_manager.nro](C:/Users/lordd/source/codex/mil-manager/build-switch2/mil_manager.nro)

## Estrutura local

Arquivos principais na SD:

- `sdmc:/switch/mil_manager/index.json`
- `sdmc:/switch/mil_manager/installed-titles-cache.json`

Configuracao e cache:

- `sdmc:/config/mil_manager/settings.ini`
- `sdmc:/config/mil_manager/cache/index.json`
- `sdmc:/config/mil_manager/cache/images/`
- `sdmc:/config/mil_manager/cache/installed-icons/`
- `sdmc:/config/mil_manager/receipts/`

Compatibilidade legada:

- o app ainda aceita leitura de caminhos antigos em `sdmc:/config/mil-manager/`
- novas gravacoes passam a usar `mil_manager`

## settings.ini

Exemplo:

```ini
language=pt-BR
scan_mode=auto
catalog_url=https://nekkk.github.io/mil-manager-catalog/index.json
```

Idiomas validos:

- `pt-BR`
- `en-US`

Valores de `scan_mode`:

- `auto`
- `full`
- `catalog`
- `off`

## Catalogo

Fonte do catalogo:

- [catalog-metadata.json](C:/Users/lordd/source/codex/mil-manager/catalog-source/catalog-metadata.json)
- [entries](C:/Users/lordd/source/codex/mil-manager/catalog-source/entries)
- [cheats-sources.json](C:/Users/lordd/source/codex/mil-manager/catalog-source/cheats-sources.json)

Cada item vive em seu proprio arquivo em `catalog-source/entries/`.

Gerar indice, cheats e site:

```powershell
python tools\generate-index.py
python tools\generate-cheats-index.py
python tools\prepare-pages-site.py
```

Saidas:

- [dist/index.json](C:/Users/lordd/source/codex/mil-manager/dist/index.json)
- [dist/cheats-index.json](C:/Users/lordd/source/codex/mil-manager/dist/cheats-index.json)
- [dist/cheats](C:/Users/lordd/source/codex/mil-manager/dist/cheats)
- [site/index.json](C:/Users/lordd/source/codex/mil-manager/site/index.json)
- [site/cheats-index.json](C:/Users/lordd/source/codex/mil-manager/site/cheats-index.json)
- [site/cheats](C:/Users/lordd/source/codex/mil-manager/site/cheats)

O gerador tambem enriquece o indice com `intro`, `thumbnailUrl`, `iconUrl`, `coverUrl` e publica thumbs otimizados em `site/thumbs/`.

Agregador de cheats:

- schema e dedupe: [cheats-index-v1.md](C:/Users/lordd/source/codex/mil-manager/docs/cheats-index-v1.md)
- fontes primarias: `Cheat Slips`, `gbatemp mirror`, `titledb`, `Chansey`
- fallback: `ibnux`
- dedupe por `titleId + buildId + hash do conteudo`

## Painel admin

Arquivos-base:

- [site-src/admin/index.html](C:/Users/lordd/source/codex/mil-manager/site-src/admin/index.html)
- [site-src/admin/app.js](C:/Users/lordd/source/codex/mil-manager/site-src/admin/app.js)

Fluxo:

1. abrir `/admin/`
2. informar `owner`, repositorio, `branch` e token GitHub
3. carregar o catalogo
4. editar e publicar

O painel escreve em:

- `catalog-source/catalog-metadata.json`
- `catalog-source/entries/*.json`

## Sincronizacao com mil-manager-catalog

Helper:

- [sync-mil-manager-catalog.ps1](C:/Users/lordd/source/codex/mil-manager/tools/sync-mil-manager-catalog.ps1)

Exemplos:

```powershell
powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-catalog.ps1
powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-catalog.ps1 -Commit
powershell -ExecutionPolicy Bypass -File tools\sync-mil-manager-catalog.ps1 -Commit -Push
```

Esse helper copia ferramentas compartilhadas, regenera `dist/index.json` e `site/`, e opcionalmente faz `commit` e `push`.
Tambem regenera `dist/cheats-index.json` e `site/cheats/`.

## Emuladores

O homebrew rodando dentro do emulador nao enxerga automaticamente a biblioteca do host. O fluxo recomendado e sincronizar antes de abrir o app.

Utilitario base:

```powershell
python tools\mil_emulator_sync.py --emulator ryujinx
```

Sync do Ryujinx:

```powershell
powershell -ExecutionPolicy Bypass -File tools\sync-ryujinx.ps1
```

Com URL explicita:

```powershell
powershell -ExecutionPolicy Bypass -File tools\sync-ryujinx.ps1 -CatalogUrl https://nekkk.github.io/mil-manager-catalog/index.json
```

Launcher com sync:

```powershell
powershell -ExecutionPolicy Bypass -File tools\start-ryujinx-with-sync.ps1
```

Arquivos gerados na SD virtual:

- `sdmc:/switch/mil_manager/index.json`
- `sdmc:/switch/mil_manager/installed-titles-cache.json`
- `sdmc:/config/mil_manager/cache/images/`

## Compatibilidade

Campos suportados no catalogo:

- `minGameVersion`
- `maxGameVersion`
- `exactGameVersions`

Durante a instalacao, o app grava recibos locais para remocao limpa e auditoria futura.
