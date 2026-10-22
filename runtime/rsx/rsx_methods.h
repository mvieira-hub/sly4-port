// RSX methods: the byte offsets the FIFO writes to. Bits 13-15 select the subchannel, so
// the 2D classes bound to subchannels 1-6 appear at 0x2000, 0x6000, 0x8000, 0xA000 and
// 0xC000. Names follow the gcm headers (and RPCS3's gcm_enums.h).
#pragma once

#include <cstdint>

namespace sly4::rsx {

namespace nv406e {  // FIFO/sync, any subchannel
constexpr uint32_t SetReference = 0x0050, SemaphoreContext = 0x0060, SemaphoreOffset = 0x0064,
                   SemaphoreAcquire = 0x0068, SemaphoreRelease = 0x006C;
}

namespace nv4097 {  // 3D, subchannel 0
constexpr uint32_t SetObject = 0x0000, NoOperation = 0x0100, Notify = 0x0104, WaitForIdle = 0x0110,
    SetContextDmaNotifies = 0x0180, SetContextDmaA = 0x0184, SetContextDmaB = 0x0188,
    SetContextDmaColorB = 0x018C, SetContextDmaState = 0x0190, SetContextDmaColorA = 0x0194,
    SetContextDmaZeta = 0x0198, SetContextDmaVertexA = 0x019C, SetContextDmaVertexB = 0x01A0,
    SetContextDmaSemaphore = 0x01A4, SetContextDmaReport = 0x01A8, SetContextDmaColorC = 0x01B4,
    SetContextDmaColorD = 0x01B8,
    SetSurfaceClipHorizontal = 0x0200, SetSurfaceClipVertical = 0x0204, SetSurfaceFormat = 0x0208,
    SetSurfacePitchA = 0x020C, SetSurfaceColorAOffset = 0x0210, SetSurfaceZetaOffset = 0x0214,
    SetSurfaceColorBOffset = 0x0218, SetSurfacePitchB = 0x021C, SetSurfaceColorTarget = 0x0220,
    SetSurfacePitchZ = 0x022C, InvalidateZcull = 0x0234, SetSurfacePitchC = 0x0280, SetSurfacePitchD = 0x0284,
    SetSurfaceColorCOffset = 0x0288, SetSurfaceColorDOffset = 0x028C, SetWindowOffset = 0x02B8,
    SetDitherEnable = 0x0300, SetAlphaTestEnable = 0x0304, SetAlphaFunc = 0x0308, SetAlphaRef = 0x030C,
    SetBlendEnable = 0x0310, SetBlendFuncSfactor = 0x0314, SetBlendFuncDfactor = 0x0318, SetBlendColor = 0x031C,
    SetBlendEquation = 0x0320, SetColorMask = 0x0324, SetStencilTestEnable = 0x0328, SetStencilMask = 0x032C,
    SetStencilFunc = 0x0330, SetStencilFuncRef = 0x0334, SetStencilFuncMask = 0x0338, SetStencilOpFail = 0x033C,
    SetStencilOpZfail = 0x0340, SetStencilOpZpass = 0x0344, SetTwoSidedStencilTestEnable = 0x0348,
    SetBackStencilMask = 0x034C, SetBackStencilFunc = 0x0350, SetBackStencilFuncRef = 0x0354,
    SetBackStencilFuncMask = 0x0358, SetBackStencilOpFail = 0x035C, SetBackStencilOpZfail = 0x0360,
    SetBackStencilOpZpass = 0x0364, SetShadeMode = 0x0368, SetBlendEnableMrt = 0x036C, SetColorMaskMrt = 0x0370,
    SetLogicOpEnable = 0x0374, SetLogicOp = 0x0378, SetBlendColor2 = 0x037C, SetDepthBoundsTestEnable = 0x0380,
    SetDepthBoundsMin = 0x0384, SetDepthBoundsMax = 0x0388, SetClipMin = 0x0394, SetClipMax = 0x0398,
    SetControl0 = 0x03B8, SetLineWidth = 0x03BC, SetLineSmoothEnable = 0x03C0,
    SetScissorHorizontal = 0x08C0, SetScissorVertical = 0x08C4, SetFogMode = 0x08CC, SetFogParams = 0x08D0,
    SetShaderProgram = 0x08E4, SetVertexTextureOffset = 0x0900, SetViewportHorizontal = 0x0A00,
    SetViewportVertical = 0x0A04, SetViewportOffset = 0x0A20, SetViewportScale = 0x0A30,
    SetPolyOffsetPointEnable = 0x0A60, SetPolyOffsetLineEnable = 0x0A64, SetPolyOffsetFillEnable = 0x0A68,
    SetDepthFunc = 0x0A6C, SetDepthMask = 0x0A70, SetDepthTestEnable = 0x0A74, SetPolygonOffsetScaleFactor = 0x0A78,
    SetPolygonOffsetBias = 0x0A7C, SetTextureControl2 = 0x0B00, SetTexCoordControl = 0x0B40,
    SetTransformProgram = 0x0B80, SetTwoSideLightEn = 0x142C, SetUserClipPlaneControl = 0x1478,
    SetVertexData3fM = 0x1500, SetVertexDataArrayOffset = 0x1680, InvalidateVertexCacheFile = 0x1710,
    InvalidateVertexFile = 0x1714, SetVertexDataBaseOffset = 0x1738, SetVertexDataBaseIndex = 0x173C,
    SetVertexDataArrayFormat = 0x1740, ClearReportValue = 0x17C8, SetZpassPixelCountEnable = 0x17CC,
    GetReport = 0x1800, SetZcullStatsEnable = 0x1804, SetBeginEnd = 0x1808, ArrayElement16 = 0x180C,
    ArrayElement32 = 0x1810, DrawArrays = 0x1814, InlineArray = 0x1818, SetIndexArrayAddress = 0x181C,
    SetIndexArrayDma = 0x1820, DrawIndexArray = 0x1824, SetFrontPolygonMode = 0x1828, SetBackPolygonMode = 0x182C,
    SetCullFace = 0x1830, SetFrontFace = 0x1834, SetPolySmoothEnable = 0x1838, SetCullFaceEnable = 0x183C,
    SetTextureControl3 = 0x1840, SetVertexData2fM = 0x1880, SetVertexData2sM = 0x1900,
    SetVertexData4ubM = 0x1940, SetVertexData4sM = 0x1980, SetTextureOffset = 0x1A00, SetTextureFormat = 0x1A04,
    SetTextureAddress = 0x1A08, SetTextureControl0 = 0x1A0C, SetTextureControl1 = 0x1A10, SetTextureFilter = 0x1A14,
    SetTextureImageRect = 0x1A18, SetTextureBorderColor = 0x1A1C, SetVertexData4fM = 0x1C00,
    SetColorKeyColor = 0x1D00, SetShaderControl = 0x1D60, SetIndexedConstantReadLimits = 0x1D64,
    SetSemaphoreOffset = 0x1D6C, BackEndWriteSemaphoreRelease = 0x1D70, TextureReadSemaphoreRelease = 0x1D74,
    SetZminMaxControl = 0x1D78, SetAntiAliasingControl = 0x1D7C, SetSurfaceCompression = 0x1D80,
    SetZcullEn = 0x1D84, SetShaderWindow = 0x1D88, SetZstencilClearValue = 0x1D8C, SetColorClearValue = 0x1D90,
    ClearSurface = 0x1D94, SetClearRectHorizontal = 0x1D98, SetClearRectVertical = 0x1D9C,
    SetClipIdTestEnable = 0x1DA4, SetRestartIndexEnable = 0x1DAC, SetRestartIndex = 0x1DB0,
    SetLineStipple = 0x1DB4, SetLineStipplePattern = 0x1DB8, SetVertexData1fM = 0x1E40,
    SetTransformExecutionMode = 0x1E94, SetRenderEnable = 0x1E98, SetTransformProgramLoad = 0x1E9C,
    SetTransformProgramStart = 0x1EA0, SetZcullControl0 = 0x1EA4, SetZcullControl1 = 0x1EA8,
    SetScullControl = 0x1EAC, SetPointSize = 0x1EE0, SetPointParamsEnable = 0x1EE4,
    SetPointSpriteControl = 0x1EE8, SetTransformTimeout = 0x1EF8, SetTransformConstantLoad = 0x1EFC,
    SetTransformConstant = 0x1F00, SetFrequencyDividerOperation = 0x1FC0, SetAttribColor = 0x1FC4,
    SetAttribTexCoord = 0x1FC8, SetAttribTexCoordEx = 0x1FCC, SetAttribUclip0 = 0x1FD0, SetAttribUclip1 = 0x1FD4,
    InvalidateL2 = 0x1FD8, SetReduceDstColor = 0x1FDC, SetNoParanoidTextureFetches = 0x1FE0,
    SetShaderPacker = 0x1FE4, SetVertexAttribInputMask = 0x1FF0, SetVertexAttribOutputMask = 0x1FF4,
    SetTransformBranchBits = 0x1FF8;
}

namespace nv0039 {  // memory to memory copy, subchannel 1
constexpr uint32_t SetContextDmaBufferIn = 0x2184, SetContextDmaBufferOut = 0x2188, OffsetIn = 0x230C,
                   OffsetOut = 0x2310, PitchIn = 0x2314, PitchOut = 0x2318, LineLengthIn = 0x231C,
                   LineCount = 0x2320, Format = 0x2324, BufferNotify = 0x2328;
}
namespace nv3062 {  // 2D surface, subchannel 3
constexpr uint32_t SetContextDmaImageSource = 0x6184, SetContextDmaImageDestin = 0x6188, SetColorFormat = 0x6300,
                   SetPitch = 0x6304, SetOffsetSource = 0x6308, SetOffsetDestin = 0x630C;
}
namespace nv309e {  // swizzled surface, subchannel 4
constexpr uint32_t SetContextDmaImage = 0x8184, SetFormat = 0x8300, SetOffset = 0x8304;
}
namespace nv308a {  // image from CPU, subchannel 5
constexpr uint32_t SetContextSurface = 0xA198, SetColorConversion = 0xA2F8, SetColorFormat = 0xA300,
                   Point = 0xA304, SizeOut = 0xA308, SizeIn = 0xA30C, Color = 0xA400;
}
namespace nv3089 {  // scaled image (blit), subchannel 6
constexpr uint32_t SetContextDmaImage = 0xC184, SetContextSurface = 0xC198, SetColorConversion = 0xC2FC,
                   SetColorFormat = 0xC300, SetOperation = 0xC304, ClipPoint = 0xC308, ClipSize = 0xC30C,
                   ImageOutPoint = 0xC310, ImageOutSize = 0xC314, DsDx = 0xC318, DtDy = 0xC31C,
                   ImageInSize = 0xC400, ImageInFormat = 0xC404, ImageInOffset = 0xC408, ImageIn = 0xC40C;
}

// Commands of the driver itself (libgcm's flip).
constexpr uint32_t GcmFlipCommand = 0xFEAC;

// Name of the method at `reg` ("SET_TEXTURE_OFFSET" with index 3...), or nullptr.
const char* method_name(uint32_t reg, uint32_t& index);

}  // namespace sly4::rsx
