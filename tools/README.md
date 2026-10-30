# Ferramentas: Sly Cooper: Thieves in Time (BCES01284)

Todos os scripts rodam em Docker (imagem `sly4-tools:1`, Python 3.11 + capstone + Pillow).
O `run.ps1` monta as pastas assim:

| No contêiner | No Windows | Acesso |
|---|---|---|
| `/game` | `PS3_GAME` | somente leitura |
| `/tools` | `tools` | somente leitura |
| `/work` | `work` (saídas) | leitura e escrita |

```powershell
.\tools\run.ps1 <script.py> [argumentos]
```

## 1. Extrair o psarc: `psarc.py`

```powershell
.\tools\run.ps1 psarc.py list --filter '*.xmb'
.\tools\run.ps1 psarc.py extract --filter '/levels/*' --filter '/scripts/*'
```

Grava em `work/psarc/` com o mesmo caminho interno. Arquivos já extraídos (mesmo tamanho) são pulados.
Sem `--filter`, extrai tudo (~20 GB descompactados, dos quais 11 GB são áudio `.xvag`).

Inventário do pacote inteiro por pasta e extensão, com os bytes mágicos de cada tipo:

```powershell
.\tools\run.ps1 psarc_inventory.py /game/USRDIR/Sly4.psarc
```

## 2. Texturas TEXR para DDS: `texr2dds.py`

```powershell
.\tools\run.ps1 texr2dds.py /work/psarc/Levels/Arabia --png
.\tools\run.ps1 texr2dds.py /work/psarc/GUI --info
```

- Percorre os blocos (`FORM` > `PROF` > `TEXR` > `TXRH` + `TGTF`), lê o GTF e grava um `.dds` por textura, com o nome original (`TXRH`). O PNG de prévia é opcional (`--png`).
- **Cobertura do jogo inteiro:** 49.981 texturas, todas DXT1 ou DXT5. A conversão é sem perda: só troca o cabeçalho GTF pelo DDS.
- Os caminhos de formatos sem compressão (unswizzle Morton e troca de bytes para A8R8G8B8, R5G6B5 etc.) estão implementados, mas **não foram testados**, porque o jogo não usa esses formatos. Formatos desconhecidos são salvos como `.gtf_raw`.

## 3. Scripts XMLB para XML/JSON: `xmlb2json.py`

```powershell
.\tools\run.ps1 xmlb2json.py /work/psarc/Scripts --format xml --names-map /work/names.tsv --unknown /work/unknown.tsv
```

Formato descoberto (ver o docstring do script):
- Árvore de elementos de 24 bytes com nome em hash, pai, filhos e atributos.
- Atributos `{hash do nome, offset do valor}`.
- Strings `{hash, texto}`.

**O hash é `h = h*31 + c` sobre o texto em maiúsculas** (o `String.hashCode` do Java). Os nomes são recuperados por dicionário:
- palavras-semente;
- tokens de todas as strings do jogo;
- pares de tokens (`DebugPrint`, `SetMusic`…);
- `--names` com listas extras, por exemplo o `work/eboot/strings.txt` gerado pelo `eboot_scan.py`.

Com as strings do EBOOT (`--names /work/eboot/strings.txt`) e os nomes do formato de planilha XML do Excel (alguns `.xmb`, como `MixStates`, eram planilhas), **85% dos nomes estão resolvidos**. Os que faltam aparecem como `h_XXXXXXXX`.

O layout foi confirmado no carregador do jogo (`Sly4_Xmlb_LoadFromStream`, `0x004FACCC`):
- o campo `+4` do elemento é o texto do elemento (5.253 elementos têm texto);
- em `0x14` fica uma lista de offsets das raízes.

Os valores não trazem o tipo, então são exibidos assim:

| Prefixo | Significado |
|---|---|
| (nenhum) | string, quando o hash confere com o texto |
| `u:` | inteiros de 32 bits |
| `f:` | floats |
| `x:` | bytes brutos em hexadecimal |

Valores pequenos podem ser ambíguos: `u:0` pode ser um float 0.0 ou um falso.

## 4. Mapa do executável: `eboot_scan.py`

**Passo 1: decifrar.** Pela linha de comando do RPCS3 (gera o `EBOOT.elf` ao lado do `.BIN`):
```powershell
& "C:\caminho\do\rpcs3\rpcs3.exe" --decrypt "PS3_GAME\USRDIR\EBOOT.BIN"
```
Também dá para usar o menu **Utilities → Decrypt PS3 Binaries**. Se o arquivo ficar em outro lugar, use `--elf`.

**Passo 2: rodar.**
```powershell
.\tools\run.ps1 eboot_scan.py
# opcional: nomes das syscalls a partir do lv2.cpp do RPCS3 copiado para work/
.\tools\run.ps1 eboot_scan.py --syscall-names /work/lv2.cpp
```

Saídas em `work/eboot/`:

| Arquivo | Conteúdo |
|---|---|
| `summary.txt` | Seções, valor da TOC (r2) e contagens |
| `functions.csv` | Entradas do `.opd` e alvos de `bl` |
| `imports.csv` | Módulo, NID e endereço do stub de cada função importada |
| `syscalls.csv` | Cada `sc`, com o número carregado em `r11` e a função onde está |
| `tag_xrefs.md` | Referências a `TEXR`, `MESH`, `MATL`, `SBKF`, `GINS`… com desmontagem |
| `strings.txt` | Strings das seções de dados (alimenta o `xmlb2json.py --names`) |

As referências às tags são encontradas de três formas:
- o 4CC montado como imediato (`lis` + `ori`/`addi`, nas duas ordens de bytes);
- o endereço de uma string, montado por `lis` + `addi` absoluto ou `addi rX, r2, d`;
- um slot da TOC lido com `lwz`/`ld d(r2)` que aponta para a string.

Resultado no EBOOT real:
- 24.172 funções;
- 251 imports de 20 módulos, dos quais **241 com nome**. Os nomes vêm de `data/ps3_nid_candidates.txt` e só entram quando o hash SHA-1 do NID confere;
- 376 instruções `sc`;
- uma função de carga encontrada para cada tag do `.san`. As tags aparecem como constantes de 32 bits em little-endian.

O ELF que o RPCS3 gera não tem nomes de seção, então `.opd` e `.lib.stub` são identificados pelo conteúdo.

As strings do EBOOT (`work/eboot/strings.txt`) passadas ao `xmlb2json.py --names` elevam os nomes resolvidos no XMLB de 70% para **81%**.

## 4b. Ghidra sem interface, via Docker: `run_ghidra.ps1`

```powershell
.\tools\run_ghidra.ps1 import
.\tools\run_ghidra.ps1 export registry:0x0055902c prefix:Sly4_Chunk_
```

Usa o Ghidra extraído em `Documents\ghidra_12.1.4_PUBLIC` com o JDK 21 do contêiner, sem instalar Java no Windows.
- O `import` aplica o mapa do `eboot_scan.py` e analisa o executável em cerca de 4 minutos.
- O `export` gera o código C decompilado e a tabela dos **144 tratadores de bloco** (`work\ghidra\export\registry.md`).

Detalhes e o uso pela interface gráfica estão em [docs/GHIDRA.md](docs/GHIDRA.md).

## 4c. Desmontagem PPU com instruções do Cell: `tools/ppc`

O Ghidra e o capstone não decodificam `lvlx`/`lvrx`/`stvlx`/`stvrx`. A imagem `sly4-ppc:1` traz o objdump do binutils 2.35 para PowerPC, que entende essas instruções com `-M cell`.

```powershell
docker build -t sly4-ppc:1 tools\ppc
docker run --rm -v "${PWD}\PS3_GAME:/game:ro" -v "${PWD}\tools\ppc:/s:ro" sly4-ppc:1 sh /s/opcode_stats.sh
```

O `opcode_stats.sh` gera o histograma de mnemônicos do código PPU: 2,8 milhões de instruções, 315 mnemônicos, 103 cobrem 99%. Esses números dimensionam o recompilador ([docs/ROADMAP.md](../docs/ROADMAP.md)).

## 5. Shaders: `cgbin_info.py`

```powershell
.\tools\run.ps1 cgbin_info.py
```

Inventário dos 541 `.vpo`/`.fpo` em `work/shaders/`: `summary.csv`, um `.json` com os parâmetros de cada shader e o `.ucode` bruto.
A estratégia para HLSL e SPIR-V está em [docs/SHADERS.md](docs/SHADERS.md).

## 6. Malhas e fases para glTF/OBJ: `san2gltf.py`

```powershell
.\tools\run.ps1 san2gltf.py /work/psarc/Levels/Paris/Hideout/All_Combined.san.cooked --obj
.\tools\run.ps1 san2gltf.py /work/psarc/Levels/Paris --mode meshes
```

Saída em `work\models\<Área>_<Fase>_<arquivo>\`:
- o `.gltf` com o `.bin`;
- `textures\*.png`;
- com `--obj`, uma pasta `obj\` com um `.obj` e um `.mtl` por malha, sem posicionamento.

| Modo | Conteúdo |
|---|---|
| `--mode scene` (padrão) | A fase montada: hierarquia de instâncias (`GINS`) com as transformações locais |
| `--mode meshes` | Cada malha uma vez, na origem |

Os formatos foram tirados dos tratadores do jogo (`Sly4_Chunk_MHDR`, `MVTX`, `MIDX`, `MSHH`, `GLOD`, `GINH`, `MTLH`) e da tabela de tipos de vértice em `0x00F67A54`. Detalhes no docstring do script.

**Validação no Paris/Hideout** (2.767 malhas, 740 mil triângulos, 1.025 texturas, 17 s):
- todas as ligações por hash se resolvem;
- os vértices cabem nas esferas de contorno;
- as normais compactadas 11:11:10 têm comprimento 1;
- a ordem dos triângulos bate com as normais em 99,9%;
- o teclado holográfico do Bentley lê corretamente, então a cena não está espelhada.

**Inferido (não lido do código), com os valores brutos guardados em `extras`:**
- `blendMode`: 0 = opaco, 2 = transparente, 3 = aditivo (exportado como `BLEND`);
- `cullMode` 2 = material de dois lados.

As cores de vértice saem como atributo `_COLOR_RAW`, porque muitas são pretas (dados para o shader). Para usá-las como cor, passe `--vertex-colors`.

**Personagens e objetos com esqueleto:**
- usam o formato **PlayStation EDGE** (bloco `MEDG`), decodificado em `edge.py`;
- a descompressão de índices foi lida no código SPU do próprio jogo;
- saem com **skin glTF**: um nó por osso, `JOINTS_0`/`WEIGHTS_0` e matrizes inversas de bind.

Detalhes em [docs/EDGE.md](docs/EDGE.md). O Sly Cooper sai completo e texturizado.

**Animações (Havok):**

```powershell
.\tools\run.ps1 san2gltf.py /work/psarc/Levels/Paris/Hideout/All_Combined.san.cooked --mode characters
```

- `--mode characters` gera um `.gltf` por personagem em `work\models\<Área>_<Fase>_<arquivo>\characters\`;
- cada um traz a malha com skin e os clipes de animação (`HKAF`/`HKAD`) cujo esqueleto bate com o dela;
- os clipes são decodificados por `havok.py` (packfile Havok 2011.3, `hkaSplineCompressedAnimation`);
- no Hideout: Sly 18 clipes, Bentley 25, Murray 10, CarmelitaFox 4;
- nas 87 fases (`--out /work/models_all`): 2.551 personagens e objetos com esqueleto, 1.730 com animação, 20.252 clipes.

Formato em [docs/HAVOK.md](docs/HAVOK.md). Para conferir todos os clipes do jogo (classes, tipos de rotação, falhas): `.\tools\run.ps1 havok_survey.py /work/psarc/Levels`.

**Ainda não exportado:**
- trilhas de float e *root motion* das animações;
- `inputFmt` 8 do EDGE (7 segmentos por fase);
- LODs além do primeiro;
- efeitos (`EINS`), luzes (`LINS`) e contornos (`OUTL`).

### Visualizador

```powershell
.\tools\run_viewer.ps1
```

Abra `http://localhost:8765/viewer/?model=../models/Paris_Hideout_All_Combined/All_Combined.gltf`. É um three.js servido pelo Docker, só na sua máquina.

Parâmetros opcionais:

| Parâmetro | Efeito |
|---|---|
| `only=regex` / `hide=regex` | Mostra ou esconde por nome de malha ou de textura |
| `cam=x,y,z&tgt=x,y,z` | Posiciona a câmera |
| `alpha=1` | Ativa o teste de alfa |
| `anim=nome` ou `anim=índice` | Escolhe o clipe de animação (padrão: o primeiro) |
| `t=segundos` | Congela a animação nesse instante |

Exemplo: `?model=../models/Paris_Hideout_All_Combined/characters/Static_Sly.gltf&anim=Sly_PingPong_MiniGame_Victory`.

O servidor também publica `work\models_all` (a exportação de todas as fases), por exemplo `?model=../models_all/West_Train_All_Combined/characters/STATIC_LonghornGuardBoxer.gltf`.

Para parar o servidor: `.\tools\run_viewer.ps1 -Stop`.

O `.gltf` também abre no Blender (File → Import → glTF 2.0).

## Testes

```powershell
.\tools\run.ps1 tests/test_eboot_scan.py
```

## Comparação com o RPCS3

`rpcs3_capture.ps1` roda o jogo no RPCS3 local (janela, sem a interface) e aperta teclas nos segundos pedidos. O F12 tira uma captura, salva em `screenshots\BCES01284` dentro da pasta do RPCS3. Enter, X, C e as setas chegam ao controle do jogo quando o jogador 1 usa o teclado. Roda no Windows, fora do contêiner:

```
.\tools\rpcs3_capture.ps1 -Keys "X@32,F12@38,F12@46,RETURN@58,F12@62" -Seconds 85
```
