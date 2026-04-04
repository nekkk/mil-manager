# Code Maintenance Guide

## Objetivo

Este guia existe para orientar ajustes manuais no app sem precisar redescobrir a estrutura do projeto toda vez.

Use este documento como mapa rapido:

- "quero mudar um texto"
- "quero alterar a UI de uma secao"
- "quero mexer na instalacao"
- "quero entender de onde vem o catalogo"
- "quero ajustar comportamento em emuladores"

## Arquivos centrais

- [main.cpp](C:/Users/lordd/source/codex/mil-manager/src/main.cpp)
  - ponto de entrada do homebrew
- [app.cpp](C:/Users/lordd/source/codex/mil-manager/src/app.cpp)
  - loop principal, estado da UI, navegacao, filtros, busca, listas, dialogs e detalhes
- [installer.cpp](C:/Users/lordd/source/codex/mil-manager/src/installer.cpp)
  - download, staging, extracao, instalacao, desinstalacao, backups e recibos
- [platform_switch.cpp](C:/Users/lordd/source/codex/mil-manager/src/platform_switch.cpp)
  - deteccao de ambiente, biblioteca instalada, build IDs, leitura de manifestos e comportamento de emuladores
- [catalog.cpp](C:/Users/lordd/source/codex/mil-manager/src/catalog.cpp)
  - parsing do catalogo principal
- [cheats.cpp](C:/Users/lordd/source/codex/mil-manager/src/cheats.cpp)
  - parsing dos indices de trapaças
- [savegames.cpp](C:/Users/lordd/source/codex/mil-manager/src/savegames.cpp)
  - parsing dos indices de jogos salvos
- [config.cpp](C:/Users/lordd/source/codex/mil-manager/src/config.cpp)
  - leitura e persistencia de settings
- [http.cpp](C:/Users/lordd/source/codex/mil-manager/src/http.cpp)
  - downloads HTTP e callback de progresso
- [models.hpp](C:/Users/lordd/source/codex/mil-manager/include/mil/models.hpp)
  - structs principais compartilhadas entre modulos

## Se voce quiser mudar...

### Textos e idiomas

Arquivos principais:

- [pt-BR.json](C:/Users/lordd/source/codex/mil-manager/romfs/lang/pt-BR.json)
- [en-US.json](C:/Users/lordd/source/codex/mil-manager/romfs/lang/en-US.json)
- [app.cpp](C:/Users/lordd/source/codex/mil-manager/src/app.cpp)

Regra pratica:

- primeiro procure a chave no `lang`
- so mexa em `app.cpp` se a chave estiver errada, faltando, ou se o codigo estiver pedindo a chave errada

Pontos importantes:

- a maioria dos textos da UI deve vir de `romfs/lang`
- quando aparecer `[[...]]`, normalmente o problema nao e traducao em si, e sim uma chave errada no codigo
- depois de mudar `lang`, recompile para embutir os JSONs novos no NRO

### Layout, listas e telas

Arquivo principal:

- [app.cpp](C:/Users/lordd/source/codex/mil-manager/src/app.cpp)

Aqui voce encontra:

- menu lateral
- cards da lista central
- painel de detalhes
- popup de variantes
- tela "Sobre a M.I.L."
- barra inferior de status
- overlay de progresso

Quando mexer em UI, normalmente o fluxo e:

1. localizar a secao em `app.cpp`
2. ajustar desenho e texto
3. validar se a navegacao e o estado continuam coerentes

### Busca, ordenacao e desempenho das listas

Arquivo principal:

- [app.cpp](C:/Users/lordd/source/codex/mil-manager/src/app.cpp)

Pontos importantes:

- a lista visivel e cacheada
- cheats e saves usam indice de busca pre-normalizado
- ordenacao e flags como `sugerido` e `detectado` sao precomputadas

Se algo ficar lento de novo, comece procurando por:

- construcao da lista visivel
- invalidacao de cache
- filtros por secao
- busca por termo

### Catalogo, cheats e saves

Arquivos:

- [catalog.cpp](C:/Users/lordd/source/codex/mil-manager/src/catalog.cpp)
- [cheats.cpp](C:/Users/lordd/source/codex/mil-manager/src/cheats.cpp)
- [savegames.cpp](C:/Users/lordd/source/codex/mil-manager/src/savegames.cpp)
- [models.hpp](C:/Users/lordd/source/codex/mil-manager/include/mil/models.hpp)

Responsabilidades:

- `catalog.cpp`: traducoes, dublagens e modificacoes
- `cheats.cpp`: indices e summaries de trapaças
- `savegames.cpp`: indices agregados de saves
- `models.hpp`: schema que o app espera consumir

Se um item aparece quebrado na UI, confirme nesta ordem:

1. schema esperado em `models.hpp`
2. parser do modulo correspondente
3. uso final em `app.cpp`

### Instalacao e remocao

No modelo atual do catalogo:

- `thumbs`, `cheats` e `saves` continuam vindo do `mil-manager-delivery`
- `traducoes`, `mods` e `dublagens` podem vir com `downloadUrl` ofuscada no catalogo principal
- essa URL e resolvida pelo app antes do download

Arquivo principal:

- [installer.cpp](C:/Users/lordd/source/codex/mil-manager/src/installer.cpp)

Aqui ficam:

- cache local ofuscado por hash
- staging
- extração ZIP
- instalacao de pacotes
- instalacao de trapaças
- instalacao e restauracao de saves
- recibos em `sdmc:/switch/mil_manager/receipts`

Se voce quiser mudar comportamento de install/uninstall, esse e o arquivo certo.

### Jogos salvos

Arquivo principal:

- [installer.cpp](C:/Users/lordd/source/codex/mil-manager/src/installer.cpp)

O fluxo atual e:

1. baixar variante
2. extrair em staging
3. abrir o save do titulo
4. fazer backup do estado atual
5. aplicar payload
6. salvar recibo

Na remocao:

1. localizar recibo
2. abrir save do titulo
3. restaurar backup
4. remover recibo

Pontos sensiveis:

- diferencas entre console e emuladores
- save inexistente em emuladores
- compatibilidade com layout de save exposto pelo filesystem

### Emuladores, jogos instalados e Build ID

Arquivo principal:

- [platform_switch.cpp](C:/Users/lordd/source/codex/mil-manager/src/platform_switch.cpp)

Aqui voce ajusta:

- deteccao de ambiente
- comportamento com loader
- leitura de manifestos importados
- scan da biblioteca
- obtencao de versao instalada
- resolucao de build ID

Se surgir algum caso especifico de ambiente ou loader, normalmente o ajuste entra aqui, nao em `app.cpp`.

### Configuracao e defaults

Arquivos:

- [config.cpp](C:/Users/lordd/source/codex/mil-manager/src/config.cpp)
- [config.hpp](C:/Users/lordd/source/codex/mil-manager/include/mil/config.hpp)

Voce encontra:

- URL default do catalogo
- paths do app
- leitura/escrita de settings
- constantes globais de operacao

## Fluxos que mais valem entender

### Boot do app

1. [main.cpp](C:/Users/lordd/source/codex/mil-manager/src/main.cpp) inicia a sessao
2. [app.cpp](C:/Users/lordd/source/codex/mil-manager/src/app.cpp) carrega config
3. [platform_switch.cpp](C:/Users/lordd/source/codex/mil-manager/src/platform_switch.cpp) detecta ambiente e biblioteca
4. [catalog.cpp](C:/Users/lordd/source/codex/mil-manager/src/catalog.cpp), [cheats.cpp](C:/Users/lordd/source/codex/mil-manager/src/cheats.cpp) e [savegames.cpp](C:/Users/lordd/source/codex/mil-manager/src/savegames.cpp) carregam os indices necessarios
5. [app.cpp](C:/Users/lordd/source/codex/mil-manager/src/app.cpp) monta listas e desenha a UI

### Atualizacao de catalogo

1. [http.cpp](C:/Users/lordd/source/codex/mil-manager/src/http.cpp) baixa os arquivos
2. [app.cpp](C:/Users/lordd/source/codex/mil-manager/src/app.cpp) atualiza cache e estado
3. parsers reconstroem os modelos em memoria
4. a lista visivel e invalidada e reconstruida

Observacao:

- no catalogo principal, `downloadUrl` pode chegar ofuscada e ser decodificada no app
- em `cheats`, `saves` e thumbs o fluxo continua baseado em `deliveryBaseUrl + relativePath`

### Instalacao

1. [app.cpp](C:/Users/lordd/source/codex/mil-manager/src/app.cpp) resolve item/variante
2. [installer.cpp](C:/Users/lordd/source/codex/mil-manager/src/installer.cpp) baixa o asset
3. o payload e aplicado no destino certo
4. o recibo e gravado
5. a UI atualiza o estado local

## Onde tomar cuidado

### `app.cpp`

E o arquivo mais sensivel do projeto.

Riscos comuns:

- quebrar navegacao entre secoes
- invalidar cache demais e deixar a UI lenta
- chamar strings erradas do `lang`
- misturar logica de plataforma com logica de UI

### `installer.cpp`

Riscos comuns:

- remover arquivo/diretorio errado
- deixar restore de save inconsistente
- quebrar compatibilidade entre console e emuladores
- alterar paths de cache ou recibo sem atualizar leitura posterior

### `platform_switch.cpp`

Riscos comuns:

- chamar servicos indisponiveis em determinado ambiente
- tratar um emulador como console real em fluxos sensiveis
- bloquear um caso legitimo por heuristica de loader

## Checklist rapido para ajuste manual

Antes de mexer:

1. identifique o modulo certo
2. veja se a mudanca e de UI, parser, plataforma ou instalacao
3. confira se existe string no `lang`

Depois de mexer:

1. build local
2. teste no ambiente afetado
3. teste navegacao basica
4. teste install/uninstall se a mudanca tocar recibos, cache ou saves

## Guia de busca rapida

Se estiver procurando...

- texto quebrado: [pt-BR.json](C:/Users/lordd/source/codex/mil-manager/romfs/lang/pt-BR.json), [en-US.json](C:/Users/lordd/source/codex/mil-manager/romfs/lang/en-US.json), [app.cpp](C:/Users/lordd/source/codex/mil-manager/src/app.cpp)
- card/lista/detalhe: [app.cpp](C:/Users/lordd/source/codex/mil-manager/src/app.cpp)
- parser de catalogo: [catalog.cpp](C:/Users/lordd/source/codex/mil-manager/src/catalog.cpp)
- parser de trapaças: [cheats.cpp](C:/Users/lordd/source/codex/mil-manager/src/cheats.cpp)
- parser de saves: [savegames.cpp](C:/Users/lordd/source/codex/mil-manager/src/savegames.cpp)
- install/uninstall: [installer.cpp](C:/Users/lordd/source/codex/mil-manager/src/installer.cpp)
- ambiente/build ID/biblioteca: [platform_switch.cpp](C:/Users/lordd/source/codex/mil-manager/src/platform_switch.cpp)
- URL default/path/config: [config.cpp](C:/Users/lordd/source/codex/mil-manager/src/config.cpp), [config.hpp](C:/Users/lordd/source/codex/mil-manager/include/mil/config.hpp)

## Proximo nivel de documentacao

Se depois voce quiser aprofundar mais, os melhores candidatos para documentacao interna adicional sao:

- mapa das principais structs em [models.hpp](C:/Users/lordd/source/codex/mil-manager/include/mil/models.hpp)
- comentario de alto nivel nos blocos mais longos de [app.cpp](C:/Users/lordd/source/codex/mil-manager/src/app.cpp)
- comentario dos fluxos de save em [installer.cpp](C:/Users/lordd/source/codex/mil-manager/src/installer.cpp)
