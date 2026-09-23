Texture2D<float> scene_depth : register(t0, space0);
cbuffer CullingConstants : register(b1, space0) {
    float4x4 inverse_projection;
    float4x4 clip_to_shadow;
    float4 shadow_parameters;
    uint4 tiles;
    uint4 viewport;
};
struct LocalLight { float4 position_range; float4 color_intensity; float4 direction_spot; float4 cone; };
StructuredBuffer<LocalLight> lights : register(t2, space0);
RWByteAddressBuffer clusters : register(u3, space0);
groupshared uint minimum_depth, maximum_depth, covered;

float3 unproject(float2 pixel, float depth) {
    float4 view_position=mul(inverse_projection,float4(pixel/float2(viewport.xy)*2.0f-1.0f,depth,1));
    return view_position.xyz/view_position.w;
}
[numthreads(16,16,1)]
void main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint3 pixel : SV_DispatchThreadID) {
    if(local.x==0 && local.y==0) {minimum_depth=asuint(1.0f);maximum_depth=0;covered=0;}
    GroupMemoryBarrierWithGroupSync();
    if(all(pixel.xy<viewport.xy)) {
        float depth=scene_depth.Load(int3(pixel.xy,0));
        if(viewport.w!=0 ? depth>0.0f : depth<1.0f) {
            InterlockedMin(minimum_depth,asuint(depth));
            InterlockedMax(maximum_depth,asuint(depth));
            InterlockedAdd(covered,1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if(local.x!=0 || local.y!=0)return;
    uint base=(group.y*tiles.x+group.x)*tiles.w*4;
    uint count=0;
    if(covered!=0) {
        float2 lower_pixel=float2(group.xy*16);
        float2 upper_pixel=float2(min((group.xy+1)*16,viewport.xy));
        float3 lower=3.402823466e+38f, upper=-3.402823466e+38f;
        [unroll]for(uint z=0;z<2;++z) [unroll]for(uint y=0;y<2;++y) [unroll]for(uint x=0;x<2;++x) {
            float3 corner=unproject(float2(x ? upper_pixel.x:lower_pixel.x,y ? upper_pixel.y:lower_pixel.y),
                                   asfloat(z ? maximum_depth:minimum_depth));
            lower=min(lower,corner);upper=max(upper,corner);
        }
        for(uint index=0;index<tiles.z;++index) {
            float4 sphere=lights[index].position_range;
            float3 distance=max(max(lower-sphere.xyz,sphere.xyz-upper),0.0f);
            if(dot(distance,distance)<=sphere.w*sphere.w) {
                clusters.Store(base+(++count)*4,index);
            }
        }
    }
    clusters.Store(base,count);
}
