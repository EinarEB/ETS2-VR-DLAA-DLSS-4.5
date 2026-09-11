// Copyright (c) 2026 ETS2 VR preview contributors. Wrapper: MIT.
// Estimator included below: CC BY-NC 4.0, credited in ETS2_VortStereo_Eye.fxh.
// Math/noise helpers adapted from Vort's MIT-licensed vort_Defs.fxh.
// Embedded as ETS2_VortStereo.addonfx; activation/order are owned by the add-on.
#include "ReShade.fxh"
#if BUFFER_WIDTH % 2
#error "ETS2 stereo motion requires two equal-width eyes."
#endif
#if BUFFER_WIDTH < 512 || BUFFER_HEIGHT < 256
#error "ETS2 stereo motion requires at least 256 by 256 pixels per eye."
#endif
#if BUFFER_COLOR_SPACE > 1
#error "This preview's motion provider supports the tested SDR color path only."
#endif
uniform uint ETS2_VORT_FRAME < source = "framecount"; >;
// No initializers: retain live add-on inputs in Performance Mode (default zero).
uniform bool ETS2_STEREO_DEPTH_VALID < hidden = true; >;
uniform bool ETS2_VORT_RESET < hidden = true; >;
texture2D ETS2VortColor : COLOR;
sampler2D sETS2VortColor { Texture = ETS2VortColor; AddressU = Clamp; AddressV = Clamp; MinFilter = Point; MagFilter = Point; MipFilter = Point; };
texture2D ETS2VortDepth : ETS2_STEREO_DEPTH;
sampler2D sETS2VortDepth { Texture = ETS2VortDepth; AddressU = Clamp; AddressV = Clamp; MinFilter = Point; MagFilter = Point; MipFilter = Point; };
texture2D ETS2VortNoise < source = "vort_BlueNoise.png"; > { Width = 256; Height = 256; Format = RGBA8; };
sampler2D sETS2VortNoise { Texture = ETS2VortNoise; AddressU = Wrap; AddressV = Wrap; MinFilter = Point; MagFilter = Point; MipFilter = Point; };
texture2D ETS2VortMotion { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
// Private verification surfaces: preserve the finest current/previous features
// before the normal history copies overwrite the previous frame.
texture2D ETS2VortDiagnosticCurrent { Width = (BUFFER_WIDTH/4)*2; Height = BUFFER_HEIGHT/2; Format = RG16F; };
texture2D ETS2VortDiagnosticPrevious { Width = (BUFFER_WIDTH/4)*2; Height = BUFFER_HEIGHT/2; Format = RG16F; };
struct VSOUT { float4 vpos : SV_POSITION; float2 uv : TEXCOORD0; };
#define PS_ARGS2 in VSOUT i, out float2 o : SV_Target0
#define PS_ARGS4 in VSOUT i, out float4 o : SV_Target0
#define TEX_SIZE(bit) Width = (BUFFER_WIDTH/2) >> bit; Height = BUFFER_HEIGHT >> bit;
#define TEX_RG16 Format = RG16F;
#define TEX_RGBA16 Format = RGBA16F;
#define SAM_POINT MinFilter = Point; MagFilter = Point; MipFilter = Point;
#define IS_DX9 0
#define IS_SRGB 1
#define POW(x,y) pow(abs(x),y)
#define A_THIRD (1.0/3.0)
#define HALF_PI 1.5707963267948966
#define DOUBLE_PI 6.283185307179586
#if BUFFER_HEIGHT < 2160
#define MAX_MIP 8
#else
#define MAX_MIP 9
#endif
float4 Sample(sampler2D s,float2 uv) { return tex2Dlod(s,float4(uv,0,0)); }
float Min2(float2 v) { return min(v.x,v.y); }
float4 GetBlueNoise(float2 pixel) { return tex2Dfetch(sETS2VortNoise,uint2(pixel)%256u); }
float GetR1(float seed,uint mod) { return frac(seed+float(ETS2_VORT_FRAME%max(2u,mod))*0.38196601125); }
float4 GetRotator(float radians) { float2 sc=0.0;sincos(radians,sc.x,sc.y);return float4(sc.y,sc.x,-sc.x,sc.y); }
float2 Rotate(float2 v,float4 rotation) { return float2(dot(v,rotation.xy),dot(v,rotation.zw)); }
bool ValidateUV(float2 uv) { return all(uv>=0.0)&&all(uv<=1.0); }
namespace ETS2VortLeft {
    static const uint EYE_INDEX=0;
    #include "ETS2_VortStereo_Eye.fxh"
}
namespace ETS2VortRight {
    static const uint EYE_INDEX=1;
    #include "ETS2_VortStereo_Eye.fxh"
}
float2 PS_Stitch(in VSOUT i) : SV_Target0 {
    bool right=i.uv.x>=0.5;
    float2 local=float2(frac(i.uv.x*2.0),i.uv.y);
    float2 mv=right ? tex2Dlod(ETS2VortRight::sEyeMotion,float4(local,0,0)).xy : tex2Dlod(ETS2VortLeft::sEyeMotion,float4(local,0,0)).xy;
    // Inspect IEEE exponent bits; fast-math compilation can remove isnan().
    if(any((asuint(mv)&0x7fffffffu)>=0x7f800000u)||!ValidateUV(local+mv))return 0.0;
    return mv*float2(0.5,1.0); // Eye UV -> combined UV, exactly once.
}
void PS_DiagnosticFeatures(in VSOUT i, out float2 current : SV_Target0, out float2 previous : SV_Target1) {
    int2 p=int2(i.vpos.xy);
    bool right=p.x >= BUFFER_WIDTH/4;
    if(right)p.x-=BUFFER_WIDTH/4;
    current=right ? tex2Dfetch(ETS2VortRight::sCurrFeatTex1,p).xy : tex2Dfetch(ETS2VortLeft::sCurrFeatTex1,p).xy;
    previous=right ? tex2Dfetch(ETS2VortRight::sPrevFeatTex1,p).xy : tex2Dfetch(ETS2VortLeft::sPrevFeatTex1,p).xy;
}
technique ETS2_VortStereo < hidden=true; ui_label="ETS2 stereo motion (Vort)"; ui_tooltip="Internal motion prerequisite managed by ETS2 DLAA."; > {
    pass LFeature1 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_WriteFeat; RenderTarget=ETS2VortLeft::CurrFeatTex1; }
    pass LFeature2 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_DownFeat2; RenderTarget=ETS2VortLeft::CurrFeatTex2; }
    pass LFeature3 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_DownFeat3; RenderTarget=ETS2VortLeft::CurrFeatTex3; }
    pass LFeature4 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_DownFeat4; RenderTarget=ETS2VortLeft::CurrFeatTex4; }
    pass LFeature5 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_DownFeat5; RenderTarget=ETS2VortLeft::CurrFeatTex5; }
    pass LFeature6 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_DownFeat6; RenderTarget=ETS2VortLeft::CurrFeatTex6; }
    pass LFeature7 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_DownFeat7; RenderTarget=ETS2VortLeft::CurrFeatTex7; }
    pass LFeature8 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_DownFeat8; RenderTarget=ETS2VortLeft::CurrFeatTex8; }
#if BUFFER_HEIGHT >= 2160
    pass LMotion9 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Motion9; RenderTarget=ETS2VortLeft::MotionTexA; }
    pass LFilter8 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Filter8; RenderTarget=ETS2VortLeft::MotionTexB; }
#endif
    pass LMotion8 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Motion8; RenderTarget=ETS2VortLeft::MotionTexA; }
    pass LFilter7 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Filter7; RenderTarget=ETS2VortLeft::MotionTexB; }
    pass LMotion7 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Motion7; RenderTarget=ETS2VortLeft::MotionTexA; }
    pass LFilter6 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Filter6; RenderTarget=ETS2VortLeft::MotionTexB; }
    pass LMotion6 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Motion6; RenderTarget=ETS2VortLeft::MotionTexA; }
    pass LFilter5 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Filter5; RenderTarget=ETS2VortLeft::MotionTexB; }
    pass LMotion5 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Motion5; RenderTarget=ETS2VortLeft::MotionTexA; }
    pass LFilter4 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Filter4; RenderTarget=ETS2VortLeft::MotionTexB; }
    pass LMotion4 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Motion4; RenderTarget=ETS2VortLeft::MotionTexA; }
    pass LFilter3 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Filter3; RenderTarget=ETS2VortLeft::MotionTexB; }
    pass LMotion3 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Motion3; RenderTarget=ETS2VortLeft::MotionTexA; }
    pass LFilter2 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Filter2; RenderTarget=ETS2VortLeft::MotionTexB; }
    pass LMotion2 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Motion2; RenderTarget=ETS2VortLeft::MotionTex2; }
    pass LMotion1 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Motion1; RenderTarget=ETS2VortLeft::MotionTex1; }
    pass LFinal { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_Filter0; RenderTarget=ETS2VortLeft::EyeMotion; }
    pass RFeature1 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_WriteFeat; RenderTarget=ETS2VortRight::CurrFeatTex1; }
    pass RFeature2 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_DownFeat2; RenderTarget=ETS2VortRight::CurrFeatTex2; }
    pass RFeature3 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_DownFeat3; RenderTarget=ETS2VortRight::CurrFeatTex3; }
    pass RFeature4 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_DownFeat4; RenderTarget=ETS2VortRight::CurrFeatTex4; }
    pass RFeature5 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_DownFeat5; RenderTarget=ETS2VortRight::CurrFeatTex5; }
    pass RFeature6 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_DownFeat6; RenderTarget=ETS2VortRight::CurrFeatTex6; }
    pass RFeature7 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_DownFeat7; RenderTarget=ETS2VortRight::CurrFeatTex7; }
    pass RFeature8 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_DownFeat8; RenderTarget=ETS2VortRight::CurrFeatTex8; }
#if BUFFER_HEIGHT >= 2160
    pass RMotion9 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Motion9; RenderTarget=ETS2VortRight::MotionTexA; }
    pass RFilter8 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Filter8; RenderTarget=ETS2VortRight::MotionTexB; }
#endif
    pass RMotion8 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Motion8; RenderTarget=ETS2VortRight::MotionTexA; }
    pass RFilter7 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Filter7; RenderTarget=ETS2VortRight::MotionTexB; }
    pass RMotion7 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Motion7; RenderTarget=ETS2VortRight::MotionTexA; }
    pass RFilter6 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Filter6; RenderTarget=ETS2VortRight::MotionTexB; }
    pass RMotion6 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Motion6; RenderTarget=ETS2VortRight::MotionTexA; }
    pass RFilter5 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Filter5; RenderTarget=ETS2VortRight::MotionTexB; }
    pass RMotion5 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Motion5; RenderTarget=ETS2VortRight::MotionTexA; }
    pass RFilter4 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Filter4; RenderTarget=ETS2VortRight::MotionTexB; }
    pass RMotion4 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Motion4; RenderTarget=ETS2VortRight::MotionTexA; }
    pass RFilter3 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Filter3; RenderTarget=ETS2VortRight::MotionTexB; }
    pass RMotion3 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Motion3; RenderTarget=ETS2VortRight::MotionTexA; }
    pass RFilter2 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Filter2; RenderTarget=ETS2VortRight::MotionTexB; }
    pass RMotion2 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Motion2; RenderTarget=ETS2VortRight::MotionTex2; }
    pass RMotion1 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Motion1; RenderTarget=ETS2VortRight::MotionTex1; }
    pass RFinal { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_Filter0; RenderTarget=ETS2VortRight::EyeMotion; }
    pass Stitch { VertexShader=PostProcessVS; PixelShader=PS_Stitch; RenderTarget=ETS2VortMotion; }
    pass DiagnosticFeatures { VertexShader=PostProcessVS; PixelShader=PS_DiagnosticFeatures; RenderTarget0=ETS2VortDiagnosticCurrent; RenderTarget1=ETS2VortDiagnosticPrevious; }
    pass LHistory1 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_CopyFeat1; RenderTarget=ETS2VortLeft::PrevFeatTex1; }
    pass LHistory2 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_CopyFeat2; RenderTarget=ETS2VortLeft::PrevFeatTex2; }
    pass LHistory3 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_CopyFeat3; RenderTarget=ETS2VortLeft::PrevFeatTex3; }
    pass LHistory4 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_CopyFeat4; RenderTarget=ETS2VortLeft::PrevFeatTex4; }
    pass LHistory5 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_CopyFeat5; RenderTarget=ETS2VortLeft::PrevFeatTex5; }
    pass LHistory6 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_CopyFeat6; RenderTarget=ETS2VortLeft::PrevFeatTex6; }
    pass LHistory7 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_CopyFeat7; RenderTarget=ETS2VortLeft::PrevFeatTex7; }
    pass LHistory8 { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_CopyFeat8; RenderTarget=ETS2VortLeft::PrevFeatTex8; }
    pass LHistoryState { VertexShader=PostProcessVS; PixelShader=ETS2VortLeft::PS_SaveState; RenderTarget=ETS2VortLeft::HistoryState; }
    pass RHistory1 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_CopyFeat1; RenderTarget=ETS2VortRight::PrevFeatTex1; }
    pass RHistory2 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_CopyFeat2; RenderTarget=ETS2VortRight::PrevFeatTex2; }
    pass RHistory3 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_CopyFeat3; RenderTarget=ETS2VortRight::PrevFeatTex3; }
    pass RHistory4 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_CopyFeat4; RenderTarget=ETS2VortRight::PrevFeatTex4; }
    pass RHistory5 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_CopyFeat5; RenderTarget=ETS2VortRight::PrevFeatTex5; }
    pass RHistory6 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_CopyFeat6; RenderTarget=ETS2VortRight::PrevFeatTex6; }
    pass RHistory7 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_CopyFeat7; RenderTarget=ETS2VortRight::PrevFeatTex7; }
    pass RHistory8 { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_CopyFeat8; RenderTarget=ETS2VortRight::PrevFeatTex8; }
    pass RHistoryState { VertexShader=PostProcessVS; PixelShader=ETS2VortRight::PS_SaveState; RenderTarget=ETS2VortRight::HistoryState; }
}
