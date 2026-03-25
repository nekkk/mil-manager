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
