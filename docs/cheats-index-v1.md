# Cheats Index v1

O agregador de cheats do MIL gera um indice consolidado em `dist/cheats-index.json` e publica os arquivos deduplicados em `dist/cheats/`.

## Objetivo

- agregar cheats a partir de varias fontes online
- evitar duplicidade de registros
- preservar origem e categoria de cada conjunto de cheats
- publicar uma saida unica, estavel e simples de consumir no app

## Fontes

Fontes primarias:

- `cheatSlips`
- `gbatempMirror`
- `titledb`
- `chansey`

Fallback:

- `ibnux`

As prioridades de merge sao definidas em [cheats-sources.json](C:/Users/lordd/source/codex/mil-manager/catalog-source/cheats-sources.json).

## Deduplicacao

A deduplicacao usa esta chave logica:

- `titleId`
- `buildId`
- `sha256(normalizedCheatContent)`

Quando duas fontes entregam o mesmo conteudo para o mesmo `titleId` e `buildId`, o agregador mantem um unico registro, preservando:

- `sources`
- `originUrls`
- `categories`
- `priorityRank`

## Schema

Exemplo resumido:

```json
{
  "schemaVersion": "1.0",
  "generatedAt": "2026-03-23T21:44:51Z",
  "generator": "MILCheatsAggregator",
  "catalogRevision": "2026.03.21.1",
  "watchedTitleIds": [
    "01006A800016E000"
  ],
  "sources": {
    "titledb": {
      "enabled": true,
      "priorityRank": 30,
      "records": 13
    }
  },
  "titles": [
    {
      "titleId": "01006A800016E000",
      "name": "Super Smash Bros. Ultimate - Cheats",
      "builds": [
        {
          "buildId": "06646FDDD47A619F",
          "categories": ["general"],
          "entries": [
            {
              "id": "01006a800016e000-06646fddd47a619f-d5e995455eb6",
              "title": "General",
              "primarySource": "gbatempMirror",
              "sources": ["gbatempMirror"],
              "categories": ["general"],
              "contentHash": "sha256:...",
              "cheatCount": 3,
              "lineCount": 6,
              "relativePath": "cheats/01006A800016E000/06646FDDD47A619F/01006a800016e000-06646fddd47a619f-d5e995455eb6.txt",
              "downloadUrl": "https://nekkk.github.io/mil-manager-catalog/cheats/01006A800016E000/06646FDDD47A619F/01006a800016e000-06646fddd47a619f-d5e995455eb6.txt",
              "originUrls": [
                "https://raw.githubusercontent.com/exefer/gbatemp-matias3ds-cheats/master/titles/01006A800016E000/cheats/06646FDDD47A619F.txt"
              ],
              "priorityRank": 20
            }
          ]
        }
      ]
    }
  ]
}
```

## Categorias

Categorias publicadas por `entry` e por `build`:

- `general`
- `graphics`
- `fps`
- `community`

`chansey` tende a alimentar `graphics` e `fps`. As demais fontes entram como `general` ou `community`, dependendo do conteudo e da origem.

## Saidas

- indice consolidado:
  - [dist/cheats-index.json](C:/Users/lordd/source/codex/mil-manager/dist/cheats-index.json)
- arquivos deduplicados:
  - [dist/cheats](C:/Users/lordd/source/codex/mil-manager/dist/cheats)
- publicacao Pages:
  - [site/cheats-index.json](C:/Users/lordd/source/codex/mil-manager/site/cheats-index.json)
  - [site/cheats](C:/Users/lordd/source/codex/mil-manager/site/cheats)

## Pipeline

1. O catalogo define quais `titleId`s devem ser observados, principalmente pela secao `cheats`.
2. O agregador consulta as fontes configuradas.
3. Os resultados sao normalizados, categorizados e deduplicados.
4. Os `.txt` resultantes sao gravados em `dist/cheats/`.
5. O indice final e publicado em `dist/cheats-index.json`.

## Limitacoes atuais

- `cheatSlips` ainda depende de paginas sementes em `seedPages` e nao possui enumeracao global automatica.
- `gbatemp.net` nao e raspado diretamente; o caminho suportado hoje e o mirror GitHub.
