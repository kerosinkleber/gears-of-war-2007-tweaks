// Overlay quads: flat (uv.x < 0) or glyphs from the font atlas (coverage in red); positions
// arrive already in clip space.
Texture2D g_font : register(t0);
SamplerState g_sampler : register(s0);

struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD; float4 color : COLOR; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; float4 color : COLOR; };

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = float4(i.pos, 0, 1);
    o.uv = i.uv;
    o.color = i.color;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float coverage = i.uv.x < 0 ? 1 : g_font.SampleLevel(g_sampler, i.uv, 0).r;
    return float4(i.color.rgb, i.color.a * coverage);
}
