// Flat-colored overlay quads; positions arrive already in clip space.
struct VSIn  { float2 pos : POSITION; float4 color : COLOR; };
struct VSOut { float4 pos : SV_Position; float4 color : COLOR; };

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = float4(i.pos, 0, 1);
    o.color = i.color;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return i.color;
}
