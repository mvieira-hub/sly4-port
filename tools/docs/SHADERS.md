# Pipeline de shaders: RSX (Cg compilado) para HLSL / SPIR-V

Base: `System/Shaders.Zip` do `Sly4.psarc`, inventariado com `cgbin_info.py`
(saída em `work/shaders/`: `summary.csv`, um `.json` por shader com os parâmetros e o `.ucode` bruto).

## 1. O que existe

| Item | Valor |
|---|---|
| Arquivos | 541: 326 vertex (`.vpo`, perfil `sce_vp_rsx`) e 215 fragment (`.fpo`, perfil `sce_fp_rsx`) |
| Formato | `CgBinaryProgram` big-endian, revisão 6, com a tabela de parâmetros **com nomes** (`gDiffuseSampler`, `gNormalSampler`…) |
| Código | ~10,7 mil instruções de VP e ~11,5 mil de FP. O maior FP tem 249 instruções (`SlyCharacterFakeFurPP_Lit_Nrm_Spec_3`) |
| Famílias | ~200 bases de material. As maiores são `SlyCharacter*`, `SlyDecal`, `SlyProp`, `SlyEnvMap`, `SlyMirror`, `SlyTerrain`, pelo, água e pós-processamento |
| Variações | Sufixos `_1/_2/_3` (provavelmente quantidade de luzes) e `_Lit/_Nrm/_Spec`. 45 VPs com skinning (`_SK_V`) |
| Casos especiais | 7 FPs escrevem profundidade (`depthReplace`: resolve de MSAA, `RestoreZCull`, outline). Nenhum usa `KIL` |

Não há código-fonte Cg no disco, só o microcódigo do RSX. Os nomes dos parâmetros já vêm no binário, então a interface de cada shader (uniforms, samplers, atributos) está toda documentada.

## 2. Estratégia recomendada

### Caminho A: aproveitar o RPCS3 (mais rápido, bom para referência)
1. Rode o jogo no RPCS3 com o renderizador **Vulkan** e ative, nas opções de depuração, o registro de programas de shader (*Log shader programs*). O emulador grava o GLSL que ele mesmo gera para cada par VP/FP realmente usado.
2. Compile esse GLSL com o **glslang** (`glslangValidator -V`) para SPIR-V e valide com `spirv-val`.
3. Para DirectX 12, use **SPIRV-Cross** (`spirv-cross --hlsl --shader-model 60`) e depois **DXC** para DXIL.

Prós: fiel ao hardware, e é exatamente o que roda hoje no emulador. Contras: o GLSL sai cheio de emulação (registradores `h`/`r`, `fx12`, remap de textura, correções de profundidade), gera só os pares que o jogo de fato carregou e herda a licença **GPL-2.0** do RPCS3 se o código dele for incorporado à sua ferramenta.

### Caminho B: decompilador próprio (offline, cobre os 541)
1. **Decodificar o ucode.**
   - VP: instruções de 128 bits (4 palavras BE), com unidades vetorial e escalar no mesmo slot, 16 entradas `v0..v15` (`attributeInputMask`) e saídas `o[]` (HPOS, COL0/1, BFC0/1, FOG, PSZ, TEX0-9).
   - FP: instruções de 128 bits com **cada palavra de 32 bits com as metades de 16 bits trocadas**. Constantes ficam *inline* (16 bytes logo após a instrução que as usa), e há registradores `R` (fp32) e `H` (fp16).
2. **Levantar para uma IR SSA simples:** mov/mad/dp3/dp4/rsq/tex/txp/kil etc., com máscaras de escrita e swizzles.
3. **Ligar parâmetros:**
   - Uniforms de VP: `resIndex` é o registrador `c[n]`. Agrupe em um UBO/cbuffer com o mesmo layout.
   - Constantes de FP que o jogo altera em tempo de execução: os offsets estão em `embeddedConstOffsets` no `.json`. Viram campos de UBO, em vez de imediatos.
   - Samplers: `TEXUNITn` vira `binding = n`.
4. **Emitir GLSL 450 ou HLSL SM 6.0** diretamente da IR e compilar com glslang/DXC.

O RPCS3 serve como **referência de semântica** (`rpcs3/Emu/RSX/Program/VertexProgramDecompiler.cpp` e `FragmentProgramDecompiler.cpp`). Se copiar código, a ferramenta passa a ser GPL-2.0.

### Caminho C: reescrever à mão por família (melhor para um port nativo)
São ~200 famílias, mas o núcleo visual é de 15 a 20 (personagem, pelo, prop, terreno, água, decal, env map, pós-processamento). Use o Caminho A ou B só como referência e reescreva essas famílias em HLSL limpo, com os nomes de parâmetro originais. Resultado: shaders legíveis, sem emulação e fáceis de otimizar.

## 3. Diferenças do RSX que precisam de tratamento

| Tema | RSX / PS3 | Alvo (DX12 / Vulkan) | Tratamento |
|---|---|---|---|
| Profundidade no clip space | z em [-1, 1] (estilo OpenGL) | z em [0, 1] | Ao final do VP: `z = (z + w) * 0.5`, ou `VK_EXT_depth_clip_control` |
| Origem da tela | inferior esquerda | DX: superior esquerda; VK: Y para baixo | Inverter Y no VP ou usar viewport com altura negativa |
| Precisão | registradores `H` (fp16) e `fx12` | fp32 | Usar float (ou `min16float` no DX) e saturar onde o `fx12` clampava |
| Remap de textura | campo `remap` do GTF (todas as texturas do jogo usam o padrão `0xAAE4`) | swizzle do SRV / `VkComponentMapping` | Identidade, para este jogo |
| Texturas | DXT1/DXT5 (49.981 de 49.981) | BC1/BC3 | Direto, sem conversão (ver `texr2dds.py`) |
| `depthReplace` | 7 FPs | `SV_Depth` / `gl_FragDepth` | Manter o resolve de MSAA ou trocar pelo resolve nativo da API |
| Clip planes | `userClipMask` no VP | `SV_ClipDistance` | Mapear 1:1 |

## 4. Ferramentas

| Ferramenta | Uso | Link |
|---|---|---|
| RPCS3 (código e emulador) | Referência do microcódigo e GLSL gerado | https://github.com/RPCS3/rpcs3 |
| glslang | GLSL para SPIR-V | https://github.com/KhronosGroup/glslang (ou o Vulkan SDK) |
| SPIRV-Cross | SPIR-V para HLSL/MSL/GLSL | https://github.com/KhronosGroup/SPIRV-Cross |
| DXC | HLSL para DXIL (DX12) | https://github.com/microsoft/DirectXShaderCompiler/releases |
| Vulkan SDK | Traz glslang, SPIRV-Cross e spirv-val juntos | https://vulkan.lunarg.com/sdk/home |
| RenderDoc | Capturar quadros do RPCS3 (Vulkan) e comparar com o port | https://renderdoc.org |

## 5. Ordem sugerida

1. `cgbin_info.py` (pronto): inventário e interfaces.
2. Caminho A em 3 ou 4 fases do jogo, para gerar o GLSL de referência das famílias principais.
3. Escolher entre o B (decompilador, se a meta for cobertura total e automática) e o C (reescrita, se a meta for um port nativo de qualidade).
4. Validação visual: capturas lado a lado no RenderDoc (RPCS3 versus port) por família de material.
