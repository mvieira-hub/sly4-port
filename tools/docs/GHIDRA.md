# Ghidra com o EBOOT.elf do Sly 4

## 0. Modo sem interface, via Docker (sem instalar Java)

O `tools\run_ghidra.ps1` roda o `analyzeHeadless` do Ghidra extraído em `$env:GHIDRA_HOME` (sem ela, `Documentos\ghidra_12.1.4_PUBLIC` do usuário) dentro da imagem `eclipse-temurin:21-jdk`:
- a instalação do Ghidra é montada como somente leitura;
- o ajuste do `r2` vai numa cópia do `ppc_64_32.cspec` em `work\ghidra\`, montada por cima da original.

```powershell
.\tools\run_ghidra.ps1 import                       # importa, aplica o mapa e analisa (~4 min)
.\tools\run_ghidra.ps1 export registry:0x0055902c prefix:Sly4_Register_ prefix:Sly4_Chunk_ refs:0x00AF0EB8
```

| Comando | O que faz |
|---|---|
| `import` | Cria `work\ghidra_proj\Sly4.gpr`. O projeto também abre na interface do Ghidra |
| `export` | Grava em `work\ghidra\export\` um `.c` decompilado por função e um `index.md` com quem chama e quem é chamado |

Tokens aceitos pelo `export`:

| Token | Seleciona |
|---|---|
| `prefix:NOME` | Funções cujo nome começa com NOME |
| `0xEND` | A função que contém o endereço |
| `callers:0xEND` | A função e todas as que a chamam |
| `refs:0xEND` | Quem referencia esse endereço de dado |
| `registry:0xEND` | Tabela tag → tratador (`registry.md`) e nomes `Sly4_Chunk_*` / `Sly4_Register_*` |
| `name:0xEND=Nome` | Renomeia a função; o nome fica salvo no projeto |
| `func:0xEND` | Cria uma função nesse endereço, se a análise não criou. Combine com `0xEND` para exportá-la |

Os avisos `Unable to resolve constructor` durante a análise são instruções do Cell (`lvlx`, `lvrx`…) que o Ghidra não decodifica. A função é cortada ali. Para ler o trecho, use o objdump do binutils compilado para PowerPC:

```powershell
docker build -t sly4-ppc:1 tools\ppc
docker run --rm -v "${PWD}\PS3_GAME:/game:ro" sly4-ppc:1 powerpc64-linux-objdump -d -M cell --start-address=0xA64C78 --stop-address=0xA65170 /game/USRDIR/EBOOT.elf
```

As seções abaixo são para usar a **interface gráfica**, que precisa de Java no Windows. Um JDK 21 em `.zip` extraído numa pasta e apontado em `support\launch.properties` (`JAVA_HOME_OVERRIDE`) evita instalar o Java no sistema.

## 1. Instalar

1. **Java JDK 21 (Temurin, x64, `.msi`):** https://adoptium.net/temurin/releases/?version=21
   No instalador, marque **"Set JAVA_HOME variable"**.
2. **Ghidra 12.1.4:** o arquivo já está baixado em `Downloads\ghidra_12.1.4_PUBLIC_20260921.zip`, com o SHA-256 conferido (`ddac49f9…d2d4db`).
   Extraia em uma pasta sem espaços no caminho, por exemplo `C:\ghidra`. O zip cria `C:\ghidra\ghidra_12.1.4_PUBLIC\`.
   Essa versão exige **Java 21 ou mais novo** (`application.java.min=21`).
3. Abra o `ghidraRun.bat`. Na primeira vez, ele pode pedir a pasta do JDK; aponte para `C:\Program Files\Eclipse Adoptium\jdk-21...`.

## 2. Ajuste para PS3, antes de importar

O PS3 usa o registrador `r2` como ponteiro da TOC (tabela de dados globais) e ele nunca muda entre funções. Sem o ajuste abaixo, o decompilador do Ghidra produz código confuso em volta do `r2`.

1. Feche o Ghidra.
2. Abra `C:\ghidra\ghidra_12.1.4_PUBLIC\Ghidra\Processors\PowerPC\data\languages\ppc_64_32.cspec` em um editor de texto. Faça uma cópia de segurança antes.
3. Dentro do bloco `<unaffected>` (perto da linha 96), adicione a linha:
   `<register name="r2"/>`
   Na versão 12.1.4, o `r2` não aparece em nenhum outro lugar desse arquivo, então basta acrescentar.

## 3. Importar

1. **File → New Project** (Non-Shared) e escolha uma pasta, por exemplo `Documents\GitHub\Sly Cooper Extraido\ghidra_proj`.
2. **File → Import File** e selecione `PS3_GAME\USRDIR\EBOOT.elf`.
3. Em **Language**, clique no `...`, **desmarque "Show Only Recommended"** e escolha **`PowerPC:BE:64:A2ALT-32addr`**: "Power ISA 3.0 Big Endian w/Altivec and 32-bit Addressing". É PowerPC de 64 bits com endereços de 32 bits e AltiVec/VMX, que o PPU do Cell tem. Nas versões antigas do Ghidra, essa linguagem se chamava "PowerISA-Altivec-64-32addr".
   Alternativa, se a decodificação parecer estranha: `PowerPC:BE:64:64-32addr`.
4. Quando o Ghidra perguntar se quer analisar agora, responda **"No"**.

## 4. Aplicar o mapa gerado pelo `eboot_scan.py`

1. **Window → Script Manager**. Clique no ícone de pastas ("Manage Script Directories") e adicione `Sly Cooper Extraido\tools\ghidra`.
2. Rode o **`Sly4ApplyMap.java`** (categoria *Sly4*) e, quando ele pedir, escolha a pasta `work\eboot`. O script é em Java porque o Ghidra 12 não traz mais o Jython, e scripts em Python exigiriam Python instalado no Windows. O Ghidra compila o `.java` sozinho. O script:
   - define `r2 = 0xF50800` (a TOC) em todo o código;
   - cria as 24.172 funções do `.opd` e dos alvos de `bl`;
   - dá nome aos 251 stubs de import (241 com nome real, como `cellFsOpen` e `cellSpursInitialize`);
   - nomeia as funções que registram os tratadores de bloco: `Sly4_Register_TEXR_TXRH`, `Sly4_Register_MESH`…
3. Depois, rode **Analysis → Auto Analyze** com as opções padrão. Leva alguns minutos.

O projeto gerado pelo modo Docker (`work\ghidra_proj\Sly4.gpr`) já vem com tudo isso aplicado, além dos nomes abaixo. Basta abri-lo em **File → Open Project**.

## 5. Por onde começar

### Como o motor lê os arquivos `.san`

Cada tipo de bloco tem um **tratador registrado** por `Sly4_RegisterChunkHandler(tag, &tratador, …)` (`0x0055902C`). A tag é uma constante de 32 bits em little-endian: `TEXR` vira `0x52584554`.
- As funções `Sly4_Register_*` só fazem esses registros.
- As funções `Sly4_Chunk_<TAG>` são as que de fato leem cada bloco. São 144, todas listadas em `work\ghidra\export\registry.md`.

| Tratador | Endereço | Bloco |
|---|---|---|
| `Sly4_Chunk_FORM` / `_PLAT` / `_PROF` | `0x007A50B4` / `0x007A50D4` / `0x007A5208` | Contêiner e perfil |
| `Sly4_Chunk_TEXR` / `_TXRH` | `0x0051D210` / `0x0051D4B4` | Textura e seu cabeçalho |
| `Sly4_Chunk_TGTF` | `0x005399CC` | Dados GTF |
| `Sly4_Chunk_MESH` / `_SMSH` | `0x00525E40` / `0x00524500` | Malha e submalha |

Os tratadores leem os campos com funções tipadas que **recebem o nome do campo**:

| Função | Lê |
|---|---|
| `FUN_005643c4` | u8 |
| `FUN_00564340` | u16 |
| `FUN_005642ac` | u32 |
| `FUN_00569c10` | string |

Por isso o código decompilado já documenta cada estrutura. Exemplo do `TXRH`: `ver`, `nameHash`, `width`, `height`, `depth`, `bpp`, `type`, `name`, `packBufOfst`, `packBufSize`.

### Scripts XMLB

| Função | Endereço | O que faz |
|---|---|---|
| `Sly4_Xmlb_Open` | `0x004FAF10` | Troca a extensão do nome pedido por `.xmb` e abre o arquivo. Isso indica que os originais eram `.xml` |
| `Sly4_Xmlb_LoadFromStream` | `0x004FACCC` | Lê o arquivo, confere `"XMLB"` com `memcmp` e reloca as raízes |
| `Sly4_Xmlb_RelocateElement` | `0x004FABB4` | Transforma offsets em ponteiros de forma recursiva. Confirma o layout do elemento: o campo `+4` é o texto do elemento |

A string `"XMLB"` (`0xAF0EB8`) é usada só como argumento de `memcmp`. Por isso a varredura em Python não a encontrou, e o Ghidra encontrou.

Fonte do ajuste de `r2` e da escolha da linguagem: Ps3GhidraScripts (espelho: https://p.codekk.com/detail/5ec56f48982bbb72602808ba).
