#version 450
// Hexscale contrast-adaptive sharpening (CAS) fragment pass.
// Samples the presented frame through a native-format view (works for RGBA
// and BGRA alike) and writes the sharpened texel into an RGBA8 target. For
// BGRA swapchains the SWAP_RB variant pre-swizzles the output so the final
// verbatim copy lands with correct channel order.
// Inspired by the public AMD FidelityFX CAS algorithm (MIT), re-implemented
// in compact form.

layout(binding = 0) uniform sampler2D inTex;
layout(push_constant) uniform Params {
    float sharpness; // 0.0 = passthrough, 1.0 = maximum
} params;

layout(location = 0) out vec4 outColor;

void main() {
    ivec2 sz = ivec2(textureSize(inTex, 0));
    ivec2 lo = ivec2(0);
    ivec2 hi = sz - 1;
    ivec2 p = clamp(ivec2(gl_FragCoord.xy), lo, hi);

    vec4 e = texelFetch(inTex, p, 0);
    vec4 n = texelFetch(inTex, clamp(p + ivec2(0, -1), lo, hi), 0);
    vec4 s = texelFetch(inTex, clamp(p + ivec2(0, 1), lo, hi), 0);
    vec4 w = texelFetch(inTex, clamp(p + ivec2(-1, 0), lo, hi), 0);
    vec4 ee = texelFetch(inTex, clamp(p + ivec2(1, 0), lo, hi), 0);

    // Local contrast envelope: keep flat gradients smooth, boost edges.
    vec4 mn = min(min(min(n, s), min(w, ee)), e);
    vec4 mx = max(max(max(n, s), max(w, ee)), e);
    vec4 amp = clamp(min(mn, 2.0 - mx) / max(mx, vec4(1e-4)), 0.0, 1.0);
    amp = sqrt(amp);

    vec4 blur = (n + s + w + ee) * 0.25;
    float strength = params.sharpness * params.sharpness * 1.6;
    vec4 result = clamp(e + (e - blur) * strength * amp, 0.0, 1.0);

#ifdef SWAP_RB
    outColor = result.bgra;
#else
    outColor = result;
#endif
}
