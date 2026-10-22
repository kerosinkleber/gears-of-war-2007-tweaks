// Supersampling resolve: exact area average of the high-resolution frame into the native
// back buffer, done in linear light so bright/dark edges keep their weight.

cbuffer Downscale : register(b0)
{
    float2 scale;    // source pixels per destination pixel (per axis)
    float2 srcMax;   // source size - 1
};

Texture2D Src : register(t0);

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 ToLinear(float3 c) { return pow(max(c, 0), 2.2); }
float3 ToGamma(float3 c) { return pow(max(c, 0), 1 / 2.2); }

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    // Destination pixel x covers source span [x * scale, (x + 1) * scale).
    float2 p0 = (pos.xy - 0.5) * scale;
    float2 p1 = p0 + scale;
    int2 i0 = (int2)floor(p0);
    int2 i1 = (int2)ceil(p1) - 1;

    float3 acc = 0;
    float wsum = 0;
    [loop] for (int y = i0.y; y <= i1.y; ++y) {
        float wy = min(p1.y, y + 1) - max(p0.y, y);
        [loop] for (int x = i0.x; x <= i1.x; ++x) {
            float wx = min(p1.x, x + 1) - max(p0.x, x);
            int2 t = min(int2(x, y), (int2)srcMax);
            acc += ToLinear(Src.Load(int3(t, 0)).rgb) * (wx * wy);
            wsum += wx * wy;
        }
    }
    return float4(ToGamma(acc / wsum), 1);
}
