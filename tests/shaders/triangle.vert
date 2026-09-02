#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec4 in_color;

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform TransformConstants {
    mat4 model_view_projection;
} transform_constants;

void main() {
    gl_Position = transform_constants.model_view_projection * vec4(in_position, 0.0, 1.0);
    out_color = in_color;
}
