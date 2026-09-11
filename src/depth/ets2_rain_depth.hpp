#pragma once
#include <string>

// Nominal current-scene coordinates of five exact game 1.60 rain tone shaders.
// This follows the center sample, not the surrounding color-blur footprint.
namespace ets2_route {
inline constexpr unsigned RainModeCount=3;
struct RainLayout { const char* constantRow; unsigned requiredRows; };
inline constexpr RainLayout RainLayoutFor(int mode){
    return mode==1?RainLayout{"12",23}:mode==2?RainLayout{"13",24}:
           mode==3?RainLayout{"11",22}:RainLayout{nullptr,0};
}
inline int RainToneMode(const std::string& hash){
    // Non-DRR rain variant, observed in one eye with stock TAA.
    if(hash=="c38680c4e07ef9294f9181b89411988ff801d501c4e1fb59b28c45de4fced09a")return 3;
    // Same center-coordinate chain without the pointwise RGB correction block.
    if(hash=="6a022a63c84960562c8bfda3389c3d3340f9d96365be2317f73b695fbaa59c2f")return 1;
    if(hash=="32dd64ae99239492474ff2dfe26f0d64ee7b8ecd205a97885a98ddb4cec462ad")return 1;
    if(hash=="05f76344e0c29b06acfec9cf107cd405728e25d3ceafafd0143b38fb0a3d61c0")return 2;
    // Rain + light shafts: the complete center-coordinate chain is identical
    // to 05f76344. The extra t3/s3 sample adds RGB at the already-mapped UV;
    // CB0[25] affects that color contribution, not this depth replay.
    if(hash=="b7174d908c495f896b2e36cbd7dabb74e801beeea74fd533d6d5862ce60760d5")return 2;
    return 0;
}
inline const char* RainDepthShader(){return R"(
Texture2D<float> SceneDepth:register(t0);
Texture2D<float4> Distortion:register(t2);
Texture2D<float4> GateDepth:register(t12);
Texture2D<float4> GateMask:register(t14);
SamplerState DepthPoint:register(s0);
SamplerState DistortionSampler:register(s2);
cbuffer GameTone:register(b0){float4 data[24];}
float main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target {
    float2 delta=Distortion.Sample(DistortionSampler,uv).xy;
    float2 offset=mad(delta,data[RAIN_CB].xy,uv);
    float2 mirrored=abs(offset);
    mirrored=float2(mirrored.x>data[1].z?2*data[1].z-mirrored.x:mirrored.x,
                    mirrored.y>data[1].w?2*data[1].w-mirrored.y:mirrored.y);
    float2 texelScale=(1.0/data[1].zw)*data[1].xy;
    int2 cell=(int2)(mirrored*texelScale);
    float gate=saturate(mad(GateDepth.Load(int3(cell,0)).w,-2.0,-2.599609375));
    if(GateMask.Load(int3(cell,0)).x==0)gate=1.0;
    float2 mapped=mad(gate,mirrored-uv,uv);
    return SceneDepth.SampleLevel(DepthPoint,mapped,0);
})";}
}
