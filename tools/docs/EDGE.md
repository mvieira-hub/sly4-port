# Personagens e malhas com skinning: formato PlayStation EDGE

Malhas com esqueleto (personagens e objetos animados) **não usam `MVTX`/`MIDX`**. A submalha tem `MHDR` + **`MEDG`**, no formato da biblioteca EDGE da Sony, processado nos SPUs. O formato está **resolvido** e implementado em `tools/edge.py`, que o `san2gltf.py` usa.

Validação no Paris/Hideout:
- 606 segmentos e 512 submalhas;
- **98,7% dos triângulos coerentes com as normais**. A referência são as malhas comuns, que dão 99,8%;
- 39 esqueletos;
- o Sly Cooper sai completo e texturizado no glTF.

## Esqueleto

Fontes: `Sly4_Chunk_SKHD`, `Sly4_Chunk_BONS`.

O `GEOB` traz um `SKEL`, que contém `SKHD` e `BONS`.

**`SKHD`:** `u8 ver`, `u32 numBones`.

**`BONS`:** `numBones` registros de 76 bytes, em little-endian:

| Campo | Tipo |
|---|---|
| `nameHash` | `u32` |
| `parentNameHash` | `u32` |
| `xBasis` | `vec3` |
| `zBasis` | `vec3` |
| `position` | `vec3` |
| `name` | `char[32]` |

- O pai é achado pelo hash.
- As transformações são **absolutas** (pose de repouso); y = z × x.
- No glTF, cada osso vira um nó com transformação local = inv(pai) × mundo, e a `inverseBindMatrix` é inv(mundo).

## `MEDG`

Fonte: `Sly4_Chunk_MEDG`, em `0x0054A490`.

```
u32 numSegments (LE)
por segmento:
  u16 x14 (LE): indexesSizes[2], spuVertexesSizes[6] (2 fluxos x 3),
                skinMatricesByteOffsets[2], skinMatricesSizes[2],
                skinIndexesAndWeightsSizes[2]
  u32 rsxOnlyVertexesSize, u32, u32
  16 bytes EdgeGeomSpuConfigInfo (BE):
     u8 flags, u8 cmdHole, u8 inputFmt, u8 inputFmt2, u8 outputFmt, u8 deltaFmt,
     u8 indexesFlavor<<4 | skinningFlavor (sempre 0x32), u8 skinMatrixFmt,
     u16 numVertexes, u16 numIndexes, u32 indexesOffset
  blob: align16(indices) + align16(fluxo 0) + align16(fluxo 1)
        + índices/pesos de skin + fluxo só-RSX
```

### Fluxos

| Fluxo | Conteúdo | Como foi validado |
|---|---|---|
| 0, `inputFmt` 0 | Posição `f32 x3` (BE) | Geometria |
| 0, `inputFmt` 7 | Posição em 32 bits, 11:11:10 com sinal (x bits 0–10, y 11–21, z 22–31), **× `MHDR.vtxScale`** | 98% coerente; 500 de 503 segmentos dentro da esfera de contorno |
| 1 | Normal [+ tangente + binormal], 11:11:10 com sinal, um `u32` cada (12 ou 4 bytes por vértice) | — |
| Skin | 4 × (`u8` peso, `u8` índice local); os pesos somam 255 | — |
| Só-RSX | Cor `u8 x4` + UV `half x2` (BE) | — |

**Paleta de ossos:** o índice local *k* vira global como `off0 + k` se `k < n0`; senão, `off1 + (k − n0)`. Aqui `off = skinMatricesByteOffsets / 48`, `n0 = skinMatricesSizes[0] / 48`, e cada matriz tem 48 bytes.

Validado no Sly: vértices presos a um único osso ficam em média a 0,15 do osso atribuído, contra 0,84 de um osso aleatório.

Alguns segmentos raros ainda são pulados e contados nas estatísticas como `edge_formato_N`. Na exportação de todas as 87 fases foram **59 segmentos de `inputFmt` 0**, cujo fluxo 0 tem menos de 12 bytes por vértice: outro layout, ainda não estudado.

## Índices comprimidos (sabor 3)

Fonte: **código SPU do próprio jogo**, no binário de job embutido no EBOOT.

| Endereço | Papel |
|---|---|
| `0x00D5F4B0` | O descompressor |
| `0x00D5FCD0` | O chamador: testa o sabor 2/3 no nibble alto do byte 6 da configuração |

Para chegar a eles, o binutils 2.35 foi compilado com `--target=spu-elf` (`tools/spu/Dockerfile`). Os jobs SPU estão como código solto no `.rodata` do EBOOT, a partir de `0xC40000` no arquivo. O descompressor foi achado pela leitura do cabeçalho: `rotmi …,-16` para o campo +0, `shufb` com `0x8006` para o byte +6, e `mpy` por `0xAAAB` para dividir por 3.

```
u16 numExplicit, u16 base, u16 maskBytes, u8 explicitBits, u8 0
mask (maskBytes)  1 bit por posição de vértice, MSB primeiro:
                  1 = índice explícito, 0 = próximo vértice novo (contador sequencial)
2 bits/triângulo  3 = triângulo novo (3 posições);
                  0, 1, 2 = reaproveita a aresta (p2,p0), (p1,p2), (p0,p1) do triângulo anterior p,
                  montando (aresta[1], aresta[0], posição)
(alinhado a byte) numExplicit valores de explicitBits bits, decodificados assim:
                  índice[i] = (valor[i] − base) + índice[i − 8]
```

Esse último passo usa **oito fluxos de delta intercalados** (laço em `0xD5F934`, que soma vetores de 8 meias-palavras). Por isso, nas malhas pequenas, "valor − base" parecia funcionar: os 8 primeiros valores não dependem dos anteriores.

## Ferramentas de pesquisa

| Arquivo | Função |
|---|---|
| `tools/spu_extract.py` | Extrai os ELFs SPU embutidos |
| `tools/spu/disasm_range.sh` | Desmonta faixas de código SPU solto |
| `tools/spu_funcs.py` | Divide a desmontagem em funções e resume cada uma |
| `tools/spu_find.py` | Procura padrões de instruções |
| `tools/edge_probe.py` | Laboratório de hipóteses do formato |
| `tools/edge_solve.py` | Busca guiada pela geometria |
| `tools/edge_samples.py` | Amostras de referência tiradas da geometria |
