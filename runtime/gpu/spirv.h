// A small SPIR-V module builder (docs/GPU.md, 5d): the RSX vertex and fragment programs
// are translated straight into SPIR-V, without a GLSL compiler in between. Types and
// constants are deduplicated; instructions go into the current function block.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace sly4::spv {

using Id = uint32_t;

// Opcodes, enums and GLSL.std.450 instructions used by the translators (SPIR-V 1.0).
enum Op : uint16_t {
    OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16,
    OpCapability = 17, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23,
    OpTypeImage = 25, OpTypeSampledImage = 27, OpTypeArray = 28, OpTypeStruct = 30, OpTypePointer = 32,
    OpTypeFunction = 33, OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43, OpConstantComposite = 44,
    OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65,
    OpDecorate = 71, OpMemberDecorate = 72, OpVectorShuffle = 79, OpCompositeConstruct = 80,
    OpCompositeExtract = 81, OpCompositeInsert = 82, OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88,
    OpImageSampleDrefImplicitLod = 89,
    OpImageSampleProjImplicitLod = 91, OpImageSampleProjDrefImplicitLod = 93, OpConvertFToS = 110, OpConvertSToF = 111, OpBitcast = 124,
    OpFNegate = 127, OpIAdd = 128, OpFAdd = 129, OpFSub = 131, OpFMul = 133, OpFDiv = 136, OpFMod = 141,
    OpISub = 130, OpVectorTimesScalar = 142, OpDot = 148, OpAny = 154, OpAll = 155, OpLogicalOr = 166,
    OpLogicalAnd = 167, OpLogicalNot = 168, OpSelect = 169, OpIEqual = 170, OpINotEqual = 171, OpSGreaterThan = 173,
    OpSLessThan = 177, OpFOrdEqual = 180, OpFOrdNotEqual = 182, OpFUnordNotEqual = 183, OpFOrdLessThan = 184,
    OpFOrdGreaterThan = 186, OpFOrdLessThanEqual = 188, OpFOrdGreaterThanEqual = 190, OpShiftRightLogical = 194,
    OpBitwiseAnd = 199, OpDPdx = 207, OpDPdy = 208, OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248,
    OpBranch = 249, OpBranchConditional = 250, OpSwitch = 251, OpKill = 252, OpReturn = 253,
};
enum StorageClass : uint32_t { UniformConstant = 0, Input = 1, Uniform = 2, Output = 3, Private = 6, Function = 7 };
enum Decoration : uint32_t {
    Block = 2, ArrayStride = 6, BuiltIn = 11, NoPerspective = 13, Flat = 14, Location = 30, Binding = 33,
    DescriptorSet = 34, Offset = 35,
};
enum BuiltInKind : uint32_t { Position = 0, PointSize = 1, FragCoord = 15, FrontFacing = 17, FragDepth = 22, VertexIndex = 42 };
enum ExecutionModel : uint32_t { Vertex = 0, Fragment = 4 };
enum ExecutionMode : uint32_t { OriginUpperLeft = 7, DepthReplacing = 12 };
enum Dim : uint32_t { Dim1D = 0, Dim2D = 1, Dim3D = 2, DimCube = 3 };
enum ImageOperand : uint32_t { Bias = 1, Lod = 2, Grad = 4 };
enum Glsl : uint32_t {
    Round = 1, Trunc = 3, FAbs = 4, FSign = 6, Floor = 8, Ceil = 9, Fract = 10, Sin = 13, Cos = 14, Pow = 26,
    Exp2 = 29, Log2 = 30, Sqrt = 31, InverseSqrt = 32, FMin = 37, FMax = 40, FClamp = 43, SClamp = 45, FMix = 46,
    Normalize = 69,
};

class Builder {
public:
    Builder();

    // Types (deduplicated).
    Id t_void();
    Id t_bool();
    Id t_float();
    Id t_int();
    Id t_uint();
    Id t_vec(Id component, uint32_t n);
    Id t_vec4() { return t_vec(t_float(), 4); }
    Id t_bvec4() { return t_vec(t_bool(), 4); }
    Id t_array(Id element, uint32_t length);
    Id t_struct(const std::vector<Id>& members);  // not deduplicated (decorations differ)
    Id t_pointer(StorageClass sc, Id type);
    Id t_function(Id ret, const std::vector<Id>& params = {});
    Id t_sampled_image(Dim dim, bool depth = false);  // depth: sampled with a comparison (Dref)

    // Constants (deduplicated).
    Id c_float(float v);
    Id c_uint(uint32_t v);
    Id c_int(int32_t v);
    Id c_bool(bool v);
    Id c_composite(Id type, const std::vector<Id>& parts);
    Id c_vec4(float x, float y, float z, float w);

    // Globals and annotations.
    Id variable(Id pointer_type, StorageClass sc);  // module scope
    void decorate(Id target, Decoration d, std::initializer_list<uint32_t> args = {});
    void member_decorate(Id st, uint32_t member, Decoration d, std::initializer_list<uint32_t> args = {});
    Id glsl() { return glsl_; }

    // Functions: begin_function, then label/instructions, then end_function.
    Id begin_function(Id ret_type, Id fn_type);
    Id local(Id pointer_type);  // OpVariable in the function's first block
    Id label();                 // new block
    Id fresh_id() { return next_++; }  // an id for a label created later with op_void(OpLabel)
    void end_function();
    // An instruction with a result: returns the new id.
    Id op(Op o, Id type, std::initializer_list<uint32_t> operands);
    Id op(Op o, Id type, const std::vector<uint32_t>& operands);
    // An instruction without result.
    void op_void(Op o, std::initializer_list<uint32_t> operands);
    void op_void(Op o, const std::vector<uint32_t>& operands);
    Id ext(Id type, Glsl inst, std::initializer_list<Id> args);

    void entry_point(ExecutionModel m, Id fn, const char* name, const std::vector<Id>& interface);
    void execution_mode(Id fn, ExecutionMode m);

    std::vector<uint32_t> finish() const;

private:
    Id next_ = 1;
    Id glsl_ = 0;
    std::vector<uint32_t> entry_, modes_, annotations_, globals_, code_, locals_;
    std::map<std::vector<uint32_t>, Id> types_, constants_;
    size_t function_body_start_ = 0;  // where this function's locals go

    Id fresh() { return next_++; }
    static void emit(std::vector<uint32_t>& out, Op o, const std::vector<uint32_t>& words);
    Id type(Op o, const std::vector<uint32_t>& words);
    Id constant(Op o, Id t, const std::vector<uint32_t>& words);
};

}  // namespace sly4::spv
