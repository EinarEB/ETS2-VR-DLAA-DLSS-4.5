// SPDX-License-Identifier: MIT
// ETS2 DLSS 4.5 guide preparation, extracted from the accepted provider-6,
// matched-depth SBS path in DLSS5_Feed.fx. Embedded as ETS2_DLAA.addonfx;
// the add-on manages its ETS2_VortStereo.addonfx prerequisite automatically.
// Vort's estimator remains a separate CC BY-NC 4.0 effect with its own credits.
// Outputs: current-to-previous motion in render pixels and raw inverted depth.
#include "ReShade.fxh"

#if (BUFFER_WIDTH % 2) != 0
    #error "ETS2 DLAA requires equal-width SBS eyes."
#endif
#if BUFFER_COLOR_SPACE > 1
    #error "ETS2 DLAA requires the accepted SDR color path."
#endif

uniform int ETS2_DLAA_PRESET <
    hidden = true;
>;
// No initializer: retain this live add-on control in Performance Mode. ReShade
// initializes its uniform storage to zero, selecting M until a preset is loaded.

texture ETS2_DLAA_ColorInput : COLOR;
sampler sETS2_DLAA_ColorInput {
    Texture = ETS2_DLAA_ColorInput; AddressU = Clamp; AddressV = Clamp;
    MipFilter = Point; MinFilter = Point; MagFilter = Point;
};
texture2D ETS2_StereoDepthInput : ETS2_STEREO_DEPTH;
sampler sETS2_StereoDepth {
    Texture = ETS2_StereoDepthInput; AddressU = Clamp; AddressV = Clamp;
    MipFilter = Point; MinFilter = Point; MagFilter = Point;
};
// Exact shared output declaration from the accepted provider-6 wrapper.
texture2D ETS2VortMotion { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
sampler sETS2_DLAA_ProviderMV {
    Texture = ETS2VortMotion; AddressU = Clamp; AddressV = Clamp;
    MipFilter = Point; MinFilter = Point; MagFilter = Point;
};

// The add-ons must set these each frame. A missing/currently invalid matched
// pair or invalid provider history produces zero guides and zero guide history.
// No initializers: these live inputs must not become specialization constants.
// Uniform storage starts at zero; the add-on still refreshes validity each frame.
uniform bool ETS2_STEREO_DEPTH_VALID < hidden = true; >;
uniform bool ETS2_MOTION_HISTORY_VALID < hidden = true; >;

// Accepted motion-affecting settings/defaults, retained for preset continuity.
// Mask-only settings are absent: the native SR Owner binds no bias-color mask.
uniform bool MV_VALIDATE < hidden = true; > = true;
uniform bool VALIDATE_STATIC < hidden = true; > = true;
uniform bool STATIC_HYSTERESIS < hidden = true; > = true;
uniform float STATIC_BIAS < hidden = true; > = 0.15;
uniform float STATIC_MIN_CONTRAST < hidden = true; > = 0.012;
uniform bool VALIDATE_DEPTH < hidden = true; > = true;
uniform float DEPTH_TOLERANCE < hidden = true; > = 0.10;
uniform bool VALIDATE_MV < hidden = true; > = true;
uniform float MV_CONSISTENCY < hidden = true; > = 1.4;
uniform float2 MV_SIGN < hidden = true; > = float2(1.0, 1.0);
uniform float MV_SCALE < hidden = true; > = 1.0;

texture ETS2_DLAA_Motion { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
texture ETS2_DLAA_Depth { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R32F; };
texture ETS2_DLAA_PrevLuma { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R16F; };
texture ETS2_DLAA_PrevDepth { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R16F; };
texture ETS2_DLAA_PrevMV { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
texture ETS2_DLAA_StaticNow { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R8; };
texture ETS2_DLAA_PrevStatic { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R8; };
// Private verification only: retain the exact rejection scores before history
// storage changes the previous-frame inputs used by ValidateTests.
texture ETS2_DLAA_DiagnosticTests { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
sampler sETS2_DLAA_PrevLuma {
    Texture = ETS2_DLAA_PrevLuma; AddressU = Clamp; AddressV = Clamp;
    MinFilter = Linear; MagFilter = Linear; MipFilter = Point;
};
sampler sETS2_DLAA_PrevDepth {
    Texture = ETS2_DLAA_PrevDepth; AddressU = Clamp; AddressV = Clamp;
    MinFilter = Point; MagFilter = Point; MipFilter = Point;
};
sampler sETS2_DLAA_PrevMV {
    Texture = ETS2_DLAA_PrevMV; AddressU = Clamp; AddressV = Clamp;
    MinFilter = Point; MagFilter = Point; MipFilter = Point;
};
sampler sETS2_DLAA_StaticNow {
    Texture = ETS2_DLAA_StaticNow; AddressU = Clamp; AddressV = Clamp;
    MinFilter = Point; MagFilter = Point; MipFilter = Point;
};
sampler sETS2_DLAA_PrevStatic {
    Texture = ETS2_DLAA_PrevStatic; AddressU = Clamp; AddressV = Clamp;
    MinFilter = Point; MagFilter = Point; MipFilter = Point;
};

float MatchedRawDepth(float2 uv)
{
    return ETS2_STEREO_DEPTH_VALID ? tex2Dlod(sETS2_StereoDepth, float4(uv, 0.0, 0.0)).x : 0.0;
}
float LinearDepth(float2 uv)
{
    float depth = MatchedRawDepth(uv);
    depth = 1.0 - depth;
    // Legacy history encoding only. It is not a predicted previous-camera Z
    // and must never be compared with current Z to reject a correspondence.
    // The NGX depth output remains the unchanged raw depth.
    return depth / (1000.0 - depth * (1000.0 - 1.0));
}
float2 SbsSampleUV(float2 sampleUV, float2 sourceUV, float2 texelSize)
{
    float start = sourceUV.x >= 0.5 ? 0.5 : 0.0;
    return clamp(sampleUV, float2(start, 0.0) + 0.5 * texelSize,
                 float2(start + 0.5, 1.0) - 0.5 * texelSize);
}
bool SbsInvalidVector(float2 uv, float2 mv)
{
    if (!all(abs(mv) < 1e6)) return true;
    const float2 target = uv + mv;
    if (any(target < 0.0) || any(target > 1.0)) return true;
    return uv.x < 0.5 ? target.x >= 0.5 : target.x < 0.5;
}
float2 ProviderSampleUV(float2 uv)
{
    float2 halfTexel = 0.5 / float2(tex2Dsize(sETS2_DLAA_ProviderMV, 0));
    float eyeStart = uv.x >= 0.5 ? 0.5 : 0.0;
    return clamp(uv, float2(eyeStart, 0.0) + halfTexel, float2(eyeStart + 0.5, 1.0) - halfTexel);
}
float2 ProviderMV(float2 uv)
{
    float4 c = float4(ProviderSampleUV(uv), 0.0, 0.0);
    return tex2Dlod(sETS2_DLAA_ProviderMV, c).xy;
}
float Luma(float2 uv)
{
    return dot(tex2Dlod(sETS2_DLAA_ColorInput, float4(uv, 0.0, 0.0)).rgb, float3(0.299, 0.587, 0.114));
}
float PatchError(float2 uv_cur, float2 uv_prev, out float contrast)
{
    const float2 px = BUFFER_PIXEL_SIZE;
    float c[9], p[9];
    float mc = 0.0, mp = 0.0;
    [unroll] for (int i = 0; i < 9; ++i)
    {
        const float2 o = float2(i % 3 - 1, i / 3 - 1) * px;
        c[i] = Luma(SbsSampleUV(uv_cur + o, uv_cur, px));
        p[i] = tex2Dlod(sETS2_DLAA_PrevLuma, float4(SbsSampleUV(uv_prev + o, uv_cur, px), 0.0, 0.0)).x;
        mc += c[i]; mp += p[i];
    }
    mc /= 9.0; mp /= 9.0;
    float err = 0.0;
    contrast = 0.0;
    [unroll] for (int j = 0; j < 9; ++j)
    {
        err += abs((c[j] - mc) - (p[j] - mp));
        contrast += abs(c[j] - mc);
    }
    contrast /= 9.0;
    return err / 9.0;
}

// y: invalid current raw depth, z: observed image-correspondence mismatch,
// w: static patch wins over the proposed correspondence. Previous-camera depth
// and previous-frame velocity do not predict either quantity for this frame.
float4 ValidateTests(float2 uv, float2 mv)
{
    const float2 puv = uv + mv;
    if (SbsInvalidVector(uv, mv)) return float4(1.0, 1.0, 1.0, 0.0);
    float4 bad = 0.0;
    float contrast = 0.0;
    float flow_error = 0.0;
    if (VALIDATE_STATIC || (VALIDATE_MV && MV_CONSISTENCY > 0.0))
        flow_error = PatchError(uv, puv, contrast);
    if (VALIDATE_STATIC && length(mv * BUFFER_SCREEN_SIZE) > 0.5)
    {
        float sc;
        const float es = PatchError(uv, uv, sc);
        if (sc >= STATIC_MIN_CONTRAST)
            bad.w = es + 0.25 * sc <= flow_error * (1.0 + STATIC_BIAS) ? 1.0 : 0.0;
    }
    if (VALIDATE_DEPTH)
    {
        const float raw_depth = MatchedRawDepth(uv);
        bad.y = raw_depth >= 0.0 && raw_depth <= 1.0 ? 0.0 : 1.0;
    }
    if (VALIDATE_MV && MV_CONSISTENCY > 0.0 && contrast >= STATIC_MIN_CONTRAST)
    {
        // Mean removal tolerates an additive brightness offset, not exposure
        // gain or local contrast changes. This score is diagnostic only.
        // A patch mismatch cannot establish that the surface is stationary.
        // The floor
        // covers 8-bit input/half-history rounding; the remaining allowance is
        // relative to measured patch contrast, not a guessed depth or velocity.
        // A textureless patch supplies no evidence for this rejection.
        const float allowance = 0.35 * contrast + 2.0 / 255.0;
        // When the static hypothesis wins, leave its existing two-frame
        // hysteresis in charge instead of bypassing it through this score.
        bad.z = bad.w > 0.5 ? 0.0 : saturate((flow_error - allowance) / allowance);
    }
    return bad;
}

void PS_MotionVectors(float4 vpos : SV_Position, float2 uv : TEXCOORD,
                      out float2 mv_out : SV_Target0, out float depth : SV_Target1,
                      out float static_now : SV_Target2, out float4 diagnostic_tests : SV_Target3)
{
    diagnostic_tests = 0.0;
    if (!ETS2_STEREO_DEPTH_VALID || !ETS2_MOTION_HISTORY_VALID) {
        mv_out = 0.0; depth = 0.0; static_now = 0.0; return;
    }
    const float2 flow = ProviderMV(uv);
    float2 mv = flow;
    static_now = 0.0;
    if (SbsInvalidVector(uv, flow)) {
        diagnostic_tests = float4(1.0, 1.0, 1.0, 0.0);
        mv_out = 0.0; depth = MatchedRawDepth(uv); return;
    }
    // The original SBS branch always disables the whole-frame geometry model.
    if (MV_VALIDATE)
    {
        const float4 bad = ValidateTests(uv, flow);
        diagnostic_tests = bad;
        static_now = bad.w;
        float static_zero = bad.w;
        if (STATIC_HYSTERESIS && bad.w > 0.5)
        {
            const float won_before = tex2Dlod(sETS2_DLAA_PrevStatic, float4(SbsSampleUV(uv, uv, BUFFER_PIXEL_SIZE), 0.0, 0.0)).x;
            if (won_before <= 0.5) static_zero = 0.0;
        }
        const bool zero_vector = max(bad.y, static_zero) > 0.5;
        mv = zero_vector ? float2(0.0, 0.0) : flow;
    }
    // Provider already converted each eye UV vector to combined UV once.
    // This is the accepted combined-UV -> render-pixel conversion, unchanged.
    mv_out = mv * float2(BUFFER_WIDTH, BUFFER_HEIGHT) * MV_SIGN * MV_SCALE;
    depth = MatchedRawDepth(uv);
    if (SbsInvalidVector(uv, mv * MV_SIGN * MV_SCALE)) {
        mv_out = 0.0; static_now = 0.0;
    }
}

// Store raw provider vectors, never the validated output, exactly as before.
// Keep the separate StaticNow/PrevStatic surfaces for the two-frame decision.
void PS_StoreHistory(float4 vpos : SV_Position, float2 uv : TEXCOORD,
                     out float luma : SV_Target0, out float depth : SV_Target1,
                     out float2 mv : SV_Target2, out float prev_static : SV_Target3)
{
    if (!ETS2_STEREO_DEPTH_VALID || !ETS2_MOTION_HISTORY_VALID) {
        luma = 0.0; depth = 0.0; mv = 0.0; prev_static = 0.0; return;
    }
    luma = Luma(uv);
    depth = LinearDepth(uv);
    mv = ProviderMV(uv);
    if (SbsInvalidVector(uv, mv)) mv = 0.0;
    prev_static = tex2Dfetch(sETS2_DLAA_StaticNow, int2(vpos.xy)).x;
}

technique ETS2_DLAA
<
    hidden = true;
    ui_label = "ETS2 DLAA";
    ui_tooltip = "Enable native stereo DLAA. Stereo motion and matched depth are managed automatically by the add-on.";
>
{
    pass Guides {
        VertexShader = PostProcessVS; PixelShader = PS_MotionVectors;
        RenderTarget0 = ETS2_DLAA_Motion; RenderTarget1 = ETS2_DLAA_Depth;
        RenderTarget2 = ETS2_DLAA_StaticNow;
        RenderTarget3 = ETS2_DLAA_DiagnosticTests;
    }
    pass History {
        VertexShader = PostProcessVS; PixelShader = PS_StoreHistory;
        RenderTarget0 = ETS2_DLAA_PrevLuma; RenderTarget1 = ETS2_DLAA_PrevDepth;
        RenderTarget2 = ETS2_DLAA_PrevMV; RenderTarget3 = ETS2_DLAA_PrevStatic;
    }
}
