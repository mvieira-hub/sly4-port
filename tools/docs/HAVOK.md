# Animações: Havok (`HKAF` / `HKAD` / `HKSF`)

As animações dos personagens usam o **Havok Animation 2011.3.0-r1**, guardado como *packfile* binário dentro de blocos Sanzaru. O decodificador fica em `tools/havok.py`; o `san2gltf.py --mode characters` o usa para exportar os clipes no glTF.

No jogo inteiro (87 fases) há 22.830 clipes, 4.320 distintos, e **todos decodificam** (`tools/havok_survey.py` faz o levantamento).

Validação no Paris/Hideout:
- o quadro 0 de cada clipe bate com a pose de referência do esqueleto Havok (diferença mediana 0);
- o movimento é contínuo entre blocos;
- clipes exportados: Sly 18, Bentley 25, Murray 10, CarmelitaFox 4, entre outros;
- no visualizador, o Sly joga pingue-pongue e comemora com a malha deformando corretamente.

## Blocos Sanzaru

| Bloco | Conteúdo |
|---|---|
| `HKSF` | `HKSH` + `HKSD`: packfile com o `hkaSkeleton` |
| `HKAF` | `HKAH` + `HKAD`: um clipe |
| `HKAH` | `u8 ver`, `char clip[64]` (nome do clipe), `char sourceFile[64]` |
| `HKAD` | Packfile com `hkaAnimationContainer`, `hkaAnimationBinding` e a animação |

## Packfile

Os cabeçalhos e as structs estão em **big-endian**, com ponteiros de 4 bytes.

```
0x00  u32 57E0E057, u32 10C0C010     magic
0x10  u8 ptrSize (4), u8 littleEndian (0), ...
0x14  u32 numSections
0x28  char version[16]                "hk_2011.3.0-r1"
0x40  seções de 0x30 bytes:
      char tag[20], u32 absoluteDataStart,
      u32 localFixups, globalFixups, virtualFixups, exports, imports, end
      (offsets relativos a absoluteDataStart)
```

| Fixup | Registro | Significado |
|---|---|---|
| Local | `u32 src, u32 dst` | Ponteiro dentro da mesma seção |
| Global | `u32 src, u32 section, u32 dst` | Ponteiro para outra seção |
| Virtual | `u32 obj, u32 section, u32 className` | Início de um objeto e o nome da classe |

As tabelas são completadas com `0xFF` até múltiplos de 16 bytes; registros com `src = 0xFFFFFFFF` são ignorados.

## Classes usadas

| Classe | Campos (offset no objeto) |
|---|---|
| `hkaSkeleton` | `+08` nome, `+0C` `parentIndices` (s16), `+18` ossos (8 bytes: nome, lock), `+24` `referencePose` (`hkQsTransform`, 48 bytes) |
| `hkaAnimationBinding` | `+08` nome do esqueleto original, `+0C` animação, `+10` `transformTrackToBoneIndices` (s16) |
| `hkaSplineCompressedAnimation` | Ver abaixo |
| `hkaInterleavedUncompressedAnimation` | `+28` `hkQsTransform[numFrames × numTracks]`, por quadro |

`hkQsTransform`: translação `vec4`, rotação `(x, y, z, w)`, escala `vec4`.

## `hkaSplineCompressedAnimation`

```
+0C f32 duration           +10 s32 numberOfTransformTracks   +14 s32 numberOfFloatTracks
+28 s32 numFrames          +2C s32 numBlocks                 +30 s32 maxFramesPerBlock
+34 s32 maskAndQuantizationSize                              +40 f32 frameDuration
+44 hkArray<u32> blockOffsets                                +74 hkArray<u8> data
```

**O blob `data` é little-endian** mesmo no PS3 (`endian = 0`), ao contrário das structs ao redor.

### Bloco

Cada bloco cobre `maxFramesPerBlock` quadros; o último quadro de um bloco é o primeiro do seguinte, então o passo entre blocos é `maxFramesPerBlock − 1`.

```
4 bytes por trilha: quantização, máscara de posição, de rotação e de escala
(em start + maskAndQuantizationSize, alinhado a 4) dados das trilhas, na ordem posição, rotação, escala
```

| Byte de quantização | Uso |
|---|---|
| bits 0–1 | Posição: 0 = 8 bits, 1 = 16 bits |
| bits 2–5 | Tipo da rotação |
| bits 6–7 | Escala: 0 = 8 bits, 1 = 16 bits |

| Bits da máscara | Significado |
|---|---|
| `0x01`, `0x02`, `0x04` | Componente x/y/z estático (um `f32`) |
| `0x10`, `0x20`, `0x40` | Componente x/y/z em spline |
| nenhum | Valor padrão (posição 0, escala 1, rotação identidade) |

### Vetor em spline (posição e escala)

```
u16 n, u8 degree, u8 knots[n + degree + 2], alinha 4
por componente: f32 min, f32 max (spline) | f32 valor (estático)
n + 1 pontos de controle, cada componente em spline quantizado em 8 ou 16 bits
  valor = min + (max − min) × q / (2^bits − 1)
alinha 4
```

A curva é uma **B-spline** de grau `degree`, avaliada pelo algoritmo de de Boor. Os nós são o índice do quadro dentro do bloco.

### Rotação

O mesmo cabeçalho (`n`, `degree`, nós) com o alinhamento do tipo; depois `n + 1` quatérnios compactados. Uma rotação estática é um único quatérnio.

| Tipo | Bytes | Alinhamento | Uso no jogo (trilhas × blocos) | Estado |
|---|---|---|---|---|
| 0 `POLAR32` | 4 | 4 | 0 | Não implementado |
| 1 `THREECOMP40` | 5 | 1 | 416.569 | Implementado |
| 2 `THREECOMP48` | 6 | 2 | 91.866 | Implementado |
| 3 `THREECOMP24` | 3 | 1 | 7.730 | Implementado |
| 4 `STRAIGHT16` | 2 | 2 | 888 | Implementado |
| 5 `UNCOMPRESSED` | 16 | 4 | 0 | Implementado |

Os três formatos "smallest three" guardam os três menores componentes e reconstroem o maior como `√(1 − x² − y² − z²)`:

- **`THREECOMP40`:** 40 bits LE; três componentes de 12 bits (bits 0, 12, 24), menos 2047, × (√½ / 2047). Os bits 36–37 dão a posição do componente omitido e o bit 38 inverte o sinal dele. Confirmado no decodificador do jogo (`0x00ABABD8`).
- **`THREECOMP48`:** três `u16` LE com 15 bits de valor, menos 16383, × (√½ / 16383). A posição do omitido é `(bit 15 de y) << 1 | (bit 15 de x)`; o bit 15 de z inverte o sinal.
- **`THREECOMP24`:** um byte por componente guardado, 7 bits de valor, menos 63, × (√½ / 63). O bit 7 do byte 0 é o bit baixo da posição do omitido, o do byte 1 é o bit alto, e o do byte 2 inverte o sinal. Os componentes guardados ficam em ordem crescente de índice.
- **`STRAIGHT16`:** os quatro componentes em nibbles, `(n − 7) / 7`, depois normalizados. Byte 0 = `x | y << 4`, byte 1 = `z | w << 4`.

**Como 24 e 16 foram resolvidos.** A HavokLib (decodificador aberto) não implementa esses dois. Uma varredura de layouts sobre os dados apontou a família certa, mas não decidiu entre as permutações. A resposta veio do EBOOT, que inclui o compressor do Havok:
- a tabela de constantes do `hkaSplineCompressedAnimation.cpp` fica em `0x00B646B0`/`0x00B6E1B0`, e os escalares da TOC (r2 + `0x4960`…) trazem 7, 63, 2047, 16383 e as constantes do `POLAR32`;
- o codificador do 24 bits (`0x00AB9E44`) calcula `int(c × √2 × 63) + 63` e põe os bits da posição com `rlwinm` (bit 0 no byte 0, bit 1 no byte 1);
- o codificador do 16 bits (`0x00AB9C8C`) e o decodificador (`0x00ABAE2C`, que normaliza com `vrsqrtefp`) definem os nibbles.

O código usa as instruções `lvlx`/`lvrx`, exclusivas do Cell, que o Ghidra e o capstone não decodificam. A desmontagem foi feita com o objdump do binutils compilado para `powerpc64-linux` (`tools/ppc/Dockerfile`, imagem `sly4-ppc:1`, opção `-M cell`).

## Ligação com a malha

O esqueleto da malha vem do `BONS` (ver [EDGE.md](EDGE.md)); o Havok tem o seu próprio. Os dois são casados **pelo nome do osso**:
- pelo menos 90% dos ossos do esqueleto Havok precisam existir no `BONS`;
- além disso, o esqueleto precisa ser (quase) o mais parecido com o `BONS` entre todos os da fase, pela sobreposição de Jaccard dos dois conjuntos de nomes, com tolerância de 90% do melhor. Sem essa regra, esqueletos pequenos de nomes genéricos (`Root`, `Pelvis`, `Head`…), como o do morcego (19 ossos), cabem inteiros no do guarda Longhorn (94), e os clipes do morcego iam parar no guarda. Na exportação de todas as fases, a regra antiga gerava cerca de 8 mil clipes falsos, por exemplo 60 clipes do LizardGuard no Sly da Arábia (cobertura 94%, Jaccard 0,38);
- `transformTrackToBoneIndices` leva a trilha ao osso Havok, e o nome leva ao nó glTF.

As trilhas trazem transformações **locais** (relativas ao pai), que viram canais glTF `translation`/`rotation`/`scale`. A exportação amostra cada quadro com interpolação `LINEAR`, mantém o sinal do quatérnio contínuo, reduz canais constantes a uma chave e omite escala identidade.

## Lacunas conhecidas

- Trilhas de float (`numberOfFloatTracks`) e movimento de raiz (*root motion*) são ignorados.
- Rotações `POLAR32` lançam `NotImplementedError`; nenhum clipe do jogo as usa.
- Personagens cujo `BONS` usa outros nomes de osso (por exemplo, a TimeMachine) ficam sem clipes.
- Os maiores saltos aparecem nos ossos de acessório (`AttachRoot`, `AttachHand_R`), que o jogo reposiciona em tempo de execução.
