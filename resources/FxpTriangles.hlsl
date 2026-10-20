row_major float4x4 world : register(c0);
row_major float4x4 view : register(c4);
row_major float4x4 projection : register(c8);
float4 fog : register(c12); // start, end, density, mode (0/linear/exp/exp2)
float4 options : register(c13); // range fog, clamp per vertex (DXVK)
float FogFactor(float d, float4 parameters) {
 float f=1;
 if(parameters.w>2.5) f=exp(-parameters.z*parameters.z*d*d);
 else if(parameters.w>1.5) f=exp(-parameters.z*d);
 else if(parameters.w>0.5) f=(parameters.y-d)/(parameters.y-parameters.x);
 return f;
}
struct Input {
 float corner:POSITION0;
 float3 p0:TEXCOORD0; float3 p1:TEXCOORD1; float3 p2:TEXCOORD2;
 float4 uv01:TEXCOORD3; float2 uv2:TEXCOORD4;
 float4 color0:TEXCOORD5; float4 color1:TEXCOORD6; float4 color2:TEXCOORD7;
};
struct Output { float4 position:POSITION; float2 uv:TEXCOORD0; float4 color:COLOR0; float fog:TEXCOORD1; float2 clipDepth:TEXCOORD2; };
Output VS(Input i) {
 Output o;
 float3 p=i.corner<0.5?i.p0:(i.corner<1.5?i.p1:i.p2);
 o.uv=i.corner<0.5?i.uv01.xy:(i.corner<1.5?i.uv01.zw:i.uv2);
 o.color=i.corner<0.5?i.color0:(i.corner<1.5?i.color1:i.color2);
 float4 v=mul(mul(float4(p,1),world),view);
 o.position=mul(v,projection);
 float d=options.x>0.5?length(v.xyz):abs(v.z);
 o.fog=FogFactor(d,fog);
 if(options.y>0.5) o.fog=saturate(o.fog);
 o.clipDepth=o.position.zw; return o;
}
sampler2D image : register(s0);
float4 fogColor : register(c0);
float4 textureModes : register(c1); // RGB/alpha: 0=diffuse, 1=texture, 2=modulate
float4 pixelFog : register(c2);
float4 pixelOptions : register(c3); // table enabled, W-fog, viewport minZ, depth range
float4 PS(Output i):COLOR {
 float4 t=tex2D(image,i.uv);
 float4 c=i.color;
 if(textureModes.x>1.5) c.rgb*=t.rgb; else if(textureModes.x>0.5) c.rgb=t.rgb;
 if(textureModes.y>1.5) c.a*=t.a; else if(textureModes.y>0.5) c.a=t.a;
 float f=i.fog;
 if(pixelOptions.x>0.5) {
  float d=pixelOptions.y>0.5?i.clipDepth.y:(i.clipDepth.x/i.clipDepth.y)*pixelOptions.w+pixelOptions.z;
  f=FogFactor(d,pixelFog);
 }
 c.rgb=lerp(fogColor.rgb,c.rgb,saturate(f)); return c;
}
