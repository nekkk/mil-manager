# Plano de implementacao

## Objetivo

Entregar um homebrew que rode em Nintendo Switch e emuladores compativeis o bastante para listar e instalar traducoes, mods, cheats e saves diretamente de um repositorio online, com baixo acoplamento a applets ou servicos instaveis.

## Decisoes da primeira entrega

1. Base em `libnx` com `C++17`, `libcurl` e `libarchive`.
2. UI inicial em console navegavel, para maximizar compatibilidade em console e emulador.
3. Catalogo remoto em JSON, hospedado preferencialmente no dominio `miltraducoes.com`.
4. Pacotes distribuidos em ZIP, podendo usar HTTPS direto ou link publico do `mega.nz`.
5. Instalacao para `sdmc:/` com estrutura final pronta dentro do ZIP.
6. Recibos locais por pacote para remocao e auditoria de versao.

## Camadas

### 1. Dominio

- modelo de catalogo
- compatibilidade por versao do jogo
- recibos de instalacao
- configuracao do app

### 2. Infraestrutura

- HTTP/HTTPS
- resolucao de links do MEGA
- extracao ZIP
- leitura/escrita na SD
- descoberta de titulos instalados

### 3. Aplicacao

- sugestoes para jogos instalados
- estados de install/remove
- fallback para indice local
- navegacao por secoes

### 4. Apresentacao

- renderer textual atual
- renderer grafico futuro desacoplado do nucleo

## Roadmap apos a base atual

1. Cache de titulos via `libnxtc` ou equivalente.
2. Tela de detalhes por jogo com assets e changelog.
3. Downloads em background com barra de progresso grafica.
4. Editor de fontes remotas e RSS.
5. Assinatura dos indices e hash dos ZIPs.
6. Testes em hardware via `nxlink` e matriz de compatibilidade por emulador.

## Roadmap de publicacao segura

Objetivo desta fase:

- remover `downloadUrl` real dos JSONs publicos
- usar IDs logicos para traducoes, modificacoes, trapaças e salvamentos
- ofuscar nomes locais e caminhos publicados por hash
- separar codigo do app, catalogo privado e entrega publica em repositorios distintos

### Estrutura alvo

1. `mil-manager` publico
   - codigo-fonte do app
   - releases do `mil_manager.nro`
   - documentacao tecnica
   - sem links diretos de artefatos finais nos arquivos do repositorio

2. `mil-manager-catalog` privado
   - painel administrativo
   - fontes brutas do catalogo
   - mapeamento de IDs logicos para origem real
   - URLs reais de armazenamento
   - geradores e workflows internos

3. `mil-manager-delivery` publico
   - somente artefatos sanitizados consumidos pelo app
   - indices publicos
   - thumbs publicados
   - traducoes, trapaças e salvamentos com nomes opacos por hash
   - sem qualquer URL real de origem

### Regras para nao quebrar o que ja funciona

1. O app continua entendendo o schema atual durante a migracao.
2. O catalogo privado passa a gerar os artefatos novos sem remover imediatamente os antigos.
3. O app primeiro aprende a consumir `logicalId` e `hashedPath`, mantendo fallback temporario para `downloadUrl`.
4. So depois da validacao em console, Ryujinx e Eden os `downloadUrl` reais deixam de ser publicados no JSON publico.
5. Os caminhos locais de cache continuam em `sdmc:/switch/mil_manager/cache/`.

### Fases de migracao

#### Fase 1. Delivery sanitizado em paralelo

- gerar `index.json`, `cheats-summary.json`, `cheats-index.json` e `saves-index.json` sem `downloadUrl` real
- publicar campos logicos como:
  - `assetId`
  - `assetType`
  - `relativePath`
  - `contentHash`
  - `size`
- publicar arquivos finais em caminhos opacos por hash

#### Fase 2. App dual-stack

- app passa a resolver instalacao por `assetId` e `relativePath`
- cache local usa nome por hash
- `downloadUrl` passa a ser apenas fallback de compatibilidade durante a transicao

#### Fase 3. Corte dos links reais

- `mil-manager-catalog` deixa de exportar `downloadUrl` para JSON publico
- `mil-manager-delivery` vira o unico endpoint consumido pelo app
- repositorio publico do app nao contem mais referencias diretas aos artefatos finais
- publicacao dos artefatos sanitizados passa a usar um fluxo dedicado para `mil-manager-delivery`

### Campos novos previstos

Para cada artefato publicado, o schema novo deve prever:

- `assetId`
- `assetType`
- `contentHash`
- `size`
- `revision`
- `relativePath`
- `logicalGroup`

Exemplos de `assetType`:

- `translationZip`
- `modZip`
- `cheatText`
- `saveZip`
- `thumbImage`

### Organizacao sugerida de arquivos no delivery

```text
delivery/
  index.json
  cheats-summary.json
  cheats-index.json
  saves-index.json
  thumbs/
    ab/
      ab12cd34ef....img
  assets/
    translations/
      4f/
        4f8e....zip
    mods/
      8a/
        8a51....zip
    cheats/
      13/
        13aa....txt
    saves/
      9c/
        9c20....zip
```

### Decisoes importantes

1. O nome publicado do arquivo nao precisa refletir `titleId`, `buildId` ou nome do jogo.
2. O app valida integridade por hash apos o download.
3. O catalogo privado mantem o mapeamento entre:
   - origem real
   - `assetId`
   - hash final publicado
4. O repositorio de delivery nao precisa conter o codigo do app nem o painel.

### Proximo passo recomendado

1. desenhar o schema `delivery v1`
2. adaptar o gerador privado para emitir IDs logicos e caminhos opacos
3. adaptar o app para consumir esse schema em modo dual-stack
4. validar console, Ryujinx e Eden antes de remover os links reais

## Backlog pos-migracao

### Regra de execucao

1. Cada item concluido deste backlog deve gerar pelo menos um commit dedicado.
2. Quando o item alterar comportamento visivel no app, preferir tambem uma tag de checkpoint.
3. Itens grandes podem ser quebrados em subetapas, mas cada subetapa concluida tambem deve ficar registrada em commit proprio.

### Operacao e repositorios

1. Avaliar se o `mil-manager-catalog` precisa ser atualizado para refletir novas mudancas estruturais do app e do delivery.
2. Automatizar a publicacao diferencial do delivery com janela recorrente de 3 em 3 dias.
3. Consolidar a operacao dos tres repositorios:
   - `mil-manager`
   - `mil-manager-catalog`
   - `mil-manager-delivery`
4. Preparar o fechamento da migracao para permitir tornar o `mil-manager-catalog` privado quando tudo estiver validado.

### Performance e inicializacao

1. Avaliar se a inicializacao precisa de mais otimizações quando houver grande quantidade de traducoes.
2. Avaliar se a descoberta de titulos precisa de otimizações quando o console tiver muitos jogos instalados.
3. Revisar a estrategia de caches, indices derivados e invalidações para evitar recomputacao pesada no boot.

### Internacionalizacao

1. Mover textos ainda embedados no codigo para `romfs/lang`.
2. Corrigir terminologia visivel remanescente:
   - `cheat instalado` -> `trapaca instalada`
   - `carregados por scan completo` -> `carregados por varredura completa`
3. Revisar mensagens de status, detalhes e rodape para garantir que nao restem strings fora do `lang`.

### Emuladores e nomenclatura interna

1. Remover mencoes nominais a Ryujinx ou Eden nas tools e comentarios internos.
2. Padronizar o termo generico `emuladores` em scripts, comentarios e documentacao tecnica onde o nome especifico nao for necessario.
3. Verificar se scripts e comentarios internos nao expoem caminhos, endpoints ou suposicoes desnecessarias.

### Documentacao do app

1. Atualizar `README` e docs do `mil-manager` com:
   - features atuais
   - documentacao tecnica das libraries usadas
   - creditos de codigo e repositorios usados como referencia
   - build
   - estrutura do app
   - uso das tools locais
2. Renomear referencias nas docs de nomes especificos de emuladores para `emulador` ou `emuladores`, quando fizer sentido.
3. Revisar documentacao para garantir que ela nao exponha detalhes sensiveis do fluxo privado.

### Documentacao do catalogo

1. Atualizar a documentacao do `mil-manager-catalog`.
2. Documentar o painel admin.
3. Documentar a operacao do painel admin, incluindo a atualizacao de thumbs.
4. Documentar a operacao entre repositorios:
   - sincronizacao manual e automatica
   - publicacao manual e automatica

### Empacotamento e release

1. Fazer limpeza final dos repositorios para o commit `v1.0` de lancamento oficial.
2. Disponibilizar `NRO` e forwarder `NSP` junto com os commits/releases.
3. Definir o fluxo de release para app, delivery e catalogo.

### Curadoria futura

1. Avaliar como ficara a inclusao manual de cheats.
2. Avaliar como ficara a inclusao manual de saves.
3. Definir se essas inclusoes manuais entram pelo painel admin, por arquivo fonte dedicado ou por ambos.
