Texture2D<float4> source_texture : register(t0, space0);
SamplerState source_sampler : register(s1, space0);

struct FragmentInput {
    float2 uv : TEXCOORD0;
};

float4 main(FragmentInput input) : SV_Target0 {
    return source_texture.Sample(source_sampler, input.uv);
}
