#include "gpu/spirv.h"

#include <cstring>

namespace sly4::spv {

Builder::Builder() {
    glsl_ = fresh();
}

void Builder::emit(std::vector<uint32_t>& out, Op o, const std::vector<uint32_t>& words) {
    out.push_back((uint32_t(words.size() + 1) << 16) | o);
    out.insert(out.end(), words.begin(), words.end());
}

Id Builder::type(Op o, const std::vector<uint32_t>& words) {
    std::vector<uint32_t> key{o};
    key.insert(key.end(), words.begin(), words.end());
    if (auto it = types_.find(key); it != types_.end()) return it->second;
    const Id id = fresh();
    std::vector<uint32_t> w{id};
    w.insert(w.end(), words.begin(), words.end());
    emit(globals_, o, w);
    types_[key] = id;
    return id;
}

Id Builder::constant(Op o, Id t, const std::vector<uint32_t>& words) {
    std::vector<uint32_t> key{o, t};
    key.insert(key.end(), words.begin(), words.end());
    if (auto it = constants_.find(key); it != constants_.end()) return it->second;
    const Id id = fresh();
    std::vector<uint32_t> w{t, id};
    w.insert(w.end(), words.begin(), words.end());
    emit(globals_, o, w);
    constants_[key] = id;
    return id;
}

Id Builder::t_void() { return type(OpTypeVoid, {}); }
Id Builder::t_bool() { return type(OpTypeBool, {}); }
Id Builder::t_float() { return type(OpTypeFloat, {32}); }
Id Builder::t_int() { return type(OpTypeInt, {32, 1}); }
Id Builder::t_uint() { return type(OpTypeInt, {32, 0}); }
Id Builder::t_vec(Id component, uint32_t n) { return type(OpTypeVector, {component, n}); }
Id Builder::t_array(Id element, uint32_t length) { return type(OpTypeArray, {element, c_uint(length)}); }
Id Builder::t_struct(const std::vector<Id>& members) {
    const Id id = fresh();
    std::vector<uint32_t> w{id};
    w.insert(w.end(), members.begin(), members.end());
    emit(globals_, OpTypeStruct, w);
    return id;
}
Id Builder::t_pointer(StorageClass sc, Id t) { return type(OpTypePointer, {sc, t}); }
Id Builder::t_function(Id ret, const std::vector<Id>& params) {
    std::vector<uint32_t> w{ret};
    w.insert(w.end(), params.begin(), params.end());
    return type(OpTypeFunction, w);
}
Id Builder::t_sampled_image(Dim dim) {
    // OpTypeImage: sampled type, dim, depth 0, arrayed 0, MS 0, sampled 1, format Unknown.
    const Id image = type(OpTypeImage, {t_float(), dim, 0, 0, 0, 1, 0});
    return type(OpTypeSampledImage, {image});
}

Id Builder::c_float(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    return constant(OpConstant, t_float(), {bits});
}
Id Builder::c_uint(uint32_t v) { return constant(OpConstant, t_uint(), {v}); }
Id Builder::c_int(int32_t v) { return constant(OpConstant, t_int(), {uint32_t(v)}); }
Id Builder::c_bool(bool v) { return constant(v ? OpConstantTrue : OpConstantFalse, t_bool(), {}); }
Id Builder::c_composite(Id t, const std::vector<Id>& parts) { return constant(OpConstantComposite, t, parts); }
Id Builder::c_vec4(float x, float y, float z, float w) {
    return c_composite(t_vec4(), {c_float(x), c_float(y), c_float(z), c_float(w)});
}

Id Builder::variable(Id pointer_type, StorageClass sc) {
    const Id id = fresh();
    emit(globals_, OpVariable, {pointer_type, id, sc});
    return id;
}

void Builder::decorate(Id target, Decoration d, std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> w{target, d};
    w.insert(w.end(), args.begin(), args.end());
    emit(annotations_, OpDecorate, w);
}
void Builder::member_decorate(Id st, uint32_t member, Decoration d, std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> w{st, member, d};
    w.insert(w.end(), args.begin(), args.end());
    emit(annotations_, OpMemberDecorate, w);
}

Id Builder::begin_function(Id ret_type, Id fn_type) {
    const Id id = fresh();
    emit(code_, OpFunction, {ret_type, id, 0, fn_type});
    label();
    function_body_start_ = code_.size();
    locals_.clear();
    return id;
}

Id Builder::local(Id pointer_type) {
    const Id id = fresh();
    emit(locals_, OpVariable, {pointer_type, id, Function});
    return id;
}

Id Builder::label() {
    const Id id = fresh();
    emit(code_, OpLabel, {id});
    return id;
}

void Builder::end_function() {
    code_.insert(code_.begin() + std::ptrdiff_t(function_body_start_), locals_.begin(), locals_.end());
    locals_.clear();
    emit(code_, OpFunctionEnd, {});
}

Id Builder::op(Op o, Id t, const std::vector<uint32_t>& operands) {
    const Id id = fresh();
    std::vector<uint32_t> w{t, id};
    w.insert(w.end(), operands.begin(), operands.end());
    emit(code_, o, w);
    return id;
}
Id Builder::op(Op o, Id t, std::initializer_list<uint32_t> operands) {
    return op(o, t, std::vector<uint32_t>(operands));
}
void Builder::op_void(Op o, const std::vector<uint32_t>& operands) { emit(code_, o, operands); }
void Builder::op_void(Op o, std::initializer_list<uint32_t> operands) {
    op_void(o, std::vector<uint32_t>(operands));
}

Id Builder::ext(Id t, Glsl inst, std::initializer_list<Id> args) {
    std::vector<uint32_t> w{glsl_, inst};
    w.insert(w.end(), args.begin(), args.end());
    return op(OpExtInst, t, w);
}

void Builder::entry_point(ExecutionModel m, Id fn, const char* name, const std::vector<Id>& interface) {
    std::vector<uint32_t> w{m, fn};
    const size_t len = std::strlen(name) + 1;  // with the terminating zero
    std::vector<uint32_t> s((len + 3) / 4, 0);
    std::memcpy(s.data(), name, len);
    w.insert(w.end(), s.begin(), s.end());
    w.insert(w.end(), interface.begin(), interface.end());
    emit(entry_, OpEntryPoint, w);
}

void Builder::execution_mode(Id fn, ExecutionMode m) { emit(modes_, OpExecutionMode, {fn, m}); }

std::vector<uint32_t> Builder::finish() const {
    std::vector<uint32_t> out{0x07230203, 0x00010000, 0, next_, 0};
    emit(out, OpCapability, {1});  // Shader
    // OpExtInstImport %glsl "GLSL.std.450"
    const char name[] = "GLSL.std.450";
    std::vector<uint32_t> w{glsl_};
    std::vector<uint32_t> s((sizeof(name) + 3) / 4, 0);
    std::memcpy(s.data(), name, sizeof(name));
    w.insert(w.end(), s.begin(), s.end());
    emit(out, OpExtInstImport, w);
    emit(out, OpMemoryModel, {0, 1});  // Logical, GLSL450
    out.insert(out.end(), entry_.begin(), entry_.end());
    out.insert(out.end(), modes_.begin(), modes_.end());
    out.insert(out.end(), annotations_.begin(), annotations_.end());
    out.insert(out.end(), globals_.begin(), globals_.end());
    out.insert(out.end(), code_.begin(), code_.end());
    return out;
}

}  // namespace sly4::spv
