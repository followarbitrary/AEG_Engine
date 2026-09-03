[[vk::binding(0, 0)]] Texture2D<float4> texture_image;
[[vk::binding(1, 0)]] SamplerState texture_sampler;

struct FragmentInput {
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
};

float4 main(FragmentInput input) : SV_Target0 {
    return input.color * texture_image.Sample(texture_sampler, input.uv);
}
