# Features

## Conteudo suportado

- traducoes
- dublagens
- modificacoes
- trapacas
- jogos salvos

## O que o app faz

- carrega catalogo remoto sanitizado
- filtra conteudo por titulo detectado quando faz sentido
- permite pesquisa manual em catalogos grandes
- baixa e instala itens individualmente
- mantem recibos locais para remocao e auditoria
- usa cache local para reduzir downloads repetidos

## Comportamentos importantes

- traducoes e modificacoes usam instalacao por pacote
- trapacas usam summary leve para listagem/pesquisa e arquivo individual por build na instalacao
- jogos salvos usam match por `titleId` e variantes por versao quando disponiveis
- em emuladores, algumas operacoes dependem de sincronizacao previa do host

## Suporte de ambiente

- Nintendo Switch real
- emuladores suportados por sincronizacao de biblioteca

## Metas de UX ja implementadas

- progresso real de download
- progresso real em etapas de extracao/aplicacao onde disponivel
- carregamento mais leve de catalogos grandes
- internacionalizacao via `romfs/lang/*.json`
- separacao entre catalogo privado e delivery publico sanitizado
