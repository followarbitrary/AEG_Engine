#include "../shadow/visibility.hlsli"
Texture2D material_color : register(t0, space1);
SamplerState material_sampler : register(s1, space1);
cbuffer TextureMaterialConstants : register(b2, space1) { float4 material_tint; };
cbuffer OpaqueLightingConstants : register(b0, space2) {
    float4 directional_direction_intensity;
    float4 directional_color;
    float4 ambient_color;
};
Texture2D<float> shadow_map : register(t0, space3);
cbuffer CullingConstants : register(b1, space3) {
    float4x4 inverse_projection;
    float4x4 clip_to_shadow;
    float4 shadow_parameters;
    uint4 tiles;
    uint4 viewport;
};
struct LocalLight { float4 position_range; float4 color_intensity; float4 direction_spot; float4 cone; };
StructuredBuffer<LocalLight> lights : register(t2, space3);
ByteAddressBuffer clusters : register(t3, space3);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0, float3 normal : NORMAL) : SV_Target0 {
    const float3 n=normalize(normal);
    float4 albedo=material_color.Sample(material_sampler,uv)*material_tint;
    float directional=max(dot(n,normalize(-directional_direction_intensity.xyz)),0.0f);
    float visibility=viewport.z!=0 ? directional_shadow(shadow_map,position,clip_to_shadow,shadow_parameters):1.0f;
    float3 lighting=ambient_color.rgb+directional_color.rgb*directional_direction_intensity.w*directional*visibility;
    float4 view_h=mul(inverse_projection,float4(position.xy/float2(viewport.xy)*2.0f-1.0f,position.z,1));
    float3 surface=view_h.xyz/view_h.w;
    uint2 tile=uint2(position.xy)/16;
    uint base=(tile.y*tiles.x+tile.x)*tiles.w*4;
    uint count=min(clusters.Load(base),tiles.z);
    for(uint entry=0;entry<count;++entry) {
        uint index=clusters.Load(base+(entry+1)*4);
        if(index>=tiles.z)continue;
        LocalLight light=lights[index];
        float3 delta=light.position_range.xyz-surface;
        float distance=length(delta);
        float3 toward_light=delta/max(distance,0.00001f);
        float attenuation=saturate(1.0f-distance/light.position_range.w);
        attenuation*=attenuation;
        if(light.direction_spot.w>0.5f) {
            float cosine=dot(-toward_light,light.direction_spot.xyz);
            attenuation*=light.cone.x==light.cone.y ? step(light.cone.y,cosine):
                saturate((cosine-light.cone.y)/(light.cone.x-light.cone.y));
        }
        lighting+=light.color_intensity.rgb*light.color_intensity.w*attenuation*max(dot(n,toward_light),0.0f);
    }
    return float4(albedo.rgb*lighting,albedo.a);
}
