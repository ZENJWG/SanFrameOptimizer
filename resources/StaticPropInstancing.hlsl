// Stock unlit/prelit, single-texture building subset. Other states use GTA's draw.
row_major float4x4 viewProjection : register(c0);
row_major float4x4 view : register(c4);
float4 fogParameters : register(c8); // end, reciprocal span, density, D3DFOGMODE
float4 fogControl : register(c9); // off/vertex/table-Z/table-W, range, minZ, Z span
float4 fogColor : register(c10);

float fogFactor(float depth) {
    if (fogParameters.w == 3) // D3DFOG_LINEAR
        return saturate((fogParameters.x - depth) * fogParameters.y);
    float d = fogParameters.z * depth;
    if (fogParameters.w == 2) d *= d; // D3DFOG_EXP2
    return saturate(exp(-d));
}
struct Input {
    float3 pos : POSITION0;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float4 r0 : TEXCOORD4;
    float4 r1 : TEXCOORD5;
    float4 r2 : TEXCOORD6;
    float4 r3 : TEXCOORD7;
};
struct Output {
    float4 pos : POSITION0;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float3 fog : TEXCOORD1; // clip Z/W, or a per-vertex factor
};
Output VS(Input i) {
    Output o;
    float4 world = i.pos.x * i.r0 + i.pos.y * i.r1 + i.pos.z * i.r2 + i.r3;
    o.pos = mul(world, viewProjection);
    o.color = i.color;
    o.uv = i.uv;
    o.fog = float3(o.pos.z, o.pos.w, 1);
    if (fogControl.x == 1) {
        float3 eye = mul(world, view).xyz;
        o.fog.z = fogFactor(fogControl.y != 0 ? length(eye) : abs(eye.z));
    }
    return o;
}
sampler2D tex : register(s0);
float4 PS(Output i) : COLOR0 {
    float4 color = tex2D(tex, i.uv) * i.color;
    if (fogControl.x != 0) {
        float factor = i.fog.z;
        if (fogControl.x >= 2) {
            // Perspective interpolation of clip W reconstructs 1 / RHW.
            float depth = fogControl.x == 3 ? i.fog.y :
                fogControl.z + (i.fog.x / i.fog.y) * fogControl.w;
            factor = fogFactor(depth);
        }
        color.rgb = lerp(fogColor.rgb, color.rgb, saturate(factor));
    }
    // Fog never changes alpha; native alpha testing remains active.
    return color;
}
