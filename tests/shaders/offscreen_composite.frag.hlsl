[[vk::binding(0, 0)]] Texture2D<float4> source_texture;
[[vk::binding(1, 0)]] SamplerState source_sampler;

struct FragmentInput {
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

float4 main(FragmentInput input) : SV_Target0 {
    return source_texture.Sample(source_sampler, input.uv);
}
