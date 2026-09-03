struct VertexInput {
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
};

struct TransformConstants {
    float4x4 model_view_projection;
};

[[vk::push_constant]] ConstantBuffer<TransformConstants> transform_constants;

VertexOutput main(VertexInput input) {
    VertexOutput output;
    output.position = mul(transform_constants.model_view_projection,
                          float4(input.position, 0.0, 1.0));
    output.color = input.color;
    output.uv = input.uv;
    return output;
}
