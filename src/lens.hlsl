// Lens warp: maps each output pixel to a view ray through the chosen lens, then samples the
// engine's wide rectilinear render along that ray. Math follows shaunlebron/flex-fov.
// Ray convention: x right, y up, z forward. Screen x spans [-1, 1], y spans [-1/aspect, 1/aspect].

cbuffer Lens : register(b0)
{
    float halfFov;        // half of the lens' horizontal FOV, radians
    float tanHalfRender;  // tan(half of the engine's horizontal render FOV)
    float aspect;         // width / height
    float blend;          // mode 1: 0 = rectilinear, 1 = Panini
    int mode;             // 1 = auto (rectilinear -> Panini), 2 = Panini, 3 = stereographic, 4/5 = fisheye
    float circle;         // mode 5: the circle's half-angle (radians); black beyond it
    float2 pad;
    float4 src;           // the warped area in the frame (uv offset, uv size): all of it, or one view
};

Texture2D Scene : register(t0);
SamplerState Linear : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID)
{
    // One triangle covering the screen.
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

float3 RayRectilinear(float2 s)
{
    return normalize(float3(s * tan(min(halfFov, 1.5690)), 1));
}

float3 RayPanini(float2 s)
{
    // Panini with d = 1: x = S sin(lon), y = S tan(lat), S = 2 / (1 + cos(lon)).
    float edge = 2 * sin(halfFov) / (1 + cos(halfFov));
    float2 p = s * edge;
    float k = p.x * p.x / 4;
    float clon = (1 - k) / (1 + k);
    float S = 1 + k;
    float lon = atan2(p.x, S * clon);
    float lat = atan2(p.y, S);
    return float3(cos(lat) * sin(lon), sin(lat), cos(lat) * cos(lon));
}

float3 RayStereographic(float2 s)
{
    // r = tan(theta / 2)
    float2 p = s * tan(halfFov * 0.5);
    float r = length(p);
    if (r < 1e-6) return float3(0, 0, 1);
    float theta = 2 * atan(r);
    return float3(p / r * sin(theta), cos(theta));
}

float3 RayFisheye(float2 s)
{
    // Equidistant: theta = r. Corners past what was rendered are left black.
    float2 p = s * halfFov;
    float r = length(p);
    if (r < 1e-6) return float3(0, 0, 1);
    return float3(p / r * sin(r), cos(r));
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 s = float2(i.uv.x * 2 - 1, (1 - i.uv.y * 2) / aspect);
    float3 ray;
    if (mode >= 4) ray = RayFisheye(s);
    else if (mode == 3) ray = RayStereographic(s);
    else if (mode == 2) ray = RayPanini(s);
    else ray = normalize(lerp(RayRectilinear(s), RayPanini(s), blend));

    if (ray.z <= 1e-4) return float4(0, 0, 0, 1);
    if (circle > 0 && length(s) * halfFov > circle) return float4(0, 0, 0, 1);  // outside the full circle
    float2 r = ray.xy / ray.z / float2(tanHalfRender, tanHalfRender / aspect);
    if (abs(r.x) > 1.001 || abs(r.y) > 1.001) return float4(0, 0, 0, 1);
    return Scene.SampleLevel(Linear, src.xy + src.zw * float2(0.5 + 0.5 * r.x, 0.5 - 0.5 * r.y), 0);
}
