#include <cassert>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

#include "shader_contract.hpp"

int main() {
    aeg::shader::ShaderReflection reflection{};
    assert(reflection.empty());
    aeg::shader::ShaderParameterField parameter{};
    parameter.id = 1;
    parameter.type = aeg::shader::ShaderParameterType::kFloat32;
    parameter.size = 4;
    parameter.default_value = std::vector<std::byte>(4);
    reflection.parameters.push_back(std::move(parameter));
    assert(!reflection.empty());
    reflection.constant_ranges.push_back({0, 4, aeg::rhi::ShaderStage::kVertex, "Material"});
    aeg::shader::ShaderProgram program{};
    program.name = "parameter-pack";
    program.stages.push_back({aeg::rhi::ShaderStage::kVertex, "main", std::vector<std::byte>(4)});
    program.contract.reflection = reflection;
    const auto packed = aeg::shader::pack_parameter_block(program, aeg::pipeline::ParameterBlock{{1, 2.5}});
    assert(packed && packed.value.size() == 4);
    float value = 0.0f;
    std::memcpy(&value, packed.value.data(), sizeof(value));
    assert(value == 2.5f);
    assert(!aeg::shader::pack_parameter_block(program, aeg::pipeline::ParameterBlock{{1, std::string{"bad"}}}));
    return 0;
}
