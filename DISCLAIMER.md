# Aviso legal

**Português** · [English](DISCLAIMER.en.md)

O **sly4-port** é um projeto de pesquisa independente e sem fins comerciais
sobre o software de *Sly Cooper: Thieves in Time* (PlayStation 3). O objetivo
é a **preservação de software** e a **interoperabilidade**: entender como
funciona um jogo escrito para o Cell Broadband Engine (PPU PowerPC 64 + SPUs,
kernel LV2, GPU RSX) e fazer uma cópia que alguém possui legitimamente rodar
de forma nativa em outra plataforma (Windows em x86-64).

## Nenhum material proprietário está incluído

Este repositório contém apenas trabalho original: código-fonte, scripts,
arquivos de compilação e documentação escritos para este projeto. Ele **não**
contém, e nunca conteve em seu histórico:

- o executável do jogo (`EBOOT.BIN` / `EBOOT.elf`), os programas SPU ou
  qualquer outro código do disco, seja na forma original, descriptografada,
  desmontada ou recompilada (as notas de engenharia citam no máximo algumas
  poucas instruções por vez, com endereços, para explicar uma descoberta
  específica);
- dados ou assets do jogo de qualquer tipo: pacotes, modelos, texturas, áudio,
  vídeo, scripts, shaders, textos ou imagens;
- firmware, módulos de sistema ou bibliotecas do SDK do PlayStation 3;
- chaves de criptografia, arquivos de licença, identificadores de console ou
  qualquer meio de descriptografar conteúdo protegido.

O C++ que o recompilador produz é derivado do código do jogo. Ele é gerado
localmente, a partir da cópia de cada usuário, numa pasta que o Git ignora, e
nunca é versionado nem distribuído. A integração contínua compila apenas o
runtime, sem nenhum dado do jogo.

A descriptografia do executável é feita por uma ferramenta separada e pública
(o RPCS3), na máquina do próprio usuário. Este projeto não distribui nenhum
código de descriptografia.

## Você precisa ter a sua própria cópia do jogo

As ferramentas e o runtime deste repositório não fazem nada sozinhos. Para
usá-los é preciso ter uma **cópia comprada legalmente** do jogo, no disco
original, e fazer o seu próprio dump para uso pessoal, de acordo com as leis
do seu país. Não peça, não compartilhe e não aponte links para arquivos do
jogo, firmware ou chaves em issues ou pull requests: esse conteúdo será
removido.

Este projeto não apoia a pirataria de nenhuma forma.

## Marcas registradas e vínculos

*Sly Cooper* e *Sly Cooper: Thieves in Time* são marcas registradas da Sony
Interactive Entertainment LLC. O jogo foi desenvolvido pela Sanzaru Games.
*PlayStation*, *PS3* e *Cell Broadband Engine* são marcas de seus respectivos
donos. Todas as marcas pertencem aos seus donos e aparecem aqui apenas para
identificar o software estudado.

Este projeto **não tem ligação, aval nem patrocínio** da Sony Interactive
Entertainment, da Sanzaru Games ou de qualquer parceiro delas.

## Referências de terceiros

Algumas técnicas seguem o código-fonte e a documentação públicos do emulador
RPCS3 e do GNU binutils, que recebem o devido crédito onde são usados. Nomes
de bibliotecas e formatos (por exemplo PlayStation EDGE, Havok, Bink, SPURS)
aparecem apenas para descrever como os dados e o código do jogo estão
organizados.

## Sem garantia, e um contato para questionamentos

O software é fornecido "no estado em que se encontra", sem garantia de
nenhum tipo (veja a [LICENSE](LICENSE)). Este aviso descreve a intenção e as
práticas do projeto; não é aconselhamento jurídico.

Se você é titular de direitos e acredita que algo neste repositório não
deveria estar aqui, abra uma issue ou fale com o autor pelo GitHub, e o
conteúdo será analisado e removido prontamente.
