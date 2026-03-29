# Curadoria manual de trapaças e jogos salvos

O pipeline suporta duas camadas ao mesmo tempo:

1. agregacao automatica de fontes publicas
2. overlay manual local com prioridade maxima

Isso permite corrigir rapidamente um `buildId`, publicar uma variante de save propria ou manter um item especial sem depender do reprocessamento das fontes externas.

## Trapaças

Estrutura:

```text
catalog-source/manual-cheats/<titleId>/<buildId>.txt
catalog-source/manual-cheats/<titleId>/<buildId>.json
```

Regras:

- o `.txt` e obrigatorio
- o `.json` e opcional
- entradas manuais usam a origem `manual`
- quando o mesmo payload ja existir nas fontes agregadas, o resultado final deduplica o conteudo e registra as duas origens

Campos do sidecar JSON:

- `title`
- `categories`

## Jogos salvos

Estruturas aceitas:

```text
catalog-source/manual-saves/<titleId>/<variant>.zip
catalog-source/manual-saves/<titleId>/<variant>.json
```

ou

```text
catalog-source/manual-saves/<titleId>/<variant>/metadata.json
catalog-source/manual-saves/<titleId>/<variant>/payload/*
```

Regras:

- entradas manuais usam a origem `manual`
- metadata manual prevalece sobre a agregada quando o payload e o mesmo
- o delivery final continua sendo gerado de forma sanitizada em `dist/delivery/...`

Campos aceitos para saves:

- `name`
- `label`
- `category`
- `author`
- `language`
- `updatedAt`

## Fluxo recomendado

1. adicionar ou ajustar os arquivos em `catalog-source/manual-cheats` e `catalog-source/manual-saves`
2. rodar:

```powershell
python tools\generate-cheats-index.py
python tools\generate-saves-index.py
```

3. validar os JSONs gerados em `dist/`
4. publicar o delivery normalmente

## Objetivo

Esse overlay existe para:

- hotfix de catalogo sem esperar fonte externa
- curadoria propria da M.I.L.
- inclusao de cheats e saves que nao existem nas fontes agregadas
- padronizacao de metadados quando a fonte publica estiver inconsistente
