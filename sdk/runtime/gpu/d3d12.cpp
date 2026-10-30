// The title's Direct3D 8 calls drawn with Direct3D 12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <chrono>
#include <memory>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <set>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/ext.h"
#include "gpu/interp.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

constexpr int kFrames = 3;
constexpr uint64_t kStaleFrames = 8;  // frames a target may go unused and still be read back
constexpr uint64_t kUploadSize = 16ull << 20;
// a draw starting with less than this left in the frame's ring waits for the GPU and starts the ring over
constexpr uint64_t kUploadReserve = 4ull << 20;
constexpr uint32_t kSrvCount = 3 * 32768;
constexpr uint32_t kSamplerCount = 2048;
constexpr uint32_t kRtvCount = 1024;
constexpr uint32_t kDsvCount = 256;
constexpr DXGI_FORMAT kBgra = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr uint32_t kQueries = 8192;  // occlusion queries in flight, several frames of counted draws
constexpr uint32_t kLowProfileAniso = 4;  // the low graphics profile's most anisotropic filtering
// the scene's samples a pixel as set: none in the low graphics profile
// and the final picture's edges smoothed after, with it or alone: FXAA (PostAa) or by the edges' shapes (SmaaOn)
uint32_t SceneMsaa() { return app::Get().low_profile || app::Get().msaa < 2 ? 0 : app::Get().msaa; }
bool PostAa() { return app::Get().edge_aa == 1; }
bool SmaaOn() { return app::Get().edge_aa == 2; }
// frame interpolation: frames shown for each the title draws, at most; draws a frame whose constants it blends
constexpr uint32_t kInterpMax = 4;
constexpr uint32_t kInterpDraws = 2048;
constexpr uint64_t kVscBytes = 192 * 16;  // a draw's vertex constants, a multiple of 256
constexpr uint64_t kInterpBlock = kInterpDraws * kVscBytes;
// the registers blended: from slot 60 of the 192 (c-36), after the Future Perfect recompilation (lmplied/TSFPRecomp): the projection, the
// model view, their product, another model view
constexpr uint32_t kInterpFirst = 60;
constexpr uint32_t kNoQuery = ~0u;

// perf diagnostic: where a frame's time goes, logged once a second with diag::Perf
using PerfClock = std::chrono::steady_clock;
double PerfMs(PerfClock::time_point t) {
    return std::chrono::duration<double, std::milli>(PerfClock::now() - t).count();
}
struct Perf {
    // milliseconds
    double draw = 0;
    double assemble = 0;
    double bind = 0;
    double sig = 0;
    double texup = 0;
    double compile = 0;
    double clear = 0;
    double copy = 0;
    double present = 0;
    double wait = 0;
    double swap = 0;
    double sync = 0;
    double gpu = 0;  // the GPU's own time on the frame's command lists, XBE_PERF only
    // counts and bytes
    uint64_t draws = 0;
    uint64_t verts = 0;
    uint64_t upload = 0;
    uint64_t overflow = 0;
    uint64_t sigs = 0;
    uint64_t sig_bytes = 0;
    uint64_t texups = 0;
    uint64_t texup_bytes = 0;
    uint64_t newtex = 0;
    uint64_t targets = 0;
    uint64_t rtups = 0;
    uint64_t shrinks = 0;
    uint64_t syncs = 0;
    uint64_t psos = 0;
    uint64_t shaders = 0;
    uint64_t raw = 0;
    uint64_t lates = 0;
    // the GPU's time by the kind of work (GpuPass), and the draws of each
    double pass_ms[7] = {};
    uint64_t pass_draws[7] = {};
    // the same by target (TargetTag): milliseconds and draws
    std::unordered_map<uint64_t, std::pair<double, uint64_t>> by_target;
};
Perf g_perf;

// what the GPU is working on, for the measurement's split of its time: draws into the scene's multisampled targets,
// full size single sampled ones, the smaller ones (shadows, blur, cameras), MSAA resolves and copies made for reading,
// the game's surface copies (the front buffer's among them), and the picture put on screen with the overlay
enum class GpuPass : uint8_t { Scene, Full, Small, Resolve, Copy, Final, Other };
constexpr const char* kGpuPassNames[] = {"scene", "full", "small", "resolve", "copy", "final", "other"};
// timed only while the measurement is on (F9): the clock is read many times a draw
struct PerfScope {
    double& acc;
    bool on = diag::Perf();
    PerfClock::time_point t = on ? PerfClock::now() : PerfClock::time_point{};
    ~PerfScope() {
        if (on) acc += PerfMs(t);
    }
};

// F3 captures only with diag::CaptureDir set
bool CaptureOn() {
    if (diag::CaptureDir().empty()) LOG_FIRST(1, "capture: set XBE_CAPTURE_DIR");
    return !diag::CaptureDir().empty();
}

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; float4 opts2; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
// Catmull-Rom over 4 x 4 texels in nine bilinear taps: an upscaled picture stays sharp
float4 cubic(float2 uv) {
    float2 size;
    img.GetDimensions(size.x, size.y);
    float2 pos = uv * size, c = floor(pos - 0.5) + 0.5, f = pos - c;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f), w12 = w1 + w2;
    float2 t0 = (c - 1) / size, t3 = (c + 2) / size, t12 = (c + w2 / w12) / size;
    float4 r = 0;
    r += (img.SampleLevel(ls, float2(t0.x, t0.y), 0) * w0.x + img.SampleLevel(ls, float2(t12.x, t0.y), 0) * w12.x +
          img.SampleLevel(ls, float2(t3.x, t0.y), 0) * w3.x) * w0.y;
    r += (img.SampleLevel(ls, float2(t0.x, t12.y), 0) * w0.x + img.SampleLevel(ls, float2(t12.x, t12.y), 0) * w12.x +
          img.SampleLevel(ls, float2(t3.x, t12.y), 0) * w3.x) * w12.y;
    r += (img.SampleLevel(ls, float2(t0.x, t3.y), 0) * w0.x + img.SampleLevel(ls, float2(t12.x, t3.y), 0) * w12.x +
          img.SampleLevel(ls, float2(t3.x, t3.y), 0) * w3.x) * w3.y;
    return saturate(r);
}
float luma(float2 uv) { return dot(img.SampleLevel(ls, uv, 0).rgb, float3(0.299, 0.587, 0.114)); }
// edges smoothed on the picture itself, after FXAA: where the brightness steps across an edge, the edge is followed
// both ways to its ends and the pixel sampled that far towards its neighbour across it, the further the nearer an end
float4 fxaa(float2 uv) {
    float2 size;
    img.GetDimensions(size.x, size.y);
    float2 px = 1 / size;
    float4 c = img.SampleLevel(ls, uv, 0);
    float m = dot(c.rgb, float3(0.299, 0.587, 0.114));
    float n = luma(uv + float2(0, -px.y)), s = luma(uv + float2(0, px.y));
    float w = luma(uv + float2(-px.x, 0)), e = luma(uv + float2(px.x, 0));
    float hi = max(m, max(max(n, s), max(w, e))), lo = min(m, min(min(n, s), min(w, e)));
    float range = hi - lo;
    if (range < max(0.0312, hi * 0.125)) return c;
    float nw = luma(uv - px), se = luma(uv + px);
    float ne = luma(uv + float2(px.x, -px.y)), sw = luma(uv + float2(-px.x, px.y));
    float across_y = abs(nw + sw - 2 * w) + 2 * abs(n + s - 2 * m) + abs(ne + se - 2 * e);
    float across_x = abs(nw + ne - 2 * n) + 2 * abs(w + e - 2 * m) + abs(sw + se - 2 * s);
    bool horz = across_y >= across_x;  // the edge runs along x, the brightness steps in y
    float l1 = horz ? n : w, l2 = horz ? s : e;
    float g1 = abs(l1 - m), g2 = abs(l2 - m);
    float step = horz ? px.y : px.x;
    float avg;
    if (g1 >= g2) {
        step = -step;
        avg = 0.5 * (l1 + m);
    } else {
        avg = 0.5 * (l2 + m);
    }
    float grad = 0.25 * max(g1, g2);
    float2 mid = uv + (horz ? float2(0, step * 0.5) : float2(step * 0.5, 0));
    float2 along = horz ? float2(px.x, 0) : float2(0, px.y);
    float2 uv1 = mid - along, uv2 = mid + along;
    float e1 = luma(uv1) - avg, e2 = luma(uv2) - avg;
    bool end1 = abs(e1) >= grad, end2 = abs(e2) >= grad;
    static const float kStride[10] = {1, 1, 1, 1, 1.5, 2, 2, 2, 4, 8};
    [loop] for (int k = 0; k < 10 && !(end1 && end2); k++) {
        if (!end1) {
            uv1 -= along * kStride[k];
            e1 = luma(uv1) - avg;
            end1 = abs(e1) >= grad;
        }
        if (!end2) {
            uv2 += along * kStride[k];
            e2 = luma(uv2) - avg;
            end2 = abs(e2) >= grad;
        }
    }
    float d1 = horz ? uv.x - uv1.x : uv.y - uv1.y, d2 = horz ? uv2.x - uv.x : uv2.y - uv.y;
    bool near1 = d1 < d2;
    float off = 0.5 - min(d1, d2) / (d1 + d2);
    if (((near1 ? e1 : e2) < 0) == (m < avg)) off = 0;
    // a pixel unlike all its neighbours (a thin line, a dot) is blended by how much it differs
    float sub = saturate(abs((2 * (n + s + w + e) + nw + ne + sw + se) / 12 - m) / range);
    sub = (-2 * sub + 3) * sub * sub;
    off = max(off, sub * sub * 0.75);
    return img.SampleLevel(ls, uv + (horz ? float2(0, off * step) : float2(off * step, 0)), 0);
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    if (opts.z < -1.5) {
        c = fxaa(i.uv);
    } else if (opts.z < 0) {
        c = cubic(i.uv);
    } else {
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
        c /= n * n;
    }
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    if (opts2.x != 1) c.rgb = pow(saturate(c.rgb), opts2.x);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

// Ambient occlusion (app::Settings::ao) from the scene's depth: the view distance of a depth pixel is the projection's
// w (p0: the stored depth's scale and offset into z/w, the projection's z offset), its position across the view from
// the projection's x and y scales (p0.w, p1.x). occlude, at half size: SSAO (p1.w 1) takes 12 points on a spiral
// within the radius (p1.y, in view units) and counts those above the surface's plane; GTAO (2) walks 3 directions
// both ways for the highest horizon and integrates the cosine-weighted sky the surface sees between them. apply, at
// full size: the half size result blurred within surfaces of a like depth, multiplied into the picture (p1.z strength)
const char* kAoHlsl = R"(
cbuffer B : register(b1) { float4 p0; float4 p1; };
Texture2D<float> dep : register(t0);
Texture2D<float> occ : register(t1);
static const float kPi = 3.14159265;
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
int2 Size() {
    uint w, h;
    dep.GetDimensions(w, h);
    return int2(w, h);
}
// 0: nothing drawn there (the cleared far plane, the sky)
float Dist(int2 c) {
    float d = dep.Load(int3(clamp(c, 0, Size() - 1), 0));
    if (d <= 0) return 0;
    float w = p0.z / ((1 - d) * p0.y - p0.x);
    return w > 0 ? w : 0;
}
float3 View(int2 c, float w) {
    float2 n = (float2(c) + 0.5) / float2(Size()) * 2 - 1;
    return float3(n.x * w * p0.w, -n.y * w * p1.x, w);
}
// the surface's normal from the nearer neighbour on each axis, so an edge does not bend it
float3 Normal(int2 c, float3 P) {
    float wl = Dist(c - int2(1, 0)), wr = Dist(c + int2(1, 0)), wu = Dist(c - int2(0, 1)), wd = Dist(c + int2(0, 1));
    float3 dx = abs(wr - P.z) < abs(wl - P.z) ? View(c + int2(1, 0), wr) - P : P - View(c - int2(1, 0), wl);
    float3 dy = abs(wd - P.z) < abs(wu - P.z) ? View(c + int2(0, 1), wd) - P : P - View(c - int2(0, 1), wu);
    float3 n = normalize(cross(dy, dx));
    return dot(n, P) > 0 ? -n : n;
}
float Noise(int2 c) { return frac(52.9829189 * frac(dot(float2(c), float2(0.06711056, 0.00583715)))); }
float Ssao(int2 c, float3 P, float3 N, float rpx, float R, float noise) {
    float sum = 0;
    [unroll] for (int i = 0; i < 12; i++) {
        float a = (i + noise) * 2.39996323, r = sqrt((i + 0.5) / 12) * rpx;
        int2 q = c + int2(round(float2(cos(a), sin(a)) * r));
        float wq = Dist(q);
        if (wq <= 0) continue;
        float3 v = View(q, wq) - P;
        float l = length(v);
        sum += max(0, dot(N, v) / (l + 1e-4) - 0.15) * saturate(1 - l * l / (R * R));
    }
    return saturate(1 - sum / 12 * 1.7);
}
float Gtao(int2 c, float3 P, float3 N, float rpx, float R, float noise) {
    float3 V = normalize(-P);
    float vis = 0;
    [unroll] for (int d = 0; d < 3; d++) {
        float phi = (d + noise) * kPi / 3;
        float3 dir = float3(cos(phi), sin(phi), 0);
        float2 omega = float2(dir.x, -dir.y);
        float3 ortho = dir - dot(dir, V) * V;
        float3 axis = normalize(cross(ortho, V));
        float3 pn = N - axis * dot(N, axis);
        float pl = length(pn) + 1e-4;
        float cosn = saturate(dot(pn, V) / pl);
        float n = sign(dot(ortho, pn)) * acos(cosn);
        float low0 = cos(n + kPi / 2), low1 = cos(n - kPi / 2);
        float h0 = low0, h1 = low1;
        [unroll] for (int s = 1; s <= 5; s++) {
            float t = (s - noise * 0.5) / 5;
            int2 off = int2(round(omega * t * t * rpx));
            if (all(off == 0)) continue;
            float w0 = Dist(c + off), w1 = Dist(c - off);
            if (w0 > 0) {
                float3 v = View(c + off, w0) - P;
                float l = length(v);
                h0 = max(h0, lerp(low0, dot(v / l, V), saturate(1 - l * l / (R * R))));
            }
            if (w1 > 0) {
                float3 v = View(c - off, w1) - P;
                float l = length(v);
                h1 = max(h1, lerp(low1, dot(v / l, V), saturate(1 - l * l / (R * R))));
            }
        }
        float a0 = n + clamp(-acos(clamp(h1, -1, 1)) - n, -kPi / 2, kPi / 2);
        float a1 = n + clamp(acos(clamp(h0, -1, 1)) - n, -kPi / 2, kPi / 2);
        float i0 = (cosn + 2 * a0 * sin(n) - cos(2 * a0 - n)) / 4;
        float i1 = (cosn + 2 * a1 * sin(n) - cos(2 * a1 - n)) / 4;
        vis += pl * (i0 + i1);
    }
    return saturate(vis / 3);
}
float occlude(float4 pos : SV_Position) : SV_Target {
    int2 c = int2(pos.xy) * 2;
    float w = Dist(c);
    if (w <= 0) return 1;
    float3 P = View(c, w);
    float3 N = Normal(c, P);
    float R = p1.y;
    // the radius across the screen: the projection's x scale over the half width
    float rpx = clamp(R / (p0.w * w) * Size().x * 0.5, 2, Size().x * 0.08);
    float noise = Noise(int2(pos.xy));
    return p1.w > 1.5 ? Gtao(c, P, N, rpx, R, noise) : Ssao(c, P, N, rpx, R, noise);
}
float4 apply(float4 pos : SV_Position) : SV_Target {
    int2 c = int2(pos.xy);
    float w = Dist(c);
    if (w <= 0) return 1;
    uint ow, oh;
    occ.GetDimensions(ow, oh);
    int2 h = c / 2;
    float sum = 0, wsum = 0;
    [unroll] for (int y = -2; y <= 2; y++)
        [unroll] for (int x = -2; x <= 2; x++) {
            int2 q = clamp(h + int2(x, y), 0, int2(ow, oh) - 1);
            float k = exp(-abs(Dist(q * 2) - w) / (0.04 * w + 0.01));
            sum += occ.Load(int3(q, 0)) * k;
            wsum += k;
        }
    // far scenery keeps its own shading: the radius is a few pixels there and only noise would show
    float ao = lerp(1, sum / max(wsum, 1e-4), p1.z * saturate(2 - w / 150));
    return float4(ao, ao, ao, 1);
}
)";

// edges smoothed by their shapes, as SMAA does (its orthogonal patterns): edges where the brightness steps (edges), then
// along each edge to its two ends and whether another edge crosses there, below or above it; the line it stands for
// runs from a crossed end, half a pixel off, to the edge's middle, and each pixel takes in its neighbour across the
// edge by how much of it lies beyond that line (weights); the picture mixed by those (blend). An edge crossed at neither
// end is a straight one and left sharp, as text is
const char* kSmaaHlsl = R"(
cbuffer B : register(b1) { float4 p0; float4 p1; };
Texture2D img : register(t0);
Texture2D edg : register(t1);
Texture2D wgt : register(t2);
SamplerState ls : register(s0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
int2 Size() {
    uint w, h;
    img.GetDimensions(w, h);
    return int2(w, h);
}
float Lum(int2 c) { return dot(img.Load(int3(clamp(c, 0, Size() - 1), 0)).rgb, float3(0.299, 0.587, 0.114)); }
// x: an edge on the pixel's left, y: on its top
float2 edges(float4 pos : SV_Position) : SV_Target {
    int2 c = int2(pos.xy);
    float l = Lum(c), ll = Lum(c - int2(1, 0)), lt = Lum(c - int2(0, 1));
    float2 d = abs(l - float2(ll, lt));
    float2 e = step(0.1, d);
    if (e.x + e.y == 0) return 0;
    // kept where it stands out from the steps around it, so the softer side of a strong edge isn't one too
    float2 m = max(d, abs(l - float2(Lum(c + int2(1, 0)), Lum(c + int2(0, 1)))));
    m = max(m, abs(float2(ll, lt) - float2(Lum(c - int2(2, 0)), Lum(c - int2(0, 2)))));
    return e * step(max(m.x, m.y), 2 * d);
}
float2 E(int2 c) { return edg.Load(int3(c, 0)).rg; }
static const int kSearch = 32;
// a pixel's part beyond the line, with d1 pixels of the edge before it and d2 after, the ends h1 and h2 off the edge:
// x, the part on this side (it takes in its neighbour), y on the neighbour's (it takes in this pixel)
float2 Area(float d1, float d2, float h1, float h2) {
    float half_len = (d1 + d2 + 1) * 0.5, mid = (1 + d2 - d1) * 0.5, b = saturate(mid);
    float a1 = h1 / half_len * (mid * b - b * b * 0.5);
    float a2 = h2 / half_len * (0.5 * (1 - b * b) - mid * (1 - b));
    return float2(max(-a1, 0) + max(-a2, 0), max(a1, 0) + max(a2, 0));
}
// r: this pixel into the one above, g: the one above into this, b: into the one on the left, a: that one into this
float4 weights(float4 pos : SV_Position) : SV_Target {
    int2 c = int2(pos.xy);
    float2 e = E(c);
    float4 w = 0;
    int d1, d2;
    [branch] if (e.y > 0.5) {
        for (d1 = 0; d1 < kSearch && E(c - int2(d1 + 1, 0)).y > 0.5; d1++) {}
        for (d2 = 0; d2 < kSearch && E(c + int2(d2 + 1, 0)).y > 0.5; d2++) {}
        float h1 = d1 < kSearch ? 0.5 * (E(c + int2(-d1, -1)).x - E(c + int2(-d1, 0)).x) : 0;
        float h2 = d2 < kSearch ? 0.5 * (E(c + int2(d2 + 1, -1)).x - E(c + int2(d2 + 1, 0)).x) : 0;
        w.rg = Area(d1, d2, h1, h2);
    }
    [branch] if (e.x > 0.5) {
        for (d1 = 0; d1 < kSearch && E(c - int2(0, d1 + 1)).x > 0.5; d1++) {}
        for (d2 = 0; d2 < kSearch && E(c + int2(0, d2 + 1)).x > 0.5; d2++) {}
        float h1 = d1 < kSearch ? 0.5 * (E(c + int2(-1, -d1)).y - E(c + int2(0, -d1)).y) : 0;
        float h2 = d2 < kSearch ? 0.5 * (E(c + int2(-1, d2 + 1)).y - E(c + int2(0, d2 + 1)).y) : 0;
        w.ba = Area(d1, d2, h1, h2);
    }
    return w;
}
float4 W(int2 c) { return wgt.Load(int3(c, 0)); }
float4 blend(float4 pos : SV_Position) : SV_Target {
    int2 c = int2(pos.xy);
    // towards the right, below, the left, above
    float4 a = float4(W(c + int2(1, 0)).a, W(c + int2(0, 1)).g, W(c).b, W(c).r);
    if (dot(a, 1) < 1e-5) return img.Load(int3(c, 0));
    float2 px = 1.0 / float2(Size()), uv = (float2(c) + 0.5) * px;
    bool h = max(a.x, a.z) > max(a.y, a.w);
    float4 off = h ? float4(a.x, 0, a.z, 0) : float4(0, a.y, 0, a.w);
    float2 k = h ? a.xz : a.yw;
    k /= k.x + k.y;
    return k.x * img.SampleLevel(ls, uv + off.xy * px, 0) + k.y * img.SampleLevel(ls, uv - off.zw * px, 0);
}
)";

// a clear of some colour channels (D3DCLEAR_TARGET_R 0x10, G 0x20, B 0x40, A 0x80): the target's copy with those
// channels set
const char* kMaskedClearHlsl = R"(
cbuffer B : register(b1) { float4 color; float4 mask; };
Texture2D img : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 psmain(float4 p : SV_Position) : SV_Target {
    return lerp(img.Load(int3(p.xy, 0)), color, mask);
}
)";

// Depth is reversed in a 32 bit float buffer: 1 at the near clip, 0 at the far one, where a float keeps its finest
// steps; a 24 bit buffer storing z / w lost the steps that told distant surfaces apart
constexpr DXGI_FORMAT kDepthTypeless = DXGI_FORMAT_R32G8X24_TYPELESS;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
constexpr DXGI_FORMAT kDepthRead = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
// Pass's blend for a picture multiplied by the pass's colour (AmbientOcclusion), below the blend factors
constexpr float kPassMultiply = -2;
constexpr float kDepthFar = 0;

const char* kDepthHlsl = R"(
#ifdef MS
Texture2DMS<float> img : register(t0);
#else
Texture2D<float> img : register(t0);
#endif
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i) : SV_Depth {
#ifdef MS
    // the nearest of the pixel's samples: an edge reads as the surface in front, never a depth between the two
    uint w, h, n;
    img.GetDimensions(w, h, n);
    int2 at = int2(i.uv * float2(w, h));
    float z = 1;
    for (uint k = 0; k < n; k++) z = min(z, img.Load(at, k));
    return z;
#else
    return img.SampleLevel(ps, i.uv, 0);
#endif
}
)";

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

// D3DBLEND, the console uses GL values
constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR

D3D12_BLEND Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return D3D12_BLEND_ZERO;
        case 1: return D3D12_BLEND_ONE;
        case 0x300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case 0x301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case 0x302: return D3D12_BLEND_SRC_ALPHA;
        case 0x303: return D3D12_BLEND_INV_SRC_ALPHA;
        case 0x304: return D3D12_BLEND_DEST_ALPHA;
        case 0x305: return D3D12_BLEND_INV_DEST_ALPHA;
        case kBlendDestColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case 0x307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case 0x308: return D3D12_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
        default: return D3D12_BLEND_ONE;
    }
}

// D3DBLENDOP, GL values
D3D12_BLEND_OP BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 0x8007: return D3D12_BLEND_OP_MIN;
        case 0x8008: return D3D12_BLEND_OP_MAX;
        default: return D3D12_BLEND_OP_ADD;
    }
}

// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207, D3D12's order from 1
D3D12_COMPARISON_FUNC Cmp(uint32_t v) {
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    return D3D12_COMPARISON_FUNC(n + 1);
}

// D3DSTENCILOP, GL values
D3D12_STENCIL_OP StencilOp(uint32_t v) {
    switch (v) {
        case 0: return D3D12_STENCIL_OP_ZERO;
        case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
        case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;
        case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;
        case 0x150A: return D3D12_STENCIL_OP_INVERT;
        case 0x8507: return D3D12_STENCIL_OP_INCR;
        case 0x8508: return D3D12_STENCIL_OP_DECR;
        default: return D3D12_STENCIL_OP_KEEP;
    }
}

// D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border, 5 clamp to edge
D3D12_TEXTURE_ADDRESS_MODE Address(uint32_t v) {
    switch (v) {
        case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

DXGI_FORMAT HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return DXGI_FORMAT_BC1_UNORM;
        case Host::BC2: return DXGI_FORMAT_BC2_UNORM;
        case Host::BC3: return DXGI_FORMAT_BC3_UNORM;
        case Host::R32F: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// rows of a texture upload start D3D12_TEXTURE_DATA_PITCH_ALIGNMENT apart
uint32_t UploadPitch(uint32_t row_bytes) { return (row_bytes + 255) & ~255u; }

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return bd;
}

// one level, one sample: callers set Flags where they differ
D3D12_RESOURCE_DESC Texture2DDesc(DXGI_FORMAT fmt, uint64_t w, uint32_t h) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    return td;
}

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    g_perf.sigs++;
    g_perf.sig_bytes += total <= kSigWholeMax ? total : kSigSamples * kSigSampleBytes;
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

struct Tex {
    ComPtr<ID3D12Resource> res;
    TextureDesc d;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;  // hash of the colours a P8 texture was decoded with
    uint8_t flags = 0;  // Extension::Decoded's
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
    uint32_t raw_points;  // points as single host pixels: visibility tests count each as one console pixel
    uint32_t msaa;        // samples of a multisampled target, 0 without
};

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;       // through the gamma lookup
    bool alpha = false;       // the source's alpha, else 1
    uint32_t taps = 1;        // n x n box filter per pixel
    bool cubic = false;       // Catmull-Rom, for a picture drawn larger than it is
    bool fxaa = false;        // edges smoothed, in place of cubic
    float mul = 1;            // colour scale
    DXGI_FORMAT rtv = kBgra;  // the target's format
    bool over = false;        // premultiplied over the target, which keeps its alpha
    uint32_t samples = 1;     // the target's
    float power = 1;          // each channel raised to this: under 1 brighter, over 1 darker
};

// Shrink's settings past the source and the size
struct ShrinkOptions {
    uint32_t taps = 1;
    // into: a copy from an earlier call to draw over again, in state into_state
    ID3D12Resource* into = nullptr;
    D3D12_RESOURCE_STATES into_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    float mul = 1;
};

// a draw's four texture stages: BindStageTexturesAndTargets fills them, the constants, views and samplers read them
// what the last draw set on the command list, so the next one sets only what differs; anything else that sets them
// clears it (after TGP482/XboxXBE-Recomp 0fd0044)
struct DrawBinds {
    ID3D12PipelineState* pso = nullptr;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    bool rasterizer = false;  // viewport and scissor hold
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    bool output = false;  // blend and stencil hold
    float blend[4] = {};
    uint32_t stencil = 0;
    D3D12_GPU_VIRTUAL_ADDRESS vsc = 0;  // root constant buffers
    D3D12_GPU_VIRTUAL_ADDRESS dc = 0;
    uint32_t views = UINT32_MAX;  // root descriptor table slots
    uint32_t samplers = UINT32_MAX;
};

// the last targets EnsureTarget and FindTarget marked used, so target_used_ is written once a frame each
struct UsedTarget {
    uint32_t data = 0;
    uint64_t frame = UINT64_MAX;
};

struct DrawTextures {
    TexInfo info[4];
    D3D12_SHADER_RESOURCE_VIEW_DESC sv[4];
    ID3D12Resource* res[4];
    bool rt[4] = {};            // stage samples a render target
    float k[4] = {1, 1, 1, 1};  // Extension::Sample's k
    float wide[4] = {};         // host texels per console texel of a target read into a smaller one, 0 otherwise
};

// a draw's shaders: what they are made from, their cache keys, the hashes Compile returned and the compiled blobs
struct DrawShaders {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    ID3DBlob* vs_code = nullptr;
    ID3DBlob* ps_code = nullptr;
    bool later = false;  // a shader still being compiled by the threads: the draw is left out
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

class D3D12Backend final : public Backend, public Context {
public:
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void WaitGpuFrame(uint64_t n) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;
    void ClearReport() override { counted_.clear(); }
    // done once the list being recorded, the next fence value, has run
    void GetReport(uint32_t report) override { reports_.push_back({report, counted_, fence_value_ + 1, presents_}); }

    ID3D12Device* Device() override { return dev_.Get(); }
    ID3D12GraphicsCommandList* List() override { return cl_.Get(); }
    void Release(ComPtr<ID3D12Resource> r) override { CurrentFrame().garbage.push_back(std::move(r)); }
    D3D12_CPU_DESCRIPTOR_HANDLE View(const Target& t) override { return Cpu(rtv_heap_.Get(), t.view, rtv_inc_); }
    void Unbind() override { bound_rt_ = bound_ds_ = nullptr; }
    void Capture(Target& t, const char* tag, float mul) override;
    float Scale() const override { return scale_.y; }
    void GuestSize(uint32_t& w, uint32_t& h) const override {
        w = guest_w_;
        h = guest_h_;
    }
    uint64_t Presents() const override { return presents_; }

private:
    struct Frame {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12Resource> upload;
        uint8_t* map = nullptr;
        uint64_t used = 0;
        uint64_t fence = 0;
        std::vector<ComPtr<ID3D12Resource>> garbage;
        uint32_t srv = 0;
        uint32_t queries = 0;  // timestamps written, two per command list
        // a measurement's pass marks: a timestamp where the kind of work changes, each with the kind that follows
        // (or the list's end)
        uint32_t marks = 0, list_marks = 0;
        GpuPass mark_pass[512];
        bool mark_end[512];
        uint64_t mark_tag[512];
        ComPtr<ID3D12CommandAllocator> xalloc;  // xl_'s
        ComPtr<ID3D12Resource> stage;           // interpolation: a block of constants slots per frame shown
        uint8_t* stage_map = nullptr;
        uint32_t stage_blocks = 0;
    };
    struct Alloc {
        ID3D12Resource* res;
        uint64_t off;
        uint8_t* cpu;
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
    };

    Alloc Upload(uint64_t size, uint64_t align = 256);
    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    uint32_t AllocSrvSlots(uint32_t n);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto c = h->GetCPUDescriptorHandleForHeapStart();
        c.ptr += size_t(i) * inc;
        return c;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto g = h->GetGPUDescriptorHandleForHeapStart();
        g.ptr += uint64_t(i) * inc;
        return g;
    }
    void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) override;
    void Begin();
    void Submit(bool wait);
    void WaitFence(uint64_t value);
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h);
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], bool gamma, bool alpha, uint32_t taps = 1,
              float mul = 1, DXGI_FORMAT rtv = kBgra) override {
        Blit(src, fmt, uvrect, BlitOptions{.gamma = gamma, .alpha = alpha, .taps = taps, .mul = mul, .rtv = rtv});
    }
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o);
    uint32_t EnsureBlitSamplers();
    void DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                        std::optional<uint32_t> sampler_slot = {});
    ID3D12Resource* Feedback(Target& t) override;
    uint64_t draw_serial_ = 1;  // counts draws and masked clears, for Feedback
    // MSAA: the scene's targets (guest sized colour and depth) hold msaa_ samples a pixel, the rest one. A
    // multisampled target is never read itself: its single sample copy t.fb is, refreshed by RefreshCopy only when
    // the target was written since. Every write moves the target out of the state RefreshCopy leaves it in
    // (CopiedFrom), so that state says the copy is current.
    // t.fb made current: a copy, a colour resolve, or a depth target's nearest samples drawn
    ID3D12Resource* RefreshCopy(Target& t);
    // what a read of the target takes, in state *st: the target itself, or a multisampled one's current copy
    ID3D12Resource* Readable(Target& t, D3D12_RESOURCE_STATES*& st);
    // the most samples the adapter has for the scene's formats, a power of two up to want
    uint32_t Samples(uint32_t want);
    uint32_t msaa_ = 1;      // samples of the scene's targets
    uint32_t msaa_set_ = 0;  // the setting they came from
    uint32_t latency_set_ = 2;  // frames queued for the display (FrameLatency)
    static D3D12_RESOURCE_STATES CopiedFrom(const Target& t) {
        return t.depth ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    }
    void Resolve(Target& t) {
        if (t.range != 1 && ext_) ext_->Resolve(*this, t);
    }
    // Pass, then its step
    void Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh, uint32_t w,
              uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3], const float p[8],
              float blend) override {
        Pass(hlsl, entry, rtv, rh, w, h, src, fmt, p, blend, 1);
    }
    // into a target of that many samples
    void FillDepthConstants(const State& s, const DrawShaders& sh, const PsoKey& k, DrawConstants& c) const;
    void Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh, uint32_t w,
              uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3], const float p[8], float blend,
              uint32_t samples);
    ID3D12PipelineState* EnsurePassPipeline(const char* hlsl, const char* entry, DXGI_FORMAT rtv, float blend,
                                            uint32_t samples);
    // Draw, in call order, each callee right after its caller
    bool DepthCopy(const State& s);
    ID3D12PipelineState* EnsureDepthCopyPipeline(bool ms, uint32_t samples);
    bool DrawDepthCopy(Target& src, Target& dst);
    // src's depth over the w x h depth target at dh, a multisampled src's nearest sample
    bool DepthPass(ID3D12Resource* src, bool src_ms, D3D12_CPU_DESCRIPTOR_HANDLE dh, uint32_t w, uint32_t h,
                   uint32_t samples);
    bool AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices,
                      Assembled& a);
    bool BindTargets(const State& s);
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    ID3D12Resource* BindTexture(const State& s, int stage, DrawTextures& stages);
    ID3D12Resource* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                      DrawTextures& stages);
    ID3D12Resource* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                     DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours,
                       bool same);
    void UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    bool StillCompiling(uint64_t key, ID3DBlob*& code, uint64_t& hash);
    bool Settle(uint64_t key, ID3DBlob* code, uint64_t hash);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    ID3D12PipelineState* EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps, bool* later = nullptr);
    void StorePipeline(uint64_t h, ID3D12PipelineState* pso);
    void FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps, D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const;
    void ExpandPoints(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d);
    void ExpandLines(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d);
    app::Scale2 TargetScale() const;  // host pixels per console pixel of the bound target
    app::Scale2 RenderTargetShift(const State& s, const DrawShaders& sh) const;
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestExtent size, DrawConstants& c);
    void FillTextureConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages, DrawConstants& c);
    void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c);
    void CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                     const DrawTextures& stages);
    void CaptureTextures(const State& s, const DrawTextures& stages);
    void EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c);
    void CaptureGeometry(const State& s, const Assembled& a, const DrawConstants& c);
    void CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c);
    void CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk);
    void CaptureShader(const char* kind, uint64_t key, const std::string& src);
    uint32_t EnsureViewSet(const DrawTextures& stages);
    void UpdateTargetUsed(uint32_t data);
    void SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages);
    uint32_t EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp);
    void SetViewportScissor(const State& s, uint32_t gw, uint32_t gh);
    void SetBlendFactorStencilRef(const State& s);
    void DrawGeometry(const Assembled& a);
    void UploadVertexBuffers(const Assembled& a);
    // visibility tests
    bool EnsureQueries();
    uint32_t BeginCountedDraw(const State& s);
    void EndCountedDraw(uint32_t query, bool points);
    float SamplesPerPixel() const;
    float PointSamples() const;
    void DeliverReports();
    // shaders by source hash: Compile returns the bytecode and sets hash
    ID3DBlob* Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash,
                      bool later = false);
    void WriteShader(uint64_t hash, uint32_t src_size, ID3DBlob* code);
    // shaders.bin beside the executable keeps compiled shaders between runs: by source hash, the source's size and
    // the bytecode; read whole at the first compile, appended to after each new one
    struct DiskShader {
        uint32_t src_size;
        ComPtr<ID3DBlob> code;
    };
    std::unordered_map<uint64_t, DiskShader> disk_shaders_;
    std::FILE* shader_file_ = nullptr;
    bool shader_file_read_ = false;
    void OpenShaderFile();
    // pipelines.bin beside the executable keeps the driver's built pipelines between runs, in an
    // ID3D12PipelineLibrary named by PsoKey hash: a pipeline seen in an earlier run is loaded, not built again. The
    // file is written back a few seconds after the last new pipeline, by a thread of its own.
    ComPtr<ID3D12PipelineLibrary> pso_lib_;
    std::vector<char> pso_lib_data_;  // the library reads from it while it lives
    uint64_t root_hash_ = 0;          // the root signature's bytes: another build's pipelines don't match
    bool pso_lib_tried_ = false, pso_lib_dirty_ = false;
    PerfClock::time_point pso_lib_stored_;
    std::shared_ptr<std::atomic<bool>> pso_lib_writing_ = std::make_shared<std::atomic<bool>>(false);
    void OpenPipelineLibrary();
    void SavePipelineLibrary();
    // shaders compiled and pipelines built or loaded on the render thread, the time they took: logged every five
    // seconds that had any, the hitches of a place seen for the first time
    struct BuildCounts {
        uint32_t shaders = 0, built = 0, loaded = 0;
        double ms = 0, worst = 0;
    } builds_;
    PerfClock::time_point builds_logged_ = PerfClock::now();
    void CountBuild(PerfClock::time_point start);
    void LogBuilds();
    // pipekeys.bin beside the executable keeps the key of every pipeline a run used, and pipekeys_seed.bin and
    // shaders_seed.bin, shipped with the game, the ones of the places we played: at the start a thread of its own
    // builds or loads them all, so a place seen before costs the render thread nothing when it comes
    std::unordered_set<uint64_t> pso_keys_seen_;
    std::FILE* pso_key_file_ = nullptr;
    void NotePipelineKey(uint64_t h, const PsoKey& k);
    std::thread prebuild_;
    std::mutex prebuild_lock_;
    std::condition_variable prebuild_cv_;
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> prebuilt_;  // made ahead, not yet asked for
    std::unordered_set<uint64_t> prebuild_claimed_;                       // asked for first: the thread skips them
    uint64_t prebuild_busy_ = 0;                                          // the one the thread is making
    std::atomic<bool> prebuild_stop_{false}, prebuild_stored_{false};
    void StartPrebuild();
    void StopPrebuild();
    ID3D12PipelineState* TakePrebuilt(uint64_t h);
    // Shaders and pipelines met for the first time in play are made by threads of their own: a draw that needs one
    // still being made is left out, a frame or two, instead of the frame waiting the tens of milliseconds a compile
    // takes. Visibility-tested draws (their pixels are counted) and captures still wait on the render thread, which
    // then makes it itself; the threads' copy is dropped. XBE_SYNC_SHADERS=1 makes all of them there as before.
    struct ShaderJob {
        std::string src;
        const char* entry;
        const char* profile;
        uint64_t hash;
    };
    struct PipelineJob {
        PsoKey k;
        uint64_t h;
        ComPtr<ID3DBlob> vs, ps, gs;
    };
    struct ShaderDone {
        uint64_t hash;
        uint32_t src_size;
        ComPtr<ID3DBlob> code;
    };
    struct PipelineDone {
        PsoKey k;
        uint64_t h;
        ComPtr<ID3D12PipelineState> pso;
    };
    std::vector<std::thread> workers_;
    std::mutex work_lock_;
    std::condition_variable work_cv_;
    std::deque<ShaderJob> shader_jobs_;      // under work_lock_
    std::deque<PipelineJob> pipeline_jobs_;  // under work_lock_
    std::vector<ShaderDone> shaders_done_;   // under work_lock_
    std::vector<PipelineDone> pipelines_done_;
    bool workers_stop_ = false;
    std::atomic<bool> work_finished_{false};
    std::unordered_set<uint64_t> shaders_queued_, pipelines_queued_;
    std::unordered_map<uint64_t, uint64_t> shaders_pending_;  // a draw's shader key -> source hash being compiled
    bool later_ok_ = false;                                    // this draw may be left out for one
    struct LaterCounts {
        uint32_t shaders = 0, pipelines = 0, draws = 0;
    } later_;
    void StartWorkers();
    void StopWorkers();
    void WorkerLoop();
    void TakeFinished();
    // Blit's pipeline
    ID3D12PipelineState* EnsureBlitPipeline(DXGI_FORMAT rtv, bool over = false, uint32_t samples = 1);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv,
                                                              uint32_t samples = 1) const;
    // Init, in order, each callee right after its caller
    bool CreateDevice();
    void FitToAdapter(const DXGI_ADAPTER_DESC1& ad);
    void FitRenderScale(double gpu_ms);
    // FitRenderScale: the GPU's time of each frame of the window, its windows counted, the settings it last saw
    std::vector<float> fit_ms_;
    uint32_t fit_windows_ = 0, fit_percent_ = 0, fit_msaa_ = 0;
    double fit_partial_ms_ = 0;  // the frame's lists run before its last, waited for
    bool CreateSwapChain(HWND hwnd);
    void CreateHeaps();
    uint32_t CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible, ComPtr<ID3D12DescriptorHeap>& out);
    void CreateBackBufferViews();
    bool CreateRootSignature();
    void CreateFrames();
    void CreateGammaLut();
    void LoadSettings();
    // Present, in call order, each callee right after its caller; DrawFrame runs the two after it
    void FinishCapture(const Surface& front);
    void StartCapture();
    void ResizeSwap();
    void UploadGammaRamp();
    void DrawFrame(Target* t, uint32_t bi);
    void BlitFrame(Target& t, D3D12_CPU_DESCRIPTOR_HANDLE rh);
    // the frame's picture with its edges smoothed by SMAA, readable; null when it can't be
    ID3D12Resource* Smaa(Target& t);
    void RenderUi();
    void ReleaseOldTextures();
    void ReleaseOldTargets();
    bool ReuseTarget(Target& t);
    void ResetFrame();
    void CountFps();
    void ApplySettings();
    ID3D12Resource* SmallCopy(Target& t, ID3D12Resource* res, D3D12_RESOURCE_STATES& st);
    ComPtr<ID3D12Resource> Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt, uint32_t w,
                                  uint32_t h, const ShrinkOptions& o = {});
    bool ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void CapStep(const Surface& rt);
    // a resource CapSave shrinks to w x h and writes, in state *state, colour times mul
    struct CapImage {
        ID3D12Resource* res;
        D3D12_RESOURCE_STATES* state;
        DXGI_FORMAT fmt;
        uint32_t w;
        uint32_t h;
        float mul = 1;
    };
    void CapSave(const CapImage& img, const char* name);
    void CapSave(Target& t, const char* name);
    // a surface's place in targets_: its key, console size and entry, end when it has none
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw;
        uint32_t gh;
        std::unordered_map<uint64_t, Target>::iterator it;
        bool sized;  // the entry is there at the surface's size
    };
    Target* FindTarget(const Surface& s, bool depth);
    // gw, gh: a console size other than the surface's, 0 for its own
    Target* EnsureTarget(const Surface& s, bool depth, uint32_t gw = 0, uint32_t gh = 0);
    Target* EnsureDepth(const State& s, const Target* rt);
    // EnsureTarget's steps, in call order
    TargetSlot FindTargetSlot(const Surface& s, bool depth, uint64_t alt, uint32_t gw = 0, uint32_t gh = 0);
    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    bool TargetStale(uint32_t data) const;
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool CreateTargetResource(Target& t, const Surface& s);
    bool CreateTargetView(Target& t);
    // UploadTarget, then its step
    void UploadTarget(Target& t, const Surface& s);
    ComPtr<ID3D12Resource> CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    // Clear's arguments past the state
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    void ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs);
    // Copy's guest lists: count src_rects, dst_points 0 or one per rect
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
    };
    void CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                    const CopyList& l);
    void CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                      const CopyList& l);
    // the target as a guest sized BGRA copy source, and the shrunk copy that holds it when it is not one already
    struct CopySource {
        ID3D12Resource* res;
        ComPtr<ID3D12Resource> down;
    };
    CopySource PrepareCopySource(Target& t, const ShrinkOptions& o);
    // RecordLate's steps, under late_lock_
    struct Late;
    void ResetLate(Late& l, const Target& t);
    ID3D12Resource* PrepareLateSource(Target& t, Late& l);
    void RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src);

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12InfoQueue> info_;  // XBE_D3D12_DEBUG: the debug layer's messages, logged at each present
    void LogDebugMessages();
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_;
    ComPtr<ID3D12Resource> bb_[kFrames];
    ComPtr<ID3D12Fence> fence_;
    // XBE_PERF: each command list's start and end timestamps, kTimestamps per frame, read once its fence has passed
    static constexpr uint32_t kTimestamps = 64;
    ComPtr<ID3D12QueryHeap> time_heap_;
    static constexpr uint32_t kMarks = 512;
    ComPtr<ID3D12QueryHeap> mark_heap_;
    ComPtr<ID3D12Resource> mark_read_;
    const uint64_t* mark_map_ = nullptr;
    GpuPass pass_ = GpuPass::Other;
    uint64_t pass_tag_ = 0;  // the target drawn to, TargetTag
    // the kind of work from here on, and the target, marked while measuring
    void Mark(GpuPass p, uint64_t tag = 0);
    // a target's shape for the measurement: guest and host size, samples
    static uint64_t TargetTag(const Target& t) {
        return uint64_t(t.gw & 0xFFF) | uint64_t(t.gh & 0xFFF) << 12 | uint64_t(t.w & 0xFFFF) << 24 |
               uint64_t(t.h & 0xFFFF) << 40 | uint64_t(t.samples & 0xF) << 56 | uint64_t(t.depth) << 60;
    }
    void WriteMark(bool end);
    GpuPass PassOf(const Target& t) const {
        if (t.samples > 1) return GpuPass::Scene;
        return t.gw == guest_w_ && t.gh == guest_h_ ? GpuPass::Full : GpuPass::Small;
    }
    // a kind of work for a scope, the one before it after
    struct PassScope {
        D3D12Backend& b;
        GpuPass prev;
        uint64_t prev_tag;
        PassScope(D3D12Backend& b, GpuPass p, uint64_t tag = 0) : b(b), prev(b.pass_), prev_tag(b.pass_tag_) {
            b.Mark(p, tag);
        }
        ~PassScope() { b.Mark(prev, prev_tag); }
    };
    ComPtr<ID3D12Resource> time_read_;
    const uint64_t* time_map_ = nullptr;
    double tick_ms_ = 0;
    double ReadGpuTime(Frame& f);
    void LogMemory();
    HANDLE event_ = nullptr;
    uint64_t fence_value_ = 0;
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    DrawBinds binds_;
    std::array<UsedTarget, 4> recent_used_{};
    uint32_t recent_used_next_ = 0;
    // texture view sets made since the descriptor ring last started over, by a hash of their resources and views, to
    // their first slot; resources freed in that time go to the frame's garbage, so no other one takes their address
    // (after TGP482/XboxXBE-Recomp 62a53d5)
    std::unordered_map<uint64_t, uint32_t> view_sets_;
    uint64_t view_sets_frame_ = UINT64_MAX;
    // the constants last uploaded in this command list, reused while a draw's are the same; none at a list's start
    // (after TGP482/XboxXBE-Recomp 908dc53)
    decltype(State::vsc) last_vsc_{};
    D3D12_GPU_VIRTUAL_ADDRESS last_vsc_gpu_ = 0;
    DrawConstants last_dc_{};
    D3D12_GPU_VIRTUAL_ADDRESS last_dc_gpu_ = 0;
    ComPtr<ID3D12GraphicsCommandList> cl_;
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    ComPtr<ID3D12DescriptorHeap> smp_heap_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12DescriptorHeap> ui_heap_;
    uint32_t srv_inc_ = 0;
    uint32_t smp_inc_ = 0;
    uint32_t rtv_inc_ = 0;
    uint32_t dsv_inc_ = 0;
    std::vector<uint32_t> free_rtv_;
    std::vector<uint32_t> free_dsv_;
    ComPtr<ID3D12RootSignature> root_;
    std::unordered_map<uint64_t, ComPtr<ID3DBlob>> blobs_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // vertex and pixel program keys to source hash
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> psos_;
    std::unordered_map<uint64_t, uint32_t> samplers_;
    uint32_t smp_used_ = 0;
    std::unordered_map<uint64_t, Target> targets_;
    // the last depth and stencil clear of each depth buffer's memory, for its targets of other sizes: the game clears
    // the depth buffer with the screen bound, then draws a security camera's picture into a smaller texture over it
    struct DepthClear {
        uint64_t z_gen = 0, s_gen = 0;
        float z = 1;
        uint32_t stencil = 0;
        uint32_t z_gw = 0, z_gh = 0, s_gw = 0, s_gh = 0;  // the console size each was made at
    };
    std::unordered_map<uint32_t, DepthClear> depth_clears_;
    uint64_t clear_serial_ = 0;
    void NoteDepthClear(Target& t, uint32_t flags, float z, uint32_t stencil);
    void TakeDepthClear(Target& t);
    std::unordered_map<uint32_t, uint64_t> target_used_;  // target address to the frame it was last used
    std::unordered_map<uint64_t, Tex> textures_;  // by data address, a P8 texture's with its palette's above
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> blit_psos_;
    Extension* ext_ = nullptr;
    // one frame capture: every draw in a text file, each target as the frame leaves it, the textures it read
    FILE* cap_ = nullptr;
    std::filesystem::path cap_dir_;
    uint32_t cap_i_ = 0;
    uint32_t cap_rt_ = 0;
    std::unordered_set<uint32_t> cap_seen_;
    std::unordered_set<uint64_t> cap_sh_;
    std::unordered_set<uint32_t> cap_vs_;  // vertex shaders whose large draw had its inputs written
    struct CapTex {
        ComPtr<ID3D12Resource> res;
        DXGI_FORMAT fmt;
        uint32_t data;
        uint32_t w;
        uint32_t h;
    };
    std::vector<CapTex> cap_tex_;
    ComPtr<ID3D12Resource> lut_;
    D3D12_RESOURCE_STATES lut_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;  // the draw's targets, the bound ones go when a texture's copy is drawn
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;
    app::Scale2 scale_{1.0f, 1.0f};  // along x and y: a picture shown wider than 4:3 has more host pixels across
    // surfaces locked without a wait: copies in flight, the newest finished one goes to guest memory
    struct Late {
        ComPtr<ID3D12Resource> rb[kFrames];
        ComPtr<ID3D12Resource> down;
        D3D12_RESOURCE_STATES down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t row_pitch = 0;
        uint32_t next = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;                      // DeliverLate runs on the title's thread
    std::atomic<uint64_t> presents_seen_{0};    // presents_ for DeliverLate
    std::atomic<uint64_t> present_fence_[kFrames + 1] = {};  // by presents_, for WaitGpuFrame
    Assembled asm_;  // kept between draws for its buffers
    // visibility tests: an occlusion query per counted draw, summed into report memory once the GPU is done
    struct Query {
        uint32_t index;
        float samples_per_pixel;  // host samples one console pixel covers
    };
    struct Report {
        uint32_t va;
        std::vector<Query> queries;
        uint64_t fence;
        uint64_t frame;  // presents before it: the frame it was made in
    };
    ComPtr<ID3D12QueryHeap> query_heap_;
    ComPtr<ID3D12Resource> query_rb_;
    const uint64_t* query_counts_ = nullptr;
    uint32_t query_next_ = 0;
    std::vector<Query> counted_;  // since the last ClearReport
    std::deque<Report> reports_;
    bool tearing_ = false;
    bool conservative_ok_ = false;  // the GPU rasterizes conservatively (kFillSealed)
    uint64_t presents_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;

    // Frame interpolation (app::Settings::frame_interp, gpu/interp.h). With n above 1 each frame the title draws is
    // shown n times, the first n-1 drawn again with its matrices blended back toward the frame before. The frame's
    // command list cl_ is kept whole and run n times: its draws read their vertex constants from interp_arena_, which
    // a copy fills before each run (an in-between frame's blended constants, last the frame's own), and after an
    // in-between run every resource the frame moved goes back to the state the frame found it in. The frame is drawn
    // as shown into out_ (DrawFrame), each run's copied into shown_, and the presenter thread shows them spread over
    // the title's frame period. A frame the GPU was waited on in the middle of, one with no 3D or whose camera cut is
    // shown once.
public:
    ~D3D12Backend() override {
        StopWorkers();
        StopPrebuild();
        StopPresenter();
    }

private:
    double InterpRate() const;
    void InterpFrameStart();
    D3D12_GPU_VIRTUAL_ADDRESS InterpConstants(const State& s);
    interp::Keys InterpKey(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) const;
    mutable uint32_t interp_fields_[13] = {};  // the last draw's key, for the log
    void InterpSplit();
    bool EnsureInterp(uint32_t n);
    void InterpPresent(Target* t);
    void InterpLog();
    void CloseList();
    void BeginExtra();
    void RunList(ID3D12GraphicsCommandList* l);
    void FillArena(uint32_t block);
    void CopyShown(ID3D12Resource* img);
    void RestoreStates();
    void StartPresenter();
    void StopPresenter();
    void DrainPresenter();
    void PresenterLoop();
    void PresentSwap();
    uint32_t RefreshRate() const;
    uint32_t interp_n_ = 1;      // constants blocks for the one being drawn: its own and one per in-between frame
    // where each frame shown this title frame falls between the last and this one (1: this one itself), one a
    // display refresh under vertical sync: at 100 Hz five for every three of the title's
    std::array<float, kInterpMax> interp_t_{};
    uint32_t interp_ts_ = 0;
    double interp_rate_ = 1;   // frames shown a title frame
    double interp_phase_ = 0;  // where the next one shown falls
    bool interp_whole_ = false;  // the frame's commands are all in cl_ so far
    uint32_t interp_slots_ = 0;  // constants slots given this frame
    interp::Keys interp_key_{};    // the draw being made's
    interp::Matcher interp_match_;
    // the frame's draws by their registers, to the older draw the first of them was blended from: every pass over one
    // surface placed by the same matrices is blended the same way (the passes after the first draw where the depth is
    // EQUAL to the first's: blended from two different older draws, they would miss it by a hair and flicker)
    std::unordered_map<uint64_t, int32_t> interp_same_;
    // the matrices the title named (State::vsc_id, d3d::g_matrix_table), the last frame's and this one's: a named
    // group is blended from the last frame's of its name, whichever draw and slot that was in
    // (twice: the name came with two different matrices in the frame, two bones alike when it was given, and is
    // no one's)
    using Mat4 = std::array<std::array<float, 4>, 4>;
    struct Named {
        Mat4 m;
        bool twice = false;
    };
    std::unordered_map<uint32_t, Named> named_older_, named_newer_;
    // this frame's matrices by their values, to the last frame's the first draw matched exactly had for it: every
    // piece placed by one matrix (the panels of a wagon's wall) is blended from one, and its seams stay shut. Each
    // matched on its own, a panel paired by its shape with another object's moved apart from its neighbours and the
    // gap between them flickered open in the frames between.
    std::unordered_map<uint64_t, Mat4> interp_shared_;
    // by program hash: whether it reads bones (interp::Bones), and which groups of the interpolated registers
    struct InterpUse {
        bool bones = false;
        uint32_t groups = 0xF;
    };
    std::unordered_map<uint64_t, InterpUse> interp_use_;
    struct InterpFrame {
        uint32_t draws3d = 0, matched = 0, cut = 0, moved = 0, kept = 0;
    } interp_frame_;
    struct InterpCounts {
        uint64_t frames = 0, held = 0, split = 0, full = 0, draws3d = 0, matched = 0, cut = 0, moved = 0, kept = 0;
        uint64_t tiers[interp::kTiers + 1] = {};  // 3D draws matched in each tier
        uint64_t skinned = 0, skinned_tiers[interp::kTiers + 1] = {}, bones_cut = 0, by_name = 0;  // of skinned draws
        uint64_t paired[5] = {};  // 3D draws' matrices by how they were paired (InterpConstants)
        uint64_t named = 0, named_found = 0;  // named groups drawn, and of them those the last frame had
    } interp_counts_;
    std::chrono::steady_clock::time_point interp_log_t_ = std::chrono::steady_clock::now();
    ComPtr<ID3D12Resource> interp_arena_;  // the draws' vertex constants as the run being made reads them
    // the states each resource was moved between this frame, first and last
    std::unordered_map<ID3D12Resource*, std::pair<D3D12_RESOURCE_STATES, D3D12_RESOURCE_STATES>> frame_states_;
    bool track_states_ = false;
    ComPtr<ID3D12GraphicsCommandList> xl_;  // the copies and barriers between runs
    ComPtr<ID3D12Resource> out_;            // the frame drawn as shown
    // Smaa's edges, weights and result at the frame's size, with their views and states
    struct SmaaTarget {
        ComPtr<ID3D12Resource> res;
        uint32_t view = UINT32_MAX;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    };
    SmaaTarget smaa_[3];
    uint32_t smaa_w_ = 0, smaa_h_ = 0;
    // ambient occlusion (AmbientOcclusion): the frame's scene, its target and depth and the draws that wrote depth
    // there, done once a frame; the half size occlusion
    uint64_t ao_frame_ = UINT64_MAX;
    const Target* ao_rt_ = nullptr;
    const Target* ao_ds_ = nullptr;
    uint32_t ao_draws_ = 0;
    bool ao_done_ = false;
    SmaaTarget ao_;
    uint32_t ao_w_ = 0, ao_h_ = 0;
    void NoteAmbientOcclusion(const State& s, const PsoKey& k);
    bool AmbientOcclusion(const State& s);
    D3D12_RESOURCE_STATES out_state_ = D3D12_RESOURCE_STATE_COPY_SOURCE;
    // each frame shown, a set for each of the last three title frames: the presenter's queue holds up to three
    // images, from the frame before the last at most, and a set's not drawn again before it's all shown
    static constexpr uint32_t kShownSets = 3;
    ComPtr<ID3D12Resource> shown_[kShownSets][kInterpMax];
    uint32_t shown_set_ = 0, shown_w_ = 0, shown_h_ = 0, shown_n_ = 0;
    double period_ms_ = 1000.0 / 60;  // between the title's frames, smoothed
    std::chrono::steady_clock::time_point last_present_{}, base_{};
    uint32_t refresh_ = 60;
    // the presenter: shows each image at its time, on a thread of its own
    struct Showing {
        ID3D12Resource* img;
        std::chrono::steady_clock::time_point at;
        double slot_ms;  // between frames shown
        bool real;       // the title's own frame: shown however late
        uint32_t set;
    };
    std::thread presenter_;
    std::mutex present_lock_;  // pq_, pending_, presenter_quit_, presenter_busy_
    std::condition_variable present_cv_;
    std::deque<Showing> pq_;
    uint32_t pending_[kShownSets] = {};
    bool presenter_quit_ = false;
    bool presenter_busy_ = false;
    std::mutex swap_lock_;  // the swap chain and its buffers
    static constexpr uint32_t kPresenterLists = 8;
    ComPtr<ID3D12CommandAllocator> palloc_[kPresenterLists];
    uint64_t palloc_fence_[kPresenterLists] = {};
    uint32_t palloc_next_ = 0;
    ComPtr<ID3D12GraphicsCommandList> pl_;
    ComPtr<ID3D12Fence> pfence_;
    uint64_t pfence_value_ = 0;
    HANDLE pevent_ = nullptr;
    HANDLE latency_wait_ = nullptr;  // the swap chain's: signalled when the display has room for a frame
    UINT SwapFlags() const;
    uint32_t FrameLatency() const;
    void WaitLatency();
    void ShowImage(ID3D12Resource* img);
    std::atomic<uint64_t> shown_count_{0}, dropped_count_{0};
    std::atomic<uint64_t> wait_us_{0};  // waited for the display's room, for the log
    std::atomic<uint64_t> shown_total_{0};  // every image the presenter showed, for the frame rate shown
    uint64_t fps_shown_ = 0;
    uint64_t interp_log_shown_ = 0;
    uint32_t interp_frames_logged_ = 0;
};

bool D3D12Backend::Init(uint32_t width, uint32_t height) {
    g_reversed_depth = true;
    guest_w_ = width;
    guest_h_ = height;
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!CreateDevice()) return false;
    msaa_set_ = SceneMsaa();
    msaa_ = Samples(msaa_set_);
    if (!CreateSwapChain(hwnd)) return false;
    CreateHeaps();
    CreateBackBufferViews();
    if (!CreateRootSignature()) return false;
    CreateFrames();
    CreateGammaLut();
    ImGui_ImplDX12_Init(dev_.Get(), kFrames, DXGI_FORMAT_B8G8R8A8_UNORM, ui_heap_.Get(),
                        ui_heap_->GetCPUDescriptorHandleForHeapStart(), ui_heap_->GetGPUDescriptorHandleForHeapStart());
    LoadSettings();
    Begin();
    refresh_ = RefreshRate();
    InterpFrameStart();
    StartPrebuild();
    StartWorkers();
    LOGI("d3d12: %ux%u guest, %ux%u window, render scale %.3f x %.3f", width, height, bb_w_, bb_h_, scale_.x,
         scale_.y);
    return true;
}

bool D3D12Backend::CreateDevice() {
    if (GetEnvironmentVariableW(L"XBE_D3D12_DEBUG", nullptr, 0)) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) return false;
    // dedicated GPU first on hybrid laptops
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(factory_.As(&f6))) {
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_))) &&
        FAILED(D3D12CreateDevice((adapter = nullptr).Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_)))) {
        LOGE("d3d12: no device");
        return false;
    }
    if (GetEnvironmentVariableW(L"XBE_D3D12_DEBUG", nullptr, 0)) dev_.As(&info_);
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) adapter->GetDesc1(&ad);
    LOGR("D3D12 adapter: %ls", adapter ? ad.Description : L"default");
    D3D12_FEATURE_DATA_D3D12_OPTIONS opt{};
    dev_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof(opt));
    // the graphics profile is the GPU's, not a setting: low on one sharing the system's memory (integrated)
    D3D12_FEATURE_DATA_ARCHITECTURE arch{};
    dev_->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch));
    app::Get().low_profile = arch.UMA;
    LOGR("d3d12: graphics profile %s, 16-bit shader math %s", app::Get().low_profile ? "low" : "high",
         (opt.MinPrecisionSupport & D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT) ? "supported" : "run at 32 bits");
    conservative_ok_ = opt.ConservativeRasterizationTier != D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;
    LOGR("d3d12: conservative rasterization %s", conservative_ok_ ? "supported" : "not supported, seams stay open");
    if (app::Get().fresh) FitToAdapter(ad);
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    ComPtr<IDXGIFactory5> f5;
    BOOL tear = FALSE;
    if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
    tearing_ = tear;
    return true;
}

// the first run: a GPU sharing the system's memory draws at most 720 lines, a small dedicated one 1080, the
// picture brought up to the output by the frame's upscale
void D3D12Backend::FitToAdapter(const DXGI_ADAPTER_DESC1& ad) {
    constexpr uint64_t kGb = 1ull << 30;
    D3D12_FEATURE_DATA_ARCHITECTURE arch{};
    dev_->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch));
    uint64_t vram = ad.DedicatedVideoMemory;
    uint32_t lines = 0;
    if (arch.UMA || vram < kGb + kGb / 2) lines = 720;
    else if (vram < 3 * kGb) lines = 1080;
    app::Settings& s = app::Get();
    s.fresh = false;
    uint32_t out = app::Output().height;
    if (!lines || !out || out <= lines) {
        LOGR("d3d12: %llu MB video memory%s: render scale kept", vram >> 20, arch.UMA ? ", shared" : "");
        if (arch.UMA) app::Save();
        return;
    }
    constexpr uint32_t kPercents[] = {100, 85, 75, 67, 50, 33, 25};
    uint32_t want = lines * 100 / out;
    uint32_t pick = kPercents[std::size(kPercents) - 1];
    for (uint32_t p : kPercents) {
        if (p <= want) {
            pick = p;
            break;
        }
    }
    s.render_percent = pick;
    LOGR("d3d12: %llu MB video memory%s: render scale %u%% for %u lines", vram >> 20, arch.UMA ? ", shared" : "", pick,
         out);
    app::Save();
}

bool D3D12Backend::CreateSwapChain(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    bb_w_ = std::max<LONG>(rc.right - rc.left, 1);
    bb_h_ = std::max<LONG>(rc.bottom - rc.top, 1);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bb_w_;
    sd.Height = bb_h_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = SwapFlags();
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
        LOGE("d3d12: no swap chain");
        return false;
    }
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&swap_);
    // the frames queued for the display at a time, waited for before each present (WaitLatency)
    latency_set_ = FrameLatency();
    swap_->SetMaximumFrameLatency(latency_set_);
    latency_wait_ = swap_->GetFrameLatencyWaitableObject();
    return true;
}

// Two frames queued with frames drawn between the title's: the presenter picks the image to show the moment there is
// room, a refresh at a time, and with one a present waited on the queue behind the title frame's runs and the display
// missed a refresh one time in five. Each frame presented as drawn (no frames between), one queued (low latency): the
// frame drawn after an input reaches the display a refresh sooner, above all under vertical sync and on a GPU that
// falls behind the title
uint32_t D3D12Backend::FrameLatency() const { return app::Get().low_latency && app::Get().frame_interp <= 1 ? 1 : 2; }

UINT D3D12Backend::SwapFlags() const {
    return DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
}

// until the display has room for another frame (a second at most, should it never)
void D3D12Backend::WaitLatency() {
    if (!latency_wait_) return;
    auto t = std::chrono::steady_clock::now();
    WaitForSingleObjectEx(latency_wait_, 1000, FALSE);
    wait_us_ += uint64_t(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t).count());
}

uint32_t D3D12Backend::CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible,
                                  ComPtr<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = t;
    hd.NumDescriptors = n;
    hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out));
    return dev_->GetDescriptorHandleIncrementSize(t);
}

void D3D12Backend::CreateBackBufferViews() {
    for (int i = 0; i < kFrames; i++) {
        swap_->GetBuffer(i, IID_PPV_ARGS(&bb_[i]));
        dev_->CreateRenderTargetView(bb_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kRtvCount + i, rtv_inc_));
    }
}

void D3D12Backend::CreateHeaps() {
    srv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount, true, srv_heap_);
    smp_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerCount, true, smp_heap_);
    rtv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvCount + kFrames + 1, false, rtv_heap_);
    dsv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCount, false, dsv_heap_);
    CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true, ui_heap_);
    for (uint32_t i = kRtvCount; i-- > 0;) free_rtv_.push_back(i);
    for (uint32_t i = kDsvCount; i-- > 0;) free_dsv_.push_back(i);
}

bool D3D12Backend::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[1].NumDescriptors = 4;
    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    for (int i = 0; i < 2; i++) {
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
    rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rs;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err))) return false;
    root_hash_ = Hash(rs->GetBufferPointer(), rs->GetBufferSize());
    dev_->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&root_));
    return true;
}

void D3D12Backend::CreateFrames() {
    for (auto& f : frames_) {
        dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd = BufferDesc(kUploadSize);
        dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&f.upload));
        f.upload->Map(0, nullptr, reinterpret_cast<void**>(&f.map));
    }
    dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t freq = 0;
    // always: a measurement can start with F9 at any time, and two timestamps a list cost next to nothing
    if (SUCCEEDED(queue_->GetTimestampFrequency(&freq)) && freq) {
        D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kFrames * kTimestamps, 0};
        D3D12_HEAP_PROPERTIES rp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd = BufferDesc(kFrames * kTimestamps * sizeof(uint64_t));
        void* map = nullptr;
        if (SUCCEEDED(dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&time_heap_))) &&
            SUCCEEDED(dev_->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&time_read_))) &&
            SUCCEEDED(time_read_->Map(0, nullptr, &map))) {
            time_map_ = static_cast<const uint64_t*>(map);
            tick_ms_ = 1000.0 / double(freq);
        } else {
            time_heap_ = nullptr;
        }
        D3D12_QUERY_HEAP_DESC md{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kFrames * kMarks, 0};
        D3D12_RESOURCE_DESC mrd = BufferDesc(kFrames * kMarks * sizeof(uint64_t));
        if (time_heap_ && SUCCEEDED(dev_->CreateQueryHeap(&md, IID_PPV_ARGS(&mark_heap_))) &&
            SUCCEEDED(dev_->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &mrd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&mark_read_))) &&
            SUCCEEDED(mark_read_->Map(0, nullptr, &map))) {
            mark_map_ = static_cast<const uint64_t*>(map);
        } else {
            mark_heap_ = nullptr;
        }
    }
    dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].alloc.Get(), nullptr, IID_PPV_ARGS(&cl_));
    for (auto& f : frames_) dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.xalloc));
    dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].xalloc.Get(), nullptr, IID_PPV_ARGS(&xl_));
    xl_->Close();
}

// gamma ramp as a 256 x 1 lookup
void D3D12Backend::CreateGammaLut() {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&lut_));
}

void D3D12Backend::LoadSettings() {
    scale_ = app::RenderScale(guest_w_, guest_h_);
    ext_ = g_extension.get();
    if (ext_) {
        ext_->Settings();
        ext_->Reset(*this);
    }
}

void D3D12Backend::Begin() {
    Frame& f = CurrentFrame();
    if (time_heap_ && f.queries + 2 <= kTimestamps)
        cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, uint32_t(&f - frames_) * kTimestamps + f.queries++);
    f.list_marks = f.marks;
    WriteMark(false);
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    bound_rt_ = bound_ds_ = nullptr;
    last_vsc_gpu_ = last_dc_gpu_ = 0;
    binds_ = {};
}

// the list ends: its timestamps resolved
void D3D12Backend::CloseList() {
    Frame& f = CurrentFrame();
    if (time_heap_ && f.queries % 2) {
        uint32_t first = uint32_t(&f - frames_) * kTimestamps;
        cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + f.queries++);
        cl_->ResolveQueryData(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + f.queries - 2, 2,
                              time_read_.Get(), (first + f.queries - 2) * sizeof(uint64_t));
    }
    if (f.marks > f.list_marks) {
        WriteMark(true);
        uint32_t first = uint32_t(&f - frames_) * kMarks;
        cl_->ResolveQueryData(mark_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + f.list_marks,
                              f.marks - f.list_marks, mark_read_.Get(), (first + f.list_marks) * sizeof(uint64_t));
        f.list_marks = f.marks;
    }
    cl_->Close();
}

void D3D12Backend::Mark(GpuPass p, uint64_t tag) {
    if (p == pass_ && tag == pass_tag_) return;
    pass_ = p;
    pass_tag_ = tag;
    WriteMark(false);
}

void D3D12Backend::WriteMark(bool end) {
    if (!mark_heap_ || !diag::Perf()) return;
    Frame& f = CurrentFrame();
    // one kept for the list's end
    if (f.marks + (end ? 1 : 2) > kMarks) return;
    cl_->EndQuery(mark_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, uint32_t(&f - frames_) * kMarks + f.marks);
    f.mark_pass[f.marks] = pass_;
    f.mark_tag[f.marks] = pass_tag_;
    f.mark_end[f.marks] = end;
    f.marks++;
}

void D3D12Backend::Submit(bool wait) {
    Frame& f = CurrentFrame();
    // the frame's draws so far read their constants from the arena: filled before they run, and the frame is run once
    if (interp_whole_ && interp_slots_) {
        BeginExtra();
        FillArena(0);
        xl_->Close();
        RunList(xl_.Get());
    }
    if (wait) InterpSplit();
    CloseList();
    RunList(cl_.Get());
    queue_->Signal(fence_.Get(), ++fence_value_);
    f.fence = fence_value_;
    if (wait) {
        WaitFence(fence_value_);
        fit_partial_ms_ += ReadGpuTime(f);
        f.alloc->Reset();
        f.xalloc->Reset();
        f.garbage.clear();
        f.used = 0;
        f.srv = 0;
        view_sets_.clear();
        cl_->Reset(f.alloc.Get(), nullptr);
        Begin();
    }
}

void D3D12Backend::Flush() { Submit(true); }

void D3D12Backend::WaitFence(uint64_t value) {
    fence_->SetEventOnCompletion(value, event_);
    WaitForSingleObject(event_, INFINITE);
}

void D3D12Backend::SetFullViewport(uint32_t w, uint32_t h) {
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    binds_ = {};
}

// one w x h colour target for a Blit, the draw's bound targets forgotten
void D3D12Backend::SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h) {
    cl_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(w, h);
}

D3D12Backend::Alloc D3D12Backend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    g_perf.upload += size;
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload.Get(), o, f.map + o, f.upload->GetGPUVirtualAddress() + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    g_perf.overflow++;
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    constexpr uint64_t kOwnAlign = 0x10000;  // a committed buffer's size granularity
    D3D12_RESOURCE_DESC bd = BufferDesc((size + kOwnAlign - 1) & ~(kOwnAlign - 1));
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                  IID_PPV_ARGS(&r));
    uint8_t* p = nullptr;
    r->Map(0, nullptr, reinterpret_cast<void**>(&p));
    f.garbage.push_back(r);
    return {r.Get(), 0, p, r->GetGPUVirtualAddress()};
}

uint32_t D3D12Backend::AllocSrvSlots(uint32_t n) {
    Frame& f = CurrentFrame();
    const uint32_t per = kSrvCount / kFrames;
    if (f.srv + n > per) {
        LOG_FIRST(4, "d3d12: descriptor ring wrapped within a frame");
        f.srv = 0;
        view_sets_.clear();
    }
    uint32_t i = uint32_t(frame_ % kFrames) * per + f.srv;
    f.srv += n;
    return i;
}

void D3D12Backend::Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) {
    if (cur == want) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter = want;
    cl_->ResourceBarrier(1, &b);
    if (track_states_) {
        auto [it, fresh] = frame_states_.try_emplace(r, cur, want);
        if (!fresh) it->second.second = want;
    }
    cur = want;
}

// a resized target gives back its view and its resources
void D3D12Backend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
    (it->second.depth ? free_dsv_ : free_rtv_).push_back(it->second.view);
    if (it->second.fb_view != ~0u) free_dsv_.push_back(it->second.fb_view);
    CurrentFrame().garbage.push_back(it->second.res);
    if (it->second.fb) CurrentFrame().garbage.push_back(it->second.fb);
    if (it->second.shrunk) CurrentFrame().garbage.push_back(it->second.shrunk);
    targets_.erase(it);
}

uint32_t Ms(const Target& t) { return t.samples; }

// the host texture of a new target, at render scale or the extension's; the scene and its depth multisampled
bool D3D12Backend::CreateTargetResource(Target& t, const Surface& s) {
    // an extension's scale is a vertical one: the horizontal keeps the render scale's proportion to it
    float sc = scale_.y;
    if (ext_) ext_->MakeTarget(s, t.depth, t.gw, t.gh, sc, t.fmt);
    // the screen at half size or less, the screen's shape (Future Perfect's glow chain, 320x240 then 160x120, and
    // the smoke drawn into it): a blur, kept at what 1080p gives it, its colour and depth alike. At 4K it would take
    // a third of the frame for no difference seen.
    constexpr float kBlurScale = 1080.0f / 480.0f;
    if (!ext_ && t.gw * 2 <= guest_w_ && t.gh * 2 <= guest_h_ && t.gw * guest_h_ == t.gh * guest_w_)
        sc = std::min(sc, kBlurScale);
    t.w =std::max(1u, uint32_t(std::lround(t.gw * scale_.x * sc / scale_.y)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * sc)));
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_UNKNOWN, t.w, t.h);
    // the game's smaller maps, shadows and blur chain stay single sampled
    t.samples = t.gw == guest_w_ && t.gh == guest_h_ ? msaa_ : 1;
    td.SampleDesc.Count = t.samples;
    D3D12_CLEAR_VALUE cv{};
    if (t.depth) {
        td.Format = kDepthTypeless;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        cv.Format = kDepthFormat;
        cv.DepthStencil.Depth = kDepthFar;
        t.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else {
        td.Format = t.fmt;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        cv.Format = td.Format;
        t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.state, &cv, IID_PPV_ARGS(&t.res)))) {
        LOGE("d3d12: no %ux%u target with %u samples", t.w, t.h, td.SampleDesc.Count);
        return false;
    }
    return true;
}

uint32_t D3D12Backend::Samples(uint32_t want) {
    auto supported = [&](uint32_t n) {
        for (DXGI_FORMAT f : {kBgra, kDepthFormat}) {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{f, n};
            if (FAILED(dev_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) ||
                !q.NumQualityLevels) {
                return false;
            }
        }
        return true;
    };
    uint32_t n = 1;
    for (uint32_t c : {8u, 4u, 2u}) {
        if (c <= want && supported(c)) {
            n = c;
            break;
        }
    }
    if (want > 1) LOGR("d3d12: MSAA %ux (asked %u)", n, want);
    return n;
}

// a view from the free pool, cleared; false when the pool is empty
bool D3D12Backend::CreateTargetView(Target& t) {
    auto& pool = t.depth ? free_dsv_ : free_rtv_;
    if (pool.empty()) return false;
    t.view = pool.back();
    pool.pop_back();
    if (t.depth) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = kDepthFormat;
        dv.ViewDimension = Ms(t) > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        dev_->CreateDepthStencilView(t.res.Get(), &dv, Cpu(dsv_heap_.Get(), t.view, dsv_inc_));
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_),
                                   D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, kDepthFar, 0, 0, nullptr);
    } else {
        dev_->CreateRenderTargetView(t.res.Get(), nullptr, Cpu(rtv_heap_.Get(), t.view, rtv_inc_));
        float black[4] = {0, 0, 0, 0};
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), black, 0, nullptr);
    }
    return true;
}

// targets_ key of a surface's colour or depth target
uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// a smaller depth surface's console size, above the address bits
constexpr int kKeyWidthShift = 34;
constexpr int kKeyHeightShift = 48;

// a smaller depth surface laid on the depth buffer's memory is a target of its own
uint64_t D3D12Backend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = TargetMapKey(data, depth);
    bool small_depth = depth && (gw != guest_w_ || gh != guest_h_);
    if (!small_depth) return key;
    return key | uint64_t(gw) << kKeyWidthShift | uint64_t(gh) << kKeyHeightShift;
}

// alt: the extension's bits of the key
D3D12Backend::TargetSlot D3D12Backend::FindTargetSlot(const Surface& s, bool depth, uint64_t alt, uint32_t gw,
                                                      uint32_t gh) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    SurfaceSize(s, slot.gw, slot.gh);
    if (gw && gh) {
        slot.gw = gw;
        slot.gh = gh;
    }
    slot.key = SizedTargetKey(slot.data, depth, slot.gw, slot.gh) | alt;
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

// the surface's target at its size, null when there is none
Target* D3D12Backend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt);
    if (!slot.sized) return nullptr;
    UpdateTargetUsed(slot.data);
    return &slot.it->second;
}

// the surface's target, made again at its size when that changed
Target* D3D12Backend::EnsureTarget(const Surface& s, bool depth, uint32_t gw, uint32_t gh) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt, gw, gh);
    UpdateTargetUsed(slot.data);
    if (slot.sized) return &slot.it->second;
    if (slot.it != targets_.end()) ReleaseTarget(slot.it);
    Target t;
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!ReuseTarget(t)) {
        if (!CreateTargetResource(t, s)) return nullptr;
        g_perf.targets++;
        // an empty pool drops the new resource with t
        if (!CreateTargetView(t)) return nullptr;
    }
    t.data = slot.data;
    t.bytes = GuestPitch(slot.gw) * slot.gh;
    if (depth) t.bytes = slot.gw * slot.gh * 4;
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    auto& ref = targets_[slot.key] = std::move(t);
    return &ref;
}

// the depth buffer under rt: a colour target smaller than the depth buffer (a security camera's or a monitor's
// picture) uses its top-left corner on the console, here a depth target of the colour target's size laid on the same
// memory, which the game clears before drawing that picture
Target* D3D12Backend::EnsureDepth(const State& s, const Target* rt) {
    if (rt && s.zs.header) {
        uint32_t gw;
        uint32_t gh;
        SurfaceSize(s.zs, gw, gh);
        if ((rt->gw < gw || rt->gh < gh) && rt->gw <= gw && rt->gh <= gh)
            return EnsureTarget(s.zs, true, rt->gw, rt->gh);
    }
    return EnsureTarget(s.zs, true);
}

bool D3D12Backend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureDepth(s, rt) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h || Ms(*rt) != Ms(*ds))) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rt->drawn = true;
    }
    if (ds) {
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        TakeDepthClear(*ds);
        ds->drawn = true;
    }
    if (rt != bound_rt_ || ds != bound_ds_) {
        D3D12_CPU_DESCRIPTOR_HANDLE rh = rt ? Cpu(rtv_heap_.Get(), rt->view, rtv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        D3D12_CPU_DESCRIPTOR_HANDLE dh = ds ? Cpu(dsv_heap_.Get(), ds->view, dsv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        cl_->OMSetRenderTargets(rt ? 1 : 0, rt ? &rh : nullptr, FALSE, ds ? &dh : nullptr);
        bound_rt_ = rt;
        bound_ds_ = ds;
    }
    Mark(PassOf(rt ? *rt : *ds), TargetTag(rt ? *rt : *ds));
    return true;
}

// DepthPass's pipeline, kept in blit_psos_: writes depth only, always passing; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsureDepthCopyPipeline(bool ms, uint32_t samples) {
    auto& pso = blit_psos_[~(uint64_t(ms) | uint64_t(samples) << 1)];
    if (pso) return pso.Get();
    std::string hlsl = std::string(ms ? "#define MS\n" : "") + kDepthHlsl;
    uint64_t h;
    ID3DBlob* vs = Compile(hlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(hlsl, "psmain", "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root_.Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.SampleMask = UINT_MAX;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthEnable = TRUE;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.DSVFormat = kDepthFormat;
    p.SampleDesc.Count = samples;
    if (FAILED(dev_->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

bool D3D12Backend::DepthPass(ID3D12Resource* src, bool src_ms, D3D12_CPU_DESCRIPTOR_HANDLE dh, uint32_t w,
                             uint32_t h, uint32_t samples) {
    ID3D12PipelineState* pso = EnsureDepthCopyPipeline(src_ms, samples);
    if (!pso) return false;
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &dh);
    SetFullViewport(w, h);
    uint32_t slot = AllocSrvSlots(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = kDepthRead;
    sv.ViewDimension = src_ms ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (!src_ms) sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    Alloc cb = Upload(256, 256);
    DrawFullScreen(pso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
    return true;
}

// src's depth drawn into dst's depth buffer
bool D3D12Backend::DrawDepthCopy(Target& src, Target& dst) {
    if (cap_) fprintf(cap_, "%04u depth copy %08X to %08X\n", cap_i_++, src.data, dst.data);
    // a multisampled source is read in place, or its copy when that is current, rather than copied first
    bool in_place = Ms(src) > 1 && !(src.fb && src.state == CopiedFrom(src));
    D3D12_RESOURCE_STATES* st = &src.state;
    ID3D12Resource* from = in_place ? src.res.Get() : Readable(src, st);
    if (!from) return false;
    // read in place, the target takes a state of its own: its copy is not made current by this
    Transition(from, *st,
               in_place ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ
                        : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst.res.Get(), dst.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (!DepthPass(from, in_place, Cpu(dsv_heap_.Get(), dst.view, dsv_inc_), dst.w, dst.h, Ms(dst))) return false;
    dst.drawn = true;
    return true;
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool D3D12Backend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1) {
        return false;
    }
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(s.rt, gw, gh);
    auto di = targets_.find(SizedTargetKey(mem::VirtualFromPhysical(s.rt.data), true, gw, gh));
    auto si = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    Target& dst = di->second;
    Target& src = si->second;
    return DrawDepthCopy(src, dst);
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

// BGRA pixels into a new w x h texture, left as a shader resource
ComPtr<ID3D12Resource> D3D12Backend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w,
                                                        uint32_t h) {
    ComPtr<ID3D12Resource> tex;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&tex));
    uint32_t row = UploadPitch(w * 4);
    Alloc a = Upload(uint64_t(row) * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * row, &px[size_t(y) * pitch], w * 4);
    D3D12_TEXTURE_COPY_LOCATION dst{tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint.Offset = a.off;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, row};
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tex.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return tex;
}

// CPU pixels of a color surface into its host target, scaled
void D3D12Backend::UploadTarget(Target& t, const Surface& s) {
    g_perf.rtups++;
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "d3d12: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ComPtr<ID3D12Resource> tex = CreatePixelTexture(px, pitch, d.width, d.height);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    float mul = 1 / t.range;
    Blit(tex.Get(), kBgra, uv, {.alpha = true, .mul = mul, .rtv = t.fmt, .over = over, .samples = Ms(t)});
    CurrentFrame().garbage.push_back(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// samplers_ key of the full screen passes' samplers, apart from the stage samplers' hashed keys
constexpr uint64_t kBlitSamplerKey = 0xB117;

// four clamp samplers: linear at slot 0, point at slots 1 to 3
uint32_t D3D12Backend::EnsureBlitSamplers() {
    uint64_t key = kBlitSamplerKey;
    auto it = samplers_.find(key);
    uint32_t si;
    if (it == samplers_.end()) {
        if (smp_used_ + 4 > kSamplerCount) {
            samplers_.clear();
            smp_used_ = 0;
        }
        si = smp_used_;
        smp_used_ += 4;
        for (int i = 0; i < 4; i++) {
            D3D12_SAMPLER_DESC sd{};
            sd.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            dev_->CreateSampler(&sd, Cpu(smp_heap_.Get(), si + i, smp_inc_));
        }
        samplers_[key] = si;
    } else {
        si = it->second;
    }
    return si;
}

// the full screen triangle: cb at b0 and b1, views at srv_slot; without a sampler slot EnsureBlitSamplers gives it
void D3D12Backend::DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                                  std::optional<uint32_t> sampler_slot) {
    binds_ = {};
    cl_->SetPipelineState(pso);
    cl_->SetGraphicsRootConstantBufferView(0, cb);
    cl_->SetGraphicsRootConstantBufferView(1, cb);
    cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), srv_slot, srv_inc_));
    // without a slot the passes take the blit samplers, looked up at this point as they always were
    uint32_t si = sampler_slot ? *sampler_slot : EnsureBlitSamplers();
    cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), si, smp_inc_));
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl_->DrawInstanced(3, 1, 0, 0);
}

void D3D12Backend::Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o) {
    uint32_t slot = AllocSrvSlots(2);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    sv.Format = kBgra;
    dev_->CreateShaderResourceView(lut_.Get(), &sv, Cpu(srv_heap_.Get(), slot + 1, srv_inc_));
    uint32_t si = EnsureBlitSamplers();
    Alloc cb = Upload(256, 256);
    float c[12] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3],
                   o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f, o.fxaa ? -2.0f : o.cubic ? -1.0f : float(o.taps), o.mul,
                   o.power, 0, 0, 0};
    std::memcpy(cb.cpu, c, sizeof(c));
    DrawFullScreen(EnsureBlitPipeline(o.rtv, o.over, o.samples), cb.gpu, slot, si);
}

constexpr char kShaderFileMagic[8] = {'X', 'B', 'E', 'S', 'H', 'D', '0', '1'};
constexpr UINT kCompileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL1;

void D3D12Backend::OpenShaderFile() {
    shader_file_read_ = true;
    std::filesystem::path path = app::ExeDir() / "shaders.bin";
    uint32_t flags = 0;
    auto read = [&](const std::filesystem::path& from) {
        std::FILE* f = _wfopen(from.c_str(), L"rb");
        if (!f) return;
        char magic[8] = {};
        bool ok = std::fread(magic, 1, 8, f) == 8 && std::fread(&flags, 4, 1, f) == 1 &&
                  std::memcmp(magic, kShaderFileMagic, 8) == 0 && flags == kCompileFlags;
        // a record cut short ends the file: a run that stopped while writing it
        for (;;) {
            uint64_t hash;
            uint32_t src_size, size;
            if (!ok || std::fread(&hash, 8, 1, f) != 1 || std::fread(&src_size, 4, 1, f) != 1 ||
                std::fread(&size, 4, 1, f) != 1 || size > (16u << 20))
                break;
            ComPtr<ID3DBlob> code;
            if (FAILED(D3DCreateBlob(size, &code)) || std::fread(code->GetBufferPointer(), 1, size, f) != size) break;
            disk_shaders_.emplace(hash, DiskShader{src_size, code});
        }
        std::fclose(f);
    };
    // the player's own first, then the ones shipped with the game, joined into the player's file below
    read(path);
    read(app::ExeDir() / "shaders_seed.bin");
    // rewritten whole: drops a cut record and the shaders of another format; a second instance only reads it
    if (app::Instance() > 1) return;
    shader_file_ = _wfopen(path.c_str(), L"wb");
    if (!shader_file_) return;
    flags = kCompileFlags;
    std::fwrite(kShaderFileMagic, 1, 8, shader_file_);
    std::fwrite(&flags, 4, 1, shader_file_);
    for (auto& [hash, d] : disk_shaders_) {
        uint32_t size = uint32_t(d.code->GetBufferSize());
        std::fwrite(&hash, 8, 1, shader_file_);
        std::fwrite(&d.src_size, 4, 1, shader_file_);
        std::fwrite(&size, 4, 1, shader_file_);
        std::fwrite(d.code->GetBufferPointer(), 1, size, shader_file_);
    }
    std::fflush(shader_file_);
    LOGR("d3d12: %zu shaders from shaders.bin", disk_shaders_.size());
}

ID3DBlob* D3D12Backend::Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash,
                                 bool later) {
    // one source can hold several entry points
    uint64_t profile_seed = Hash(profile, std::strlen(profile));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    hash = Hash(src.data(), src.size(), entry_seed);
    auto it = blobs_.find(hash);
    if (it != blobs_.end()) return it->second.Get();
    if (!shader_file_read_) OpenShaderFile();
    auto dit = disk_shaders_.find(hash);
    if (dit != disk_shaders_.end() && dit->second.src_size == uint32_t(src.size())) {
        blobs_[hash] = dit->second.code;
        return dit->second.code.Get();
    }
    // the threads' to compile: nothing yet, the caller tells it from a failure by blobs_ not holding the hash
    if (later && !workers_.empty()) {
        if (shaders_queued_.insert(hash).second) {
            {
                std::lock_guard lock(work_lock_);
                shader_jobs_.push_back({src, entry, profile, hash});
            }
            work_cv_.notify_one();
            later_.shaders++;
        }
        return nullptr;
    }
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> err;
    PerfScope perf_compile{g_perf.compile};
    g_perf.shaders++;
    auto build_start = PerfClock::now();
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, profile, kCompileFlags, 0,
                            &code, &err);
    builds_.shaders++;
    CountBuild(build_start);
    if (FAILED(hr)) {
        LOGE("d3d12: %s compile failed: %s", profile, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        LOGE("%s", src.c_str());
        blobs_[hash] = nullptr;
        return nullptr;
    }
    blobs_[hash] = code;
    WriteShader(hash, uint32_t(src.size()), code.Get());
    return code.Get();
}

void D3D12Backend::WriteShader(uint64_t hash, uint32_t src_size, ID3DBlob* code) {
    if (!shader_file_) return;
    uint32_t size = uint32_t(code->GetBufferSize());
    std::fwrite(&hash, 8, 1, shader_file_);
    std::fwrite(&src_size, 4, 1, shader_file_);
    std::fwrite(&size, 4, 1, shader_file_);
    std::fwrite(code->GetBufferPointer(), 1, size, shader_file_);
    std::fflush(shader_file_);
}

// a full screen pass's pipeline: solid, no culling, one target, writing all channels; callers set the blend
D3D12_GRAPHICS_PIPELINE_STATE_DESC D3D12Backend::FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv,
                                                                        uint32_t samples) const {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = rtv;
    d.SampleDesc.Count = samples;
    return d;
}

ID3D12PipelineState* D3D12Backend::EnsureBlitPipeline(DXGI_FORMAT rtv, bool over, uint32_t samples) {
    auto& pso = blit_psos_[uint64_t(rtv) | uint64_t(over) << 32 | uint64_t(samples) << 40];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kBlitHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kBlitHlsl, "psmain", "ps_5_0", h);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv, samples);
    if (over) {
        auto& blend = d.BlendState.RenderTarget[0];
        // premultiplied colour, the target keeps its alpha
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    return pso.Get();
}

// Pass's pipeline for the shader, kept in blit_psos_; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsurePassPipeline(const char* hlsl, const char* entry, DXGI_FORMAT rtv,
                                                      float blend, uint32_t samples) {
    uint64_t seed = uint64_t(rtv) << 2 | (blend >= 0 ? 1 : blend == kPassMultiply ? 2 : 0) | uint64_t(samples) << 40;
    uint64_t source_seed = Hash(hlsl, std::strlen(hlsl), seed);
    auto& pso = blit_psos_[Hash(entry, std::strlen(entry), source_seed)];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(hlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(hlsl, entry, "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv, samples);
    if (blend >= 0) {
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
        b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    } else if (blend == kPassMultiply) {
        // the target's colour times the pass's, its alpha kept
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_ZERO;
        b.DestBlend = D3D12_BLEND_SRC_COLOR;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

void D3D12Backend::Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh,
                        uint32_t w, uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3],
                        const float p[8], float blend, uint32_t samples) {
    ID3D12PipelineState* pso = EnsurePassPipeline(hlsl, entry, rtv, blend, samples);
    if (!pso) return;
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 3 && src[i] ? fmt[i] : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 3 ? src[i] : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, p, 8 * sizeof(float));
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(w, h);
    if (blend >= 0) {
        float f[4] = {blend, blend, blend, blend};
        cl_->OMSetBlendFactor(f);
        binds_ = {};
    }
    DrawFullScreen(pso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
}

// a copy of the target to read while drawing to it
ID3D12Resource* D3D12Backend::Feedback(Target& t) {
    // the stages of one draw sampling the target read one copy, and so do the draws of one screen effect in a row (the
    // rain drops on the screen in Mansion of Madness, each tinting the picture as it was): a copy per draw has each
    // drop read the ones before it and the screen darkens as they pile up; any other draw in between makes a new copy
    bool kept = t.fb_draw && t.fb_draw + 1 >= draw_serial_;
    t.fb_draw = draw_serial_;
    if (kept && t.fb && t.fb_state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) return t.fb.Get();
    return RefreshCopy(t);
}

ID3D12Resource* D3D12Backend::RefreshCopy(Target& t) {
    PassScope pass{*this, GpuPass::Resolve, TargetTag(t)};
    bool ms = Ms(t) > 1;
    // a multisampled depth target's copy is drawn into, through a view of its own
    bool by_draw = t.depth && ms;
    if (!t.fb) {
        if (by_draw && free_dsv_.empty()) return nullptr;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = t.res->GetDesc();
        td.SampleDesc.Count = 1;
        td.Alignment = 0;
        td.Flags = by_draw ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE;
        t.fb_state = by_draw ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.fb_state, nullptr,
                                                 IID_PPV_ARGS(&t.fb)))) {
            return nullptr;
        }
        if (by_draw) {
            t.fb_view = free_dsv_.back();
            free_dsv_.pop_back();
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
            dv.Format = kDepthFormat;
            dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dev_->CreateDepthStencilView(t.fb.Get(), &dv, Cpu(dsv_heap_.Get(), t.fb_view, dsv_inc_));
        }
    }
    if (by_draw) {
        Transition(t.res.Get(), t.state, CopiedFrom(t));
        Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        if (!DepthPass(t.res.Get(), true, Cpu(dsv_heap_.Get(), t.fb_view, dsv_inc_), t.w, t.h, 1)) return nullptr;
    } else if (ms) {
        Transition(t.res.Get(), t.state, CopiedFrom(t));
        Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        cl_->ResolveSubresource(t.fb.Get(), 0, t.res.Get(), 0, t.fmt);
    } else {
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_COPY_DEST);
        cl_->CopyResource(t.fb.Get(), t.res.Get());
    }
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return t.fb.Get();
}

ID3D12Resource* D3D12Backend::Readable(Target& t, D3D12_RESOURCE_STATES*& st) {
    if (Ms(t) <= 1) {
        st = &t.state;
        return t.res.Get();
    }
    st = &t.fb_state;
    if (t.fb && t.state == CopiedFrom(t)) return t.fb.Get();
    return RefreshCopy(t);
}

// the key's vertex attributes, ATTR0 to ATTR15 in register order; returns how many
uint32_t FillInputLayout(const PsoKey& k, D3D12_INPUT_ELEMENT_DESC el[16]) {
    uint32_t n = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        el[n++] = {"ATTR", r, DXGI_FORMAT(e & 0xFF), (e >> 8) & 0xFF, e >> 16,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    }
    return n;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(uint32_t topo) {
    switch (topo) {
        case 0: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case 1: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
// PsoKey::fill: drawn with conservative rasterization (app::Settings::seal_seams), above the D3DFILL values
constexpr uint32_t kFillSealed = 0x80000000u;
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW

// a D3DCMP test on reversed depth: less and greater trade places
D3D12_COMPARISON_FUNC ReversedCmp(uint32_t v) {
    static const uint8_t mirror[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return D3D12_COMPARISON_FUNC(mirror[Cmp(v) - 1] + 1);
}

void FillDepthStencilDesc(const PsoKey& k, D3D12_DEPTH_STENCIL_DESC& ds) {
    ds.DepthEnable = k.zen;
    ds.DepthWriteMask = k.zwrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = ReversedCmp(k.zfunc);
    ds.StencilEnable = k.sten;
    ds.StencilReadMask = UINT8(k.smask);
    ds.StencilWriteMask = UINT8(k.swmask);
    D3D12_DEPTH_STENCILOP_DESC so{StencilOp(k.sfail), StencilOp(k.szfail), StencilOp(k.spass), Cmp(k.sfunc)};
    ds.FrontFace = ds.BackFace = so;
}

// everything of the pipeline but its input layout
void D3D12Backend::FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps,
                                    D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const {
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& b = d.BlendState.RenderTarget[0];
    b.BlendEnable = k.blend;
    b.SrcBlend = Blend(k.src, false);
    b.DestBlend = Blend(k.dst, false);
    b.BlendOp = BlendOp(k.op);
    b.SrcBlendAlpha = Blend(k.src, true);
    b.DestBlendAlpha = Blend(k.dst, true);
    b.BlendOpAlpha = BlendOp(k.op);
    b.RenderTargetWriteMask = UINT8(k.wmask);
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = k.fill == kFillWireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = k.cull ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = k.ccw;
    d.RasterizerState.DepthBias = k.zbias;
    d.RasterizerState.SlopeScaledDepthBias = k.zslope;
    d.RasterizerState.DepthClipEnable = TRUE;
    if ((k.fill & kFillSealed) && conservative_ok_)
        d.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
    FillDepthStencilDesc(k, d.DepthStencilState);
    d.PrimitiveTopologyType = TopologyType(k.topo);
    d.NumRenderTargets = k.has_rt ? 1 : 0;
    d.RTVFormats[0] = k.has_rt ? DXGI_FORMAT(k.rtfmt) : DXGI_FORMAT_UNKNOWN;
    d.DSVFormat = k.has_ds ? kDepthFormat : DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = std::max(k.msaa, 1u);
}

// a point or line drawn by a geometry shader is solid, front facing and never culled
void ApplyGeometryShader(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, ID3DBlob* gs) {
    d.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = FALSE;
}

// a point is drawn as its whole console pixel by a geometry shader, solid, front facing and never culled
void D3D12Backend::ExpandPoints(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
    uint64_t hash;
    ID3DBlob* gs = Compile(PointHlsl(), "main", "gs_5_0", hash);
    if (gs) ApplyGeometryShader(d, gs);
}

// a line is drawn a console pixel wide by a geometry shader, solid and never culled: the rain's streaks were one host
// pixel, faint on an upscaled target
void D3D12Backend::ExpandLines(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
    uint64_t hash;
    ID3DBlob* gs = Compile(LineHlsl(), "main", "gs_5_0", hash);
    if (!gs) {
        LOGE("d3d12: the line geometry shader did not compile, lines stay a pixel wide");
        return;
    }
    static bool told = false;
    if (!told) LOGR("d3d12: lines drawn a console pixel wide");
    told = true;
    ApplyGeometryShader(d, gs);
}

app::Scale2 D3D12Backend::TargetScale() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw || !t->gh) return {1, 1};
    return {float(t->w) / float(t->gw), float(t->h) / float(t->gh)};
}

// the XDK's screen space offset puts the console's positions half a pixel right and down of the host's, unless a
// program writes them without c-37; render target reads move back that far (TexUv)
app::Scale2 D3D12Backend::RenderTargetShift(const State& s, const DrawShaders& sh) const {
    bool offset = !sh.prog || ReadsViewportOffset(s.vs->program);
    if (!offset) return {0, 0};
    app::Scale2 sc = TargetScale();
    return {-0.5f * sc.x, -0.5f * sc.y};
}

ID3D12PipelineState* D3D12Backend::EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps, bool* later) {
    uint64_t h = Hash(&k, sizeof(k));
    auto it = psos_.find(h);
    if (it != psos_.end()) return it->second.Get();
    if (ID3D12PipelineState* made = TakePrebuilt(h)) return made;
    if (later && !workers_.empty()) {
        // loaded from the library here, quick; built by the threads
        if (!pso_lib_tried_) OpenPipelineLibrary();
        if (!pipelines_queued_.count(h)) {
            PipelineJob j{k, h, vs, ps, nullptr};
            uint64_t gh;
            if (k.topo == uint32_t(Topology::Points) && !k.raw_points) j.gs = Compile(PointHlsl(), "main", "gs_5_0", gh);
            if (k.topo == uint32_t(Topology::Lines)) j.gs = Compile(LineHlsl(), "main", "gs_5_0", gh);
            D3D12_INPUT_ELEMENT_DESC el[16];
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
            d.InputLayout = {el, FillInputLayout(k, el)};
            FillPipelineDesc(k, vs, ps, d);
            if (j.gs) ApplyGeometryShader(d, j.gs.Get());
            wchar_t name[17];
            swprintf(name, std::size(name), L"%016llX", (unsigned long long)h);
            ComPtr<ID3D12PipelineState> pso;
            auto build_start = PerfClock::now();
            if (pso_lib_ && SUCCEEDED(pso_lib_->LoadGraphicsPipeline(name, &d, IID_PPV_ARGS(&pso)))) {
                psos_[h] = pso;
                builds_.loaded++;
                CountBuild(build_start);
                NotePipelineKey(h, k);
                return pso.Get();
            }
            pipelines_queued_.insert(h);
            {
                std::lock_guard lock(work_lock_);
                pipeline_jobs_.push_back(std::move(j));
            }
            work_cv_.notify_one();
            later_.pipelines++;
        }
        *later = true;
        return nullptr;
    }
    D3D12_INPUT_ELEMENT_DESC el[16];
    uint32_t n = FillInputLayout(k, el);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.InputLayout = {el, n};
    FillPipelineDesc(k, vs, ps, d);
    if (k.topo == uint32_t(Topology::Points) && !k.raw_points) ExpandPoints(d);
    if (k.topo == uint32_t(Topology::Lines)) ExpandLines(d);
    ComPtr<ID3D12PipelineState> pso;
    if (!pso_lib_tried_) OpenPipelineLibrary();
    wchar_t name[17];
    swprintf(name, std::size(name), L"%016llX", (unsigned long long)h);
    auto build_start = PerfClock::now();
    if (pso_lib_ && SUCCEEDED(pso_lib_->LoadGraphicsPipeline(name, &d, IID_PPV_ARGS(&pso)))) {
        psos_[h] = pso;
        builds_.loaded++;
        CountBuild(build_start);
        NotePipelineKey(h, k);
        return pso.Get();
    }
    g_perf.psos++;
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) LOGE("d3d12: pipeline creation failed");
    builds_.built++;
    CountBuild(build_start);
    psos_[h] = pso;
    if (pso) NotePipelineKey(h, k);
    if (pso) StorePipeline(h, pso.Get());
    return pso.Get();
}

void D3D12Backend::StorePipeline(uint64_t h, ID3D12PipelineState* pso) {
    if (!pso_lib_) return;
    wchar_t name[17];
    swprintf(name, std::size(name), L"%016llX", (unsigned long long)h);
    // the name taken by a pipeline that no longer matches (a changed build): a new library from here on
    if (FAILED(pso_lib_->StorePipeline(name, pso))) {
        ComPtr<ID3D12Device1> dev1;
        pso_lib_.Reset();
        if (SUCCEEDED(dev_.As(&dev1)) && SUCCEEDED(dev1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&pso_lib_))))
            pso_lib_->StorePipeline(name, pso);
    }
    pso_lib_dirty_ = true;
    pso_lib_stored_ = PerfClock::now();
}

void D3D12Backend::StartWorkers() {
    static const bool sync = [] {
        const char* e = std::getenv("XBE_SYNC_SHADERS");
        return e && *e && *e != '0';
    }();
    if (sync) {
        LOGR("d3d12: shaders and pipelines made on the render thread (XBE_SYNC_SHADERS)");
        return;
    }
    // one or two, leaving the title and render threads their cores
    unsigned cores = std::thread::hardware_concurrency();
    unsigned n = cores >= 8 ? 2 : 1;
    for (unsigned i = 0; i < n; i++) workers_.emplace_back([this] { WorkerLoop(); });
    LOGR("d3d12: %u threads make new shaders and pipelines, their draws left out meanwhile", n);
}

void D3D12Backend::StopWorkers() {
    {
        std::lock_guard lock(work_lock_);
        workers_stop_ = true;
    }
    work_cv_.notify_all();
    for (std::thread& t : workers_) t.join();
    workers_.clear();
}

void D3D12Backend::WorkerLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::unique_lock lock(work_lock_);
    for (;;) {
        work_cv_.wait(lock, [&] { return workers_stop_ || !shader_jobs_.empty() || !pipeline_jobs_.empty(); });
        if (workers_stop_) return;
        if (!shader_jobs_.empty()) {
            ShaderJob j = std::move(shader_jobs_.front());
            shader_jobs_.pop_front();
            lock.unlock();
            ComPtr<ID3DBlob> code, err;
            if (FAILED(D3DCompile(j.src.data(), j.src.size(), nullptr, nullptr, nullptr, j.entry, j.profile,
                                  kCompileFlags, 0, &code, &err))) {
                LOGE("d3d12: %s compile failed: %s", j.profile,
                     err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                LOGE("%s", j.src.c_str());
                code.Reset();
            }
            lock.lock();
            shaders_done_.push_back({j.hash, uint32_t(j.src.size()), std::move(code)});
        } else {
            PipelineJob j = std::move(pipeline_jobs_.front());
            pipeline_jobs_.pop_front();
            lock.unlock();
            D3D12_INPUT_ELEMENT_DESC el[16];
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
            d.InputLayout = {el, FillInputLayout(j.k, el)};
            FillPipelineDesc(j.k, j.vs.Get(), j.ps.Get(), d);
            if (j.gs) ApplyGeometryShader(d, j.gs.Get());
            ComPtr<ID3D12PipelineState> pso;
            if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
                LOGE("d3d12: pipeline creation failed");
                pso.Reset();
            }
            lock.lock();
            pipelines_done_.push_back({j.k, j.h, std::move(pso)});
        }
        work_finished_ = true;
    }
}

// what the threads finished, into the caches; one the render thread made meanwhile keeps its own
void D3D12Backend::TakeFinished() {
    if (!work_finished_.exchange(false)) return;
    std::vector<ShaderDone> shaders;
    std::vector<PipelineDone> pipelines;
    {
        std::lock_guard lock(work_lock_);
        shaders.swap(shaders_done_);
        pipelines.swap(pipelines_done_);
    }
    for (ShaderDone& s : shaders) {
        shaders_queued_.erase(s.hash);
        if (!blobs_.emplace(s.hash, s.code).second) continue;
        if (s.code) WriteShader(s.hash, s.src_size, s.code.Get());
    }
    for (PipelineDone& p : pipelines) {
        pipelines_queued_.erase(p.h);
        if (!psos_.emplace(p.h, p.pso).second || !p.pso) continue;
        NotePipelineKey(p.h, p.k);
        StorePipeline(p.h, p.pso.Get());
    }
}

constexpr char kPipelineFileMagic[8] = {'X', 'B', 'E', 'P', 'S', 'O', '0', '3'};

std::filesystem::path PipelineFile() { return app::ExeDir() / "pipelines.bin"; }

void D3D12Backend::OpenPipelineLibrary() {
    pso_lib_tried_ = true;
    ComPtr<ID3D12Device1> dev1;
    if (FAILED(dev_.As(&dev1))) return;
    if (std::FILE* f = _wfopen(PipelineFile().c_str(), L"rb")) {
        char magic[8] = {};
        uint64_t root = 0;
        if (std::fread(magic, 1, 8, f) == 8 && std::fread(&root, 8, 1, f) == 1 &&
            std::memcmp(magic, kPipelineFileMagic, 8) == 0 && root == root_hash_) {
            std::fseek(f, 0, SEEK_END);
            long end = std::ftell(f);
            if (end > 16) {
                pso_lib_data_.resize(size_t(end - 16));
                std::fseek(f, 16, SEEK_SET);
                if (std::fread(pso_lib_data_.data(), 1, pso_lib_data_.size(), f) != pso_lib_data_.size())
                    pso_lib_data_.clear();
            }
        }
        std::fclose(f);
    }
    // another driver or GPU refuses the old one: start empty
    if (!pso_lib_data_.empty() &&
        FAILED(dev1->CreatePipelineLibrary(pso_lib_data_.data(), pso_lib_data_.size(), IID_PPV_ARGS(&pso_lib_)))) {
        pso_lib_data_.clear();
        pso_lib_.Reset();
    }
    if (!pso_lib_ && FAILED(dev1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&pso_lib_)))) return;
    LOGR("d3d12: %zu KB of pipelines from pipelines.bin", pso_lib_data_.size() / 1024);
}

void D3D12Backend::CountBuild(PerfClock::time_point start) {
    double ms = PerfMs(start);
    builds_.ms += ms;
    builds_.worst = std::max(builds_.worst, ms);
}

void D3D12Backend::LogBuilds() {
    if (PerfMs(builds_logged_) < 5000) return;
    builds_logged_ = PerfClock::now();
    const BuildCounts& b = builds_;
    if (b.shaders || b.built || b.ms >= 2)
        LOGR("d3d12: %u shaders compiled, %u pipelines built and %u loaded: %.1f ms on the render thread (worst %.1f)",
             b.shaders, b.built, b.loaded, b.ms, b.worst);
    builds_ = {};
    if (later_.shaders || later_.pipelines || later_.draws)
        LOGR("d3d12: %u shaders and %u pipelines handed to the threads, %u draws left out waiting for them",
             later_.shaders, later_.pipelines, later_.draws);
    later_ = {};
}

void D3D12Backend::SavePipelineLibrary() {
    // a second instance only reads it
    if (!pso_lib_ || app::Instance() > 1 || pso_lib_writing_->load()) return;
    pso_lib_dirty_ = false;
    pso_lib_writing_->store(true);
    // the library serialized by the thread too, megabytes the render thread used to stop for; the library syncs
    // itself against the pipelines stored meanwhile, and one stored between the size and the copy fails it: the
    // next save writes it
    std::thread([lib = pso_lib_, root = root_hash_, writing = pso_lib_writing_] {
        std::vector<char> data(16 + lib->GetSerializedSize());
        std::memcpy(data.data(), kPipelineFileMagic, 8);
        std::memcpy(data.data() + 8, &root, 8);
        if (FAILED(lib->Serialize(data.data() + 16, data.size() - 16))) {
            writing->store(false);
            return;
        }
        // beside it first, so a run that stops while writing keeps the old file
        std::filesystem::path path = PipelineFile(), tmp = path;
        tmp += L".tmp";
        if (std::FILE* f = _wfopen(tmp.c_str(), L"wb")) {
            bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
            ok = std::fclose(f) == 0 && ok;
            if (ok) MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        writing->store(false);
    }).detach();
}

constexpr char kPipelineKeyMagic[8] = {'X', 'B', 'E', 'P', 'K', 'E', 'Y', '2'};

void D3D12Backend::NotePipelineKey(uint64_t h, const PsoKey& k) {
    if (!pso_keys_seen_.insert(h).second || !pso_key_file_) return;
    std::fwrite(&k, sizeof(k), 1, pso_key_file_);
    std::fflush(pso_key_file_);
}

void D3D12Backend::StartPrebuild() {
    if (!shader_file_read_) OpenShaderFile();
    if (!pso_lib_tried_) OpenPipelineLibrary();
    std::filesystem::path own = app::ExeDir() / "pipekeys.bin";
    std::vector<PsoKey> keys;
    auto read = [&](const std::filesystem::path& from) {
        std::FILE* f = _wfopen(from.c_str(), L"rb");
        if (!f) return;
        char magic[8] = {};
        uint32_t size = 0;
        // another layout of the key (a changed build) is not read: its pipelines come again as they're met
        if (std::fread(magic, 1, 8, f) == 8 && std::fread(&size, 4, 1, f) == 1 &&
            std::memcmp(magic, kPipelineKeyMagic, 8) == 0 && size == sizeof(PsoKey)) {
            PsoKey k;
            while (std::fread(&k, sizeof(k), 1, f) == 1)
                if (pso_keys_seen_.insert(Hash(&k, sizeof(k))).second) keys.push_back(k);
        }
        std::fclose(f);
    };
    read(own);
    read(app::ExeDir() / "pipekeys_seed.bin");
    // rewritten whole, the shipped ones joined in; a second instance only reads it
    if (app::Instance() <= 1 && (pso_key_file_ = _wfopen(own.c_str(), L"wb"))) {
        uint32_t size = sizeof(PsoKey);
        std::fwrite(kPipelineKeyMagic, 1, 8, pso_key_file_);
        std::fwrite(&size, 4, 1, pso_key_file_);
        if (!keys.empty()) std::fwrite(keys.data(), sizeof(PsoKey), keys.size(), pso_key_file_);
        std::fflush(pso_key_file_);
    }
    // the shaders each key names looked up here, the thread only reading what it's handed
    struct Job {
        PsoKey k;
        uint64_t h;
        ComPtr<ID3DBlob> vs, ps;
    };
    std::vector<Job> jobs;
    size_t missing = 0;
    for (const PsoKey& k : keys) {
        auto vit = disk_shaders_.find(k.vs), pit = disk_shaders_.find(k.ps);
        if (vit == disk_shaders_.end() || pit == disk_shaders_.end()) {
            missing++;
            continue;
        }
        jobs.push_back({k, Hash(&k, sizeof(k)), vit->second.code, pit->second.code});
    }
    if (jobs.empty()) return;
    uint64_t gh;
    ComPtr<ID3DBlob> point_gs = Compile(PointHlsl(), "main", "gs_5_0", gh);
    ComPtr<ID3DBlob> line_gs = Compile(LineHlsl(), "main", "gs_5_0", gh);
    LOGR("d3d12: %zu pipelines of earlier runs being made ahead (%zu without their shaders)", jobs.size(), missing);
    prebuild_ = std::thread([this, jobs = std::move(jobs), point_gs, line_gs, lib = pso_lib_] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        auto start = PerfClock::now();
        uint32_t loaded = 0, built = 0;
        for (const Job& j : jobs) {
            if (prebuild_stop_) break;
            {
                std::lock_guard lock(prebuild_lock_);
                if (prebuild_claimed_.count(j.h)) continue;
                prebuild_busy_ = j.h;
            }
            D3D12_INPUT_ELEMENT_DESC el[16];
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
            d.InputLayout = {el, FillInputLayout(j.k, el)};
            FillPipelineDesc(j.k, j.vs.Get(), j.ps.Get(), d);
            if (j.k.topo == uint32_t(Topology::Points) && !j.k.raw_points && point_gs)
                ApplyGeometryShader(d, point_gs.Get());
            if (j.k.topo == uint32_t(Topology::Lines) && line_gs) ApplyGeometryShader(d, line_gs.Get());
            wchar_t name[17];
            swprintf(name, std::size(name), L"%016llX", (unsigned long long)j.h);
            ComPtr<ID3D12PipelineState> pso;
            if (lib && SUCCEEDED(lib->LoadGraphicsPipeline(name, &d, IID_PPV_ARGS(&pso)))) {
                loaded++;
            } else if (SUCCEEDED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
                built++;
                if (lib && SUCCEEDED(lib->StorePipeline(name, pso.Get()))) prebuild_stored_ = true;
            }
            {
                std::lock_guard lock(prebuild_lock_);
                if (pso) prebuilt_[j.h] = std::move(pso);
                prebuild_busy_ = 0;
            }
            prebuild_cv_.notify_all();
        }
        LOGR("d3d12: %u pipelines loaded and %u built ahead in %.0f ms", loaded, built, PerfMs(start));
    });
}

void D3D12Backend::StopPrebuild() {
    prebuild_stop_ = true;
    if (prebuild_.joinable()) prebuild_.join();
}

// a pipeline the thread made ahead, waiting for it when it's the one being made; one it hasn't reached is the
// render thread's to make, and the thread skips it
ID3D12PipelineState* D3D12Backend::TakePrebuilt(uint64_t h) {
    if (!prebuild_.joinable()) return nullptr;
    std::unique_lock lock(prebuild_lock_);
    prebuild_cv_.wait(lock, [&] { return prebuild_busy_ != h; });
    auto it = prebuilt_.find(h);
    if (it == prebuilt_.end()) {
        prebuild_claimed_.insert(h);
        return nullptr;
    }
    ID3D12PipelineState* pso = (psos_[h] = std::move(it->second)).Get();
    prebuilt_.erase(it);
    return pso;
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled
// Wide taps average a 3 console texel box each (kGlowBoxTexels): at 4K, 6 host texels a console one, 81 taps a stage
// over the whole picture, and Mansion of Madness's glow pass (one draw into 320x240) cost more than the scene. Over
// kSmallWide host texels a console one, the draw reads a copy box-filtered down to kSmallScale instead: 9 taps.
constexpr float kSmallWide = 2.5f;
constexpr uint32_t kSmallScale = 2;

ID3D12Resource* D3D12Backend::SmallCopy(Target& t, ID3D12Resource* res, D3D12_RESOURCE_STATES& st) {
    // the stages of one draw share it; the next draw makes it again, the target may have been drawn to since
    if (t.shrunk && t.shrunk_draw == draw_serial_) return t.shrunk.Get();
    uint32_t w = t.gw * kSmallScale, h = t.gh * kSmallScale;
    if (t.shrunk) {
        D3D12_RESOURCE_DESC d = t.shrunk->GetDesc();
        if (d.Width != w || d.Height != h) {
            CurrentFrame().garbage.push_back(t.shrunk);
            t.shrunk = nullptr;
        }
    }
    uint32_t taps = uint32_t(std::ceil(float(t.w) / float(w) - 1e-3f));
    ComPtr<ID3D12Resource> down = Shrink(res, st, t.fmt, w, h, {.taps = taps, .into = t.shrunk.Get(), .into_state = t.shrunk_state});
    if (!down) return nullptr;
    t.shrunk = down;
    t.shrunk_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    t.shrunk_draw = draw_serial_;
    Transition(t.shrunk.Get(), t.shrunk_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    bound_rt_ = bound_ds_ = nullptr;  // the draw binds its targets again
    return t.shrunk.Get();
}

ID3D12Resource* D3D12Backend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                                DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    info.flags |= kTexTarget;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / (t.gw * stages.k[stage]);
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (ext_) ext_->Sampled(d, &t, 0, info);
    sv.Format = t.depth ? kDepthRead : t.fmt;
    // read as a format without alpha (X8R8G8B8, R5G6B5), a target's pixels have alpha 1 whatever its draws left there:
    // a security camera's or a monitor's picture is drawn opaque, not blended with what is behind it
    if (!t.depth && (d.format == 0x07 || d.format == 0x1E || d.format == 0x05 || d.format == 0x11))
        sv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (feedback) return Feedback(t);
    D3D12_RESOURCE_STATES* st;
    ID3D12Resource* res = Readable(t, st);
    if (!res) return nullptr;
    if (stages.wide[stage] > kSmallWide && t.fmt == kBgra) {
        if (ID3D12Resource* copy = SmallCopy(t, res, *st)) {
            stages.wide[stage] = kSmallScale;
            return copy;
        }
    }
    Transition(res, *st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    info.kind = kTex2D;
    return res;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

// false when the texture could not be made, its entry dropped
bool D3D12Backend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    g_perf.newtex++;
    if (tx.res) CurrentFrame().garbage.push_back(tx.res);
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    bool bc = fi.compressed;
    uint32_t w = d.width;
    uint32_t h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = d.depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = UINT16(d.cube ? 6 : d.depth);
    td.MipLevels = UINT16(tx.levels);
    td.Format = tx.fmt;
    td.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&tx.res)))) {
        textures_.erase(key);
        return false;
    }
    return true;
}

void CopyLevelRows(uint8_t* out, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, UINT rows, UINT64 row_size,
                   const std::vector<uint8_t>& px, uint32_t pitch) {
    uint32_t slices = fp.Footprint.Depth;
    size_t src_rows = pitch ? px.size() / pitch / std::max(slices, 1u) : 0;
    for (uint32_t z = 0; z < slices; z++) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* dst = out + (size_t(z) * rows + y) * fp.Footprint.RowPitch;
            if (y < src_rows) {
                std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min<size_t>(pitch, row_size));
            } else {
                std::memset(dst, 0, size_t(row_size));
            }
        }
    }
}

void D3D12Backend::UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px,
                                      uint32_t pitch) {
    D3D12_RESOURCE_DESC rd = tx.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    uint32_t sub = f * tx.levels + l;
    dev_->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &rows, &row_size, &bytes);
    Alloc a = Upload(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    CopyLevelRows(a.cpu, fp, rows, row_size, px, pitch);
    fp.Offset = a.off;
    D3D12_TEXTURE_COPY_LOCATION dl{tx.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dl.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint = fp;
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

void D3D12Backend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                                 const uint32_t* colours, bool same) {
    D3D12_RESOURCE_STATES st = same ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_COPY_DEST);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            if (ext_) ext_->Decoded(d, fi, f, l, px, pitch, tx.flags);
            UploadTextureLevel(tx, f, l, px, pitch);
        }
    }
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void FillTextureView(const Tex& tx, const TextureDesc& d, D3D12_SHADER_RESOURCE_VIEW_DESC& sv) {
    sv.Format = tx.fmt;
    if (d.cube) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = tx.levels;
    } else if (d.depth > 1) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = tx.levels;
    } else {
        sv.Texture2D.MipLevels = tx.levels;
    }
}

// a P8 texture's palette: its colours, their address (part of the texture's key) and their hash, all 0 without one
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

// a P8 texture is decoded with the stage's palette: 256 colours, fewer by the size code in the top two bits
void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;          // D3DFMT_P8
    constexpr uint32_t kPaletteCommon = 0;     // D3DPalette: Common, its top two bits the size code
    constexpr uint32_t kPaletteData = 4;       // D3DPalette: Data, physical address of the colours
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
ID3D12Resource* D3D12Backend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                               DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& tx = textures_[key];
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return nullptr;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash) {
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        auto perf_t = PerfClock::now();
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data) ^ palette.hash;
        g_perf.sig += PerfMs(perf_t);
        if (sig != tx.sig || !same) {
            PerfScope perf_up{g_perf.texup};
            g_perf.texups++;
            g_perf.texup_bytes += total;
            tx.sig = sig;
            UploadTexture(tx, d, fi, p, palette.va ? palette.colours : nullptr, same);
        }
    }
    FillTextureView(tx, d, stages.sv[stage]);
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    if (ext_) ext_->Sampled(d, nullptr, tx.flags, info);
    return tx.res.Get();
}

ID3D12Resource* D3D12Backend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    info = {};
    sv = {};
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.Texture2D.MipLevels = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;

    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    uint64_t alt = 0;
    bool box = true;
    if (ext_) ext_->Sample(d.data, fi.depth, stage, alt, stages.k[stage], box);
    auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height) | alt);
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        target_used_[d.data] = frame_;
        return BindTargetTexture(s, stage, d, tit->second, box, stages);
    }
    return BindGuestTexture(s, stage, d, fi, stages);
}

// D3DTEXF: 1 point, 2 linear, 3 anisotropic
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
// D3DTADDRESS values the samplers test
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

D3D12_SAMPLER_DESC StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso) {
    D3D12_SAMPLER_DESC sd = {};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    D3D12_FILTER_TYPE fmin = lin(min) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmag = lin(mag) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmip = mip == kTexfLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_FILTER(fmin << 4 | fmag << 2 | fmip);
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    if (aniso && lin(min) && mip && stages.sv[i].Texture2D.MipLevels > 1 && !stages.info[i].shadow) an = aniso;
    if (an > 1) {
        sd.Filter = D3D12_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = std::min<uint32_t>(an, 16);
    }
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp
    sd.AddressU = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
    sd.AddressV = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
    sd.AddressW = Address(t[kTssAddressW]);
    sd.MipLODBias = Bits(t[kTssMipLodBias]);
    sd.MinLOD = float(t[kTssMaxMipLevel]);
    sd.MaxLOD = mip ? D3D12_FLOAT32_MAX : sd.MinLOD;
    Color(t[kTssBorderColor], sd.BorderColor);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, D3D12 tests r OP texel; with both reversed (g_reversed_depth) it is the same OP
        sd.Filter = lin(min) || lin(mag) ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                                         : D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sd.ComparisonFunc = Cmp(s.rs[kRsShadowFunc]);
        sd.MaxAnisotropy = 0;
    }
    return sd;
}

uint32_t D3D12Backend::EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp) {
    D3D12_SAMPLER_DESC sd[4];
    uint32_t aniso = app::Get().low_profile ? std::min(app::Get().aniso, kLowProfileAniso) : app::Get().aniso;
    for (int i = 0; i < 4; i++) sd[i] = StageSampler(s, i, stages, clamp, aniso);
    uint64_t key = Hash(sd, sizeof(sd));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    if (smp_used_ + 4 > kSamplerCount) {
        samplers_.clear();
        smp_used_ = 0;
    }
    uint32_t base = smp_used_;
    smp_used_ += 4;
    for (int i = 0; i < 4; i++) dev_->CreateSampler(&sd[i], Cpu(smp_heap_.Get(), base + i, smp_inc_));
    samplers_[key] = base;
    return base;
}

uint32_t SimpleState(const State& s, uint32_t method) {
    return s.simple[method >> 2];
}

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    // a shader made from a declaration alone (no program) is the fixed function pipeline on those inputs
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    // up to a quad with no depth test: it may be a pass over the whole target, see QuadHlsl; points never are
    bool points = a.topology == Topology::Points;
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable] && !points;
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

// the program's hash is cached in the guest shader object
void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

// vertex shader cache key salts
constexpr uint64_t kDeclarationSalt = 0x10000;    // fixed function seed, above the 16 bit register mask
constexpr uint64_t kPretransformedSalt = 0x5A5A;  // screen space fixed function vertices, apart from untransformed
constexpr uint64_t kQuadSalt = 0xC3C30000;        // the QuadHlsl variant, apart from the plain shader

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgramHlsl(s.vs->program, a.regs, a.decode);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

// sets sh.vh and sh.vs_code, compiling the shader on a cache miss
void D3D12Backend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    if (vit != shader_src_.end()) {
        sh.vs_code = blobs_[vit->second].Get();
        sh.vh = vit->second;
        return;
    }
    if (StillCompiling(sh.vkey, sh.vs_code, sh.vh)) {
        sh.later = true;
        return;
    }
    if (sh.vs_code) return;
    std::string src = ReversedDepthHlsl(VertexHlsl(s, a, sh));
    sh.vs_code = Compile(sh.quad ? QuadHlsl(std::move(src)) : src, "main", "vs_5_0", sh.vh, later_ok_);
    sh.later = Settle(sh.vkey, sh.vs_code, sh.vh);
}

// a shader key the threads were handed: true while its shader is still being compiled and this draw may be left out;
// once compiled the key is settled and code set, and a draw that can't wait leaves code null to compile it itself
bool D3D12Backend::StillCompiling(uint64_t key, ID3DBlob*& code, uint64_t& hash) {
    auto it = shaders_pending_.find(key);
    if (it == shaders_pending_.end()) return false;
    auto bit = blobs_.find(it->second);
    if (bit == blobs_.end()) return later_ok_;
    hash = it->second;
    code = bit->second.Get();
    shader_src_[key] = hash;
    shaders_pending_.erase(it);
    return false;
}

// after Compile: the key settled, or left pending when the threads have it; true when they do
bool D3D12Backend::Settle(uint64_t key, ID3DBlob* code, uint64_t hash) {
    if (!code && !blobs_.count(hash)) {
        shaders_pending_[key] = hash;
        return true;
    }
    shader_src_[key] = hash;
    shaders_pending_.erase(key);
    return false;
}

bool D3D12Backend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    {
        PerfScope perf_bind{g_perf.bind};
        for (int i = 0; i < 4; i++) stages.res[i] = BindTexture(s, i, stages);
    }
    // textures may have recorded copies that unbound the targets
    return BindTargets(s);
}

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

// sets sh.ph and sh.ps_code, compiling the shader on a cache miss
void D3D12Backend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    // pixel shaders share the map with vertex shaders, the salt keeps their keys apart
    auto pit = shader_src_.find(sh.pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) {
        sh.ps_code = blobs_[pit->second].Get();
        sh.ph = pit->second;
        return;
    }
    uint64_t key = sh.pkey ^ kPixelKeySalt;
    if (StillCompiling(key, sh.ps_code, sh.ph)) {
        sh.later = true;
        return;
    }
    if (sh.ps_code) return;
    sh.ps_code = Compile(PixelHlsl(pk), "main", "ps_5_0", sh.ph, later_ok_);
    sh.later |= Settle(key, sh.ps_code, sh.ph);
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to D3D12 RGBA bits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

constexpr uint32_t kZUseW = 2;  // D3DZB_USEW

bool OffsetsPolygons(const State& s, Topology topology) {
    return topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable);
}

// polygon offset slopes are per console pixel, an upscaled target's pixels are smaller; a view widened past the
// title's angle (g_view_widening) makes each pixel's depth step larger by as much, so the slope is taken back by it
float HostSlopeScale(const State& s, const Target* ds) {
    float widening = std::max(g_view_widening.load(std::memory_order_relaxed), 1.0f);
    return Bits(SimpleState(s, kMSlopeScale)) * (ds ? float(ds->w) / ds->gw : 1.0f) / widening;
}

void D3D12Backend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    // polygon offset: the slope here, toward the camera is up on reversed depth; the units are in ReversedDepthHlsl
    if (OffsetsPolygons(s, a.topology)) k.zslope = -HostSlopeScale(s, bound_ds_);
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
}

void D3D12Backend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? bound_rt_->fmt : 0;
    uint32_t samples = Ms(bound_rt_ ? *bound_rt_ : *bound_ds_);
    k.msaa = samples > 1 ? samples : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
    // Seams sealed: pieces of a level whose edges nearly meet leave cracks a fraction of a console pixel wide, which at
    // the console's resolution seldom cover a pixel's centre and at a high one make lines one sees the sky or the room
    // behind through, flickering as the view sways. Drawn conservatively, a triangle covers every pixel it touches,
    // closing them. Only solid triangles that test depth: those writing it without blending, and the passes laid over
    // them with an EQUAL test, which then find the same depth on the same pixels (each of a seam's pixels keeps one
    // piece's depth, so one piece's passes); see-through ones would blend twice where two meet.
    if (app::Get().seal_seams && conservative_ok_ && a.topology == Topology::Triangles && k.fill != kFillWireframe &&
        k.has_ds && k.zen && ((!k.blend && k.zwrite) || Cmp(k.zfunc) == D3D12_COMPARISON_FUNC_EQUAL))
        k.fill |= kFillSealed;
}

// alpha test off: compare always, in Cmp() minus 1 terms like the enabled test
constexpr float kAlphaTestAlways = float(D3D12_COMPARISON_FUNC_ALWAYS - 1);

// D3DRS_FOGTABLEMODE values
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;
constexpr float kFogOff = -1.0f;  // the fog setting off: every fog factor 1, and the final combiners blend no fog in

// table mode none under a vertex program: oFog is the blend factor, passed on as linear fog from 1 to 0. Fog off
// under a vertex program passes oFog on the same way: a title whose combiners read the fog register reads what its
// program wrote (TimeSplitters FP: oFog 0 is no fog, its final combiner blends towards the fog colour by it)
void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (!app::Get().fog) {
        c.fog[3] = kFogOff;
        return;
    }
    if (!s.rs[kRsFogEnable] && !vertex_program) return;
    if (vertex_program && (!s.rs[kRsFogEnable] || s.rs[kRsFogTableMode] == kFogTableNone)) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(Cmp(SimpleState(s, kMAlphaFunc)) - 1) : kAlphaTestAlways;
    c.misc[2] = float(Cmp(s.rs[kRsShadowFunc]) - 1);
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
    std::memcpy(c.attr, s.attr.data(), sizeof(c.attr));
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

constexpr float kGlowBoxTexels = 3.0f;  // console texels across the box each wide tap averages

void D3D12Backend::FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    // a 3 console texel box per tap keeps the console chain's glow width and brightness
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width) * k);
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void D3D12Backend::FillTextureConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages,
                                        DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        if (stages.rt[i]) {
            app::Scale2 shift = RenderTargetShift(s, sh);
            c.rtshift[0] = shift.x;
            c.rtshift[1] = shift.y;
        }
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    // world, view, projection, then the viewport inside the target
    float wv[16];
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightAttenuation1 = 22;
constexpr int kLightAttenuation2 = 23;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    // lights to view space
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

// a w buffer's clip range in the units of a draw's clip w
struct WRange {
    float near_w;
    float far_w;
};

// the projection's near and far clip w as D3DDevice_SetTransform finds them
WRange ProjectionWRange(const float* m) {
    float m33 = m[10];
    float m34 = m[11];
    float m43 = m[14];
    float m44 = m[15];
    if (m33 == m34 || m33 == 0) return {0, 1};
    return {m44 - m43 / m33 * m34, (m44 - m43) / (m33 - m34) * m34 + m44};
}

// NV097_SET_CLIP_MIN and MAX in buffer units, sent by D3DDevice_SetDepthClipPlanes
constexpr uint32_t kMClipMin = 0x394;
constexpr uint32_t kMClipMax = 0x398;
constexpr float kFallbackNear = 1.0f / 65536;  // of the far w, when no range is known

// Fixed function clip w is the host's view depth over the projection's range; a program's is in buffer units up to
// zscale, its near end the clip minimum the title sent
WRange DrawWRange(const State& s, bool program) {
    WRange r{0, s.zscale};
    if (!program) {
        r = ProjectionWRange(s.transforms[kTsProjection].data());
    } else if (SimpleState(s, kMClipMax)) {
        r.near_w = Bits(SimpleState(s, kMClipMin));
    }
    if (r.near_w <= 0 || r.near_w >= r.far_w) r.near_w = r.far_w * kFallbackNear;
    return r;
}

// ReversedDepthHlsl's zparams: x the w buffer's scale (0 for a z buffer), y its far w, z the polygon offset in units of
// the clip w for a w buffer, of depth for a z buffer
void D3D12Backend::FillDepthConstants(const State& s, const DrawShaders& sh, const PsoKey& k, DrawConstants& c) const {
    bool offset = OffsetsPolygons(s, Topology(k.topo));
    float units = offset ? Bits(SimpleState(s, kMDepthOffset)) : 0;
    if (s.rs[kRsZEnable] != kZUseW) {
        c.zparams[2] = units / s.zscale;
        c.zparams[3] = s.zscale;  // PixelKey::depth_ties
        return;
    }
    WRange r = DrawWRange(s, sh.prog);
    c.zparams[0] = r.near_w / (r.far_w - r.near_w);
    c.zparams[1] = r.far_w;
    c.zparams[2] = sh.prog ? units : units * r.far_w / s.zscale;
}

void D3D12Backend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                   GuestExtent size, DrawConstants& c) {
    // an extension's constants change the copy the draw reads: none blended under one, and each draw its own copy.
    // Otherwise the vertex constants and the draw's own are uploaded only when they differ from the last draw's.
    D3D12_GPU_VIRTUAL_ADDRESS vgpu;
    Alloc vc{};
    if (interp_n_ > 1 && !ext_) {
        vgpu = InterpConstants(s);
        last_vsc_gpu_ = 0;
    } else if (!ext_ && last_vsc_gpu_ && !std::memcmp(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc))) {
        vgpu = last_vsc_gpu_;
    } else {
        vc = Upload(sizeof(s.vsc), 256);
        std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
        vgpu = vc.gpu;
        std::memcpy(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
        last_vsc_gpu_ = ext_ ? 0 : vc.gpu;
    }
    FillSurfaceConstants(s, size, c);
    // points: host pixels per console pixel, also keeps them off the far edge pull out of screen passes
    if (k.topo == uint32_t(Topology::Points)) c.surf[3] = TargetScale().y;
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    if (ext_) ext_->Constants(reinterpret_cast<float(*)[4]>(vc.cpu), c.ext, bound_rt_, k.blend ? k.src : 1);
    FillDepthConstants(s, sh, k, c);
    FillTextureConstants(s, sh, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    if (!last_dc_gpu_ || std::memcmp(&last_dc_, &c, sizeof(c))) {
        Alloc dc = Upload(sizeof(DrawConstants), 256);
        std::memcpy(dc.cpu, &c, sizeof(c));
        last_dc_ = c;
        last_dc_gpu_ = dc.gpu;
    }
    if (vgpu != binds_.vsc) cl_->SetGraphicsRootConstantBufferView(0, vgpu);
    if (last_dc_gpu_ != binds_.dc) cl_->SetGraphicsRootConstantBufferView(1, last_dc_gpu_);
    binds_.vsc = vgpu;
    binds_.dc = last_dc_gpu_;
}

void D3D12Backend::CaptureTextures(const State& s, const DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        ID3D12Resource* res = stages.res[i];
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        bool tg = targets_.count(SizedTargetKey(d.data, Format(d.format).depth, d.width, d.height));
        const auto& ts = s.tss[size_t(i)];
        fprintf(cap_, " | t%d %08X fmt %X %ux%u%s%s levels %u mip %u bias %.2f maxmip %u", i, d.data, d.format,
                d.width, d.height, tg ? " target" : "", res ? "" : " unbound", d.levels, ts[kTssMipFilter],
                Bits(ts[kTssMipLodBias]), ts[kTssMaxMipLevel]);
        if (res && !tg && stages.info[i].kind == kTex2D && cap_seen_.insert(d.data).second) {
            cap_tex_.push_back({res, stages.sv[i].Format, d.data, d.width, d.height});
            // the guest's bytes, every level, for looking at the mips the host view does not show
            uint32_t bytes = TotalSize(d);
            if (!d.linear && bytes && bytes <= (16u << 20) && mem::IsCommitted(d.data) &&
                mem::IsCommitted(d.data + bytes - 1)) {
                char name[64];
                snprintf(name, sizeof(name), "tex_%08X_fmt%X_%ux%u_l%u.raw", d.data, d.format, d.width, d.height,
                         d.levels);
                if (FILE* f = _wfopen((cap_dir_ / name).c_str(), L"wb")) {
                    fwrite(kernel::HostPtr(d.data), 1, bytes, f);
                    fclose(f);
                }
            }
        }
    }
}

// s.vsc holds the 192 vertex shader constants in hardware order: D3D register + 96
constexpr int kVsConstants = 192;
constexpr int kVsConstantBias = 96;

void D3D12Backend::CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c) {
    fprintf(cap_, "\n     verts");
    uint32_t n = 0;
    for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
    for (size_t v = 0; v < std::min<size_t>(4, n ? a.vertices.size() / (n * 4) : 0); v++) {
        fprintf(cap_, " [");
        for (uint32_t k = 0; k < n * 4; k++) fprintf(cap_, " %g", a.vertices[v * n * 4 + k]);
        fprintf(cap_, " ]");
    }
    for (int r = 0; r < kVsConstants; r++) {
        const auto& v = s.vsc[r];
        if (v[0] || v[1] || v[2] || v[3]) {
            fprintf(cap_, "\n     c%d %g %g %g %g", r - kVsConstantBias, v[0], v[1], v[2], v[3]);
        }
    }
    fprintf(cap_, "\n     pc");
    for (size_t r = 0; r < std::size(c.pc); r++) {
        fprintf(cap_, " (%.3f %.3f %.3f %.3f)", c.pc[r][0], c.pc[r][1], c.pc[r][2], c.pc[r][3]);
    }
}

// the draw's line: its state, then its textures
void D3D12Backend::CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                               const DrawTextures& stages) {
    fprintf(cap_,
            "%04u draw rt %08X ds %08X prim %u count %u vs %08X ps %08X blend %u %X %X zen %u %X zwrite %u "
            "zbias %d %.2f cull %u cw %X c0 %.3f %.3f %.3f %.3f",
            cap_i_++, s.rt.data, bound_ds_ ? s.zs.data : 0, prim, count, s.vs_handle, s.ps ? s.ps->handle : 0, k.blend,
            k.src, k.dst, k.zen, k.zfunc, k.zwrite, k.zbias, k.zslope, k.cull, k.wmask, c.pc[0][0], c.pc[0][1],
            c.pc[0][2], c.pc[0][3]);
    if (s.scissor_count)
        fprintf(cap_, " scissor %d %d %d %d", int32_t(s.scissors[0]), int32_t(s.scissors[1]), int32_t(s.scissors[2]),
                int32_t(s.scissors[3]));
    // the declaration: register, stream, offset and type of each input
    if (s.vs)
        for (int r = 0; r < 16; r++) {
            const auto& in = s.vs->inputs[size_t(r)];
            if (in.stream >= 0 && in.type) fprintf(cap_, " v%d:%d+%u/%02X", r, in.stream, in.offset, in.type);
        }
    CaptureTextures(s, stages);
}

// small draws and each vertex shader's first: their inputs and constants; then ends the draw's line
void D3D12Backend::EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c) {
    if (count <= kQuadVertices || cap_vs_.insert(s.vs_handle).second) CaptureInputs(s, a, c);
    fputc('\n', cap_);
    CaptureGeometry(s, a, c);
}

// every draw's geometry in geom.txt, to find where pieces that should meet leave a gap: the constants that changed
// since the draw before, the float inputs as stored (or as converted), the indices
void D3D12Backend::CaptureGeometry(const State& s, const Assembled& a, const DrawConstants& c) {
    static std::array<std::array<float, 4>, kVsConstants> before;
    static std::filesystem::path dir;  // the capture written to: a new one starts the file and the constants over
    uint32_t i = cap_i_ - 1;
    bool first = dir != cap_dir_;
    FILE* f = _wfopen((cap_dir_ / "geom.txt").c_str(), first ? L"w" : L"a");
    if (!f) return;
    if (first) {
        before = {};
        dir = cap_dir_;
    }
    fprintf(f, "draw %u topology %u strip %d pretransformed %d draw_count %u index_count %u base_vertex %d "
               "vertex_count %u surf %g %g\n",
            i, uint32_t(a.topology), a.strip, a.pretransformed, a.draw_count, a.index_count, a.base_vertex,
            a.vertex_count, c.surf[0], c.surf[1]);
    for (int r = 0; r < kVsConstants; r++) {
        const auto& v = s.vsc[r];
        if (std::memcmp(&v[0], &before[size_t(r)][0], sizeof(float) * 4)) {
            fprintf(f, "c%d %.9g %.9g %.9g %.9g\n", r, v[0], v[1], v[2], v[3]);
            std::memcpy(&before[size_t(r)][0], &v[0], sizeof(float) * 4);
        }
    }
    if (!a.vertices.empty()) {
        uint32_t n = 0;
        for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
        fprintf(f, "converted regs %X\n", a.regs);
        for (size_t v = 0; n && (v + 1) * n * 4 <= a.vertices.size(); v++) {
            for (uint32_t k = 0; k < n * 4; k++) fprintf(f, "%.9g ", a.vertices[v * n * 4 + k]);
            fputc('\n', f);
        }
    }
    for (int r = 0; r < 16 && a.vertices.empty(); r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!((a.regs >> r) & 1)) continue;
        uint32_t n = e.format == DXGI_FORMAT_R32G32B32_FLOAT      ? 3
                     : e.format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 4
                     : e.format == DXGI_FORMAT_R32G32_FLOAT       ? 2
                                                                  : 0;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!n || !vb.src || !vb.stride) continue;
        fprintf(f, "v%d %u\n", r, n);
        for (uint32_t v = 0; uint64_t(v) * vb.stride + e.offset + n * 4 <= vb.bytes; v++) {
            float x[4];
            std::memcpy(x, vb.src + size_t(v) * vb.stride + e.offset, n * 4);
            for (uint32_t k = 0; k < n; k++) fprintf(f, "%.9g ", x[k]);
            fputc('\n', f);
        }
    }
    if (!a.indices.empty()) {
        fprintf(f, "indices %zu\n", a.indices.size());
        for (uint32_t ix : a.indices) fprintf(f, "%u\n", ix);
    } else if (a.index_count && a.index_src) {
        fprintf(f, "indices %u\n", a.index_count);
        for (uint32_t k = 0; k < a.index_count; k++)
            fprintf(f, "%u\n", a.index16 ? static_cast<const uint16_t*>(a.index_src)[k]
                                         : static_cast<const uint32_t*>(a.index_src)[k]);
    }
    fclose(f);
}

// each shader once, as translated
void D3D12Backend::CaptureShader(const char* kind, uint64_t key, const std::string& src) {
    char name[48];
    snprintf(name, sizeof(name), "%s_%016llX.hlsl", kind, (unsigned long long)key);
    if (!cap_sh_.insert(key).second) {
        fprintf(cap_, "     %s\n", name);
        return;
    }
    if (FILE* f = _wfopen((cap_dir_ / name).c_str(), L"w")) {
        fwrite(src.data(), 1, src.size(), f);
        fclose(f);
    }
    fprintf(cap_, "     %s\n", name);
}

void D3D12Backend::CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk) {
    std::string src = ReversedDepthHlsl(VertexHlsl(s, a, sh));
    CaptureShader("vs", sh.vkey, sh.quad ? QuadHlsl(std::move(src)) : src);
    CaptureShader("ps", sh.pkey, PixelHlsl(pk));
}

D3D12_SRV_DIMENSION NullViewDimension(uint8_t kind) {
    if (kind == kTexCube) return D3D12_SRV_DIMENSION_TEXTURECUBE;
    if (kind == kTex3D) return D3D12_SRV_DIMENSION_TEXTURE3D;
    return D3D12_SRV_DIMENSION_TEXTURE2D;
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text, full screen passes. The console's
// pixel centres sample such a quad from its first texel to half a texel short of its far edge; upscaled, the far
// edge's last half texel is drawn too, and wrapped it blends in the opposite edge (thin dark lines on HUD sprites).
// Clamped, it takes the edge texel instead, which a wrapped texture used once over 0..1 has there anyway.
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;  // most vertices a small draw has
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

// a stage without a texture gets a null view of its kind; the samplers read the views after
void D3D12Backend::UpdateTargetUsed(uint32_t data) {
    for (const UsedTarget& u : recent_used_)
        if (u.data == data && u.frame == frame_) return;
    target_used_[data] = frame_;
    recent_used_[recent_used_next_++ % recent_used_.size()] = {data, frame_};
}

// the first slot of the stages' four views, made only when this frame has not made the same set yet
uint32_t D3D12Backend::EnsureViewSet(const DrawTextures& stages) {
    if (view_sets_frame_ != frame_) {
        view_sets_.clear();
        view_sets_frame_ = frame_;
    }
    uint64_t key = Hash(stages.sv, sizeof(stages.sv), Hash(stages.res, sizeof(stages.res)));
    auto it = view_sets_.find(key);
    if (it != view_sets_.end()) return it->second;
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++)
        dev_->CreateShaderResourceView(stages.res[i], &stages.sv[i], Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    view_sets_[key] = slot;
    return slot;
}

// UnitTexcoords only changes stages that wrap (after TGP482/XboxXBE-Recomp 16bf1cd)
bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss)
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    return false;
}

// a stage without a texture gets a null view of its kind; the samplers read the views after
void D3D12Backend::SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        if (stages.res[i]) continue;
        stages.sv[i].ViewDimension = NullViewDimension(stages.info[i].kind);
        stages.sv[i].Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    uint32_t views = EnsureViewSet(stages);
    if (views != binds_.views) cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), views, srv_inc_));
    binds_.views = views;
    uint32_t samplers = EnsureStageSamplers(s, stages, AnyStageWraps(s) && UnitTexcoords(a));
    if (samplers != binds_.samplers) cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), samplers, smp_inc_));
    binds_.samplers = samplers;
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void D3D12Backend::SetViewportScissor(const State& s, uint32_t gw, uint32_t gh) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    D3D12_VIEWPORT vp{0, 0, W, H, 0, 1};
    float sx = W / gw;
    float sy = H / gh;
    LONG x0 = LONG(s.viewport.x * sx);
    LONG y0 = LONG(s.viewport.y * sy);
    LONG x1 = LONG((s.viewport.x + s.viewport.w) * sx);
    LONG y1 = LONG((s.viewport.y + s.viewport.h) * sy);
    // The title's scissors (a room clipped to the doorway it is seen through) are whole console pixels, which decide
    // by their pixel centres: geometry reaching up to half a pixel past one is cut there, and the piece beside it,
    // drawn from its own edge, covers the console's next centre. Upscaled, host pixels fall between the cut and that
    // edge and neither paints them: see-through lines along doorways that come and go with the camera. Each edge moves
    // out half a console pixel, rounded inwards to host pixels, so at the console's size it is where it was.
    if (s.scissor_count) {
        x0 = std::max(x0, LONG(std::ceil((float(int32_t(s.scissors[0])) - 0.5f) * sx)));
        y0 = std::max(y0, LONG(std::ceil((float(int32_t(s.scissors[1])) - 0.5f) * sy)));
        x1 = std::min(x1, LONG(std::floor((float(int32_t(s.scissors[2])) + 0.5f) * sx)));
        y1 = std::min(y1, LONG(std::floor((float(int32_t(s.scissors[3])) + 0.5f) * sy)));
    }
    D3D12_RECT rect{std::clamp<LONG>(x0, 0, LONG(W)), std::clamp<LONG>(y0, 0, LONG(H)),
                    std::clamp<LONG>(x1, 0, LONG(W)), std::clamp<LONG>(y1, 0, LONG(H))};
    if (binds_.rasterizer && !std::memcmp(&vp, &binds_.viewport, sizeof(vp)) &&
        !std::memcmp(&rect, &binds_.scissor, sizeof(rect)))
        return;
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &rect);
    binds_.rasterizer = true;
    binds_.viewport = vp;
    binds_.scissor = rect;
}

void D3D12Backend::SetBlendFactorStencilRef(const State& s) {
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    uint32_t ref = SimpleState(s, kMStencilRef) & 0xFF;
    if (binds_.output && !std::memcmp(bf, binds_.blend, sizeof(bf)) && ref == binds_.stencil) return;
    cl_->OMSetBlendFactor(bf);
    cl_->OMSetStencilRef(ref);
    binds_.output = true;
    std::memcpy(binds_.blend, bf, sizeof(bf));
    binds_.stencil = ref;
}

D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    if (a.topology == Topology::Lines) {
        return a.strip ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    }
    return a.strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

void D3D12Backend::UploadVertexBuffers(const Assembled& a) {
    D3D12_VERTEX_BUFFER_VIEW vbv[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(b.bytes, 16);
        std::memcpy(vb.cpu, b.src, b.bytes);
        vbv[i] = {vb.gpu, b.bytes, b.stride};
    }
    cl_->IASetVertexBuffers(0, a.buffer_count, vbv);
}

void D3D12Backend::DrawGeometry(const Assembled& a) {
    UploadVertexBuffers(a);
    D3D_PRIMITIVE_TOPOLOGY topology = PrimitiveTopology(a);
    if (topology != binds_.topology) cl_->IASetPrimitiveTopology(topology);
    binds_.topology = topology;
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        D3D12_INDEX_BUFFER_VIEW ibv{ib.gpu, ibytes, a.index16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        cl_->IASetIndexBuffer(&ibv);
        cl_->DrawIndexedInstanced(a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        cl_->DrawInstanced(a.draw_count, 1, 0, 0);
    }
}

// calls the extension's EndDraw on every return from Draw once made
struct DrawEnd {
    Extension* e;
    ~DrawEnd() {
        if (e) e->EndDraw();
    }
};

// Assemble, timed and counted
bool D3D12Backend::AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count,
                                const uint8_t* indices, Assembled& a) {
    bool assembled;
    {
        PerfScope perf_assemble{g_perf.assemble};
        assembled = Assemble(s, prim, start, count, indices, a);
    }
    g_perf.verts += a.vertex_count;
    g_perf.raw += a.vertices.empty();
    return assembled;
}

bool D3D12Backend::EnsureQueries() {
    if (query_counts_) return true;
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_OCCLUSION, kQueries, 0};
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(uint64_t(kQueries) * sizeof(uint64_t));
    void* p = nullptr;
    bool made = SUCCEEDED(dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&query_heap_))) &&
                SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr, IID_PPV_ARGS(&query_rb_))) &&
                SUCCEEDED(query_rb_->Map(0, nullptr, &p));
    if (!made) {
        LOG_FIRST(1, "d3d12: no occlusion queries, visibility tests stay pending");
        return false;
    }
    query_counts_ = static_cast<const uint64_t*>(p);
    return true;
}

// a draw while the title counts visible pixels gets an occlusion query of its own, kNoQuery otherwise
uint32_t D3D12Backend::BeginCountedDraw(const State& s) {
    if (!s.simple[kMCountEnable >> 2] || !EnsureQueries()) return kNoQuery;
    uint32_t query = query_next_++ % kQueries;
    cl_->BeginQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
    return query;
}

void D3D12Backend::EndCountedDraw(uint32_t query, bool points) {
    if (query == kNoQuery) return;
    cl_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
    uint64_t at = uint64_t(query) * sizeof(uint64_t);
    cl_->ResolveQueryData(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query, 1, query_rb_.Get(), at);
    counted_.push_back({query, points ? PointSamples() : SamplesPerPixel()});
}

// the console counts its own pixels: the bound target's scale and multisampling come out
float D3D12Backend::SamplesPerPixel() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw || !t->gh) return 1;
    float area = float(t->w) * float(t->h) / (float(t->gw) * float(t->gh));
    return area * float(Ms(*t));
}


// a visibility test's point, drawn as a single host pixel, is one console pixel: only multisampling comes out
float D3D12Backend::PointSamples() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    return t ? float(Ms(*t)) : 1.0f;
}

// reports whose draws the GPU has finished, written as the console's GPU writes them: time, count, then 0 for done
void D3D12Backend::DeliverReports() {
    constexpr uint32_t kTime = 0;
    constexpr uint32_t kTimeHigh = 4;
    constexpr uint32_t kCount = 8;
    constexpr uint32_t kStatus = 12;
    uint64_t done = fence_->GetCompletedValue();
    while (!reports_.empty() && reports_.front().fence <= done) {
        const Report& r = reports_.front();
        double pixels = 0;
        for (const Query& q : r.queries) pixels += double(query_counts_[q.index]) / q.samples_per_pixel;
        kernel::W32(r.va + kTime, uint32_t(presents_));
        kernel::W32(r.va + kTimeHigh, 0);
        // A test sprite of a fractional size covers more or fewer of the host's pixels with where it falls between
        // them (TimeSplitters FP's coronas: 320 to 440 from frame to frame), and the title draws by the count: each
        // count is held at its highest over the last g_report_hold frames. Reports are a ring the title walks, a slot
        // per test, so a light is told by its place among the frame's tests, which the title makes in the same order.
        uint32_t count = uint32_t(std::lround(pixels));
        {
            static uint64_t frame = ~0ull;
            static uint32_t place = 0;
            static std::unordered_map<uint32_t, std::deque<uint32_t>> held;
            if (r.frame != frame) {
                frame = r.frame;
                place = 0;
            }
            std::deque<uint32_t>& recent = held[place++];
            recent.push_back(count);
            while (recent.size() > std::max<uint32_t>(g_report_hold, 1)) recent.pop_front();
            count = *std::max_element(recent.begin(), recent.end());
        }
        kernel::W32(r.va + kCount, count);
        // XBE_TRACE_REPORTS=<skip>: 2000 reports to the log after the first <skip>
        static const char* trace_env = std::getenv("XBE_TRACE_REPORTS");
        static long traced = trace_env ? -std::atol(trace_env) : 0;
        if (trace_env && traced < 2000 && traced++ >= 0)
            LOGR("report: %08X %u pixels (%zu draws, %llu raw) frame %llu", r.va, count, r.queries.size(),
                 r.queries.empty() ? 0ull : (unsigned long long)query_counts_[r.queries[0].index],
                 (unsigned long long)presents_);
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(r.va + kStatus, 0);
        reports_.pop_front();
    }
}

// a drop logged once per reason and vertex shader handle
#define LOG_DROP(fmt, ...)                                                                  \
    do {                                                                                    \
        static std::set<uint32_t> seen_;                                                    \
        if (seen_.insert(s.vs_handle).second) LOGI(fmt " (vs %08X)", __VA_ARGS__, s.vs_handle); \
    } while (0)

void D3D12Backend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    PerfScope perf_draw{g_perf.draw};
    g_perf.draws++;
    draw_serial_++;
    if (cap_) CapStep(s.rt);
    if (DepthCopy(s)) return;
    DrawEnd draw_end{ext_};
    if (ext_) ext_->BeginDraw(s);
    Assembled& a = asm_;
    if (!AssembleDraw(s, prim, start, count, indices, a)) { LOG_DROP("draw dropped: assemble, vs %08X prim %u count %u", s.vs_handle, prim, count); return; }
    // past the ring every upload is a buffer of its own: seconds lost on a frame that draws a whole level once
    if (CurrentFrame().used > kUploadSize - kUploadReserve) Submit(true);
    if (!BindTargets(s)) { LOG_DROP("draw dropped: targets rt %08X zs %08X", s.rt.data, s.zs.data); return; }
    if (ext_ && ext_->Bound(*this, s, bound_rt_, bound_ds_, a.pretransformed) && !BindTargets(s)) return;
    g_perf.pass_draws[size_t(pass_)]++;
    if (diag::Perf()) g_perf.by_target[pass_tag_].second++;

    // a draw whose pixels are counted, or captured, waits for its shaders; the others may be left out meanwhile
    TakeFinished();
    later_ok_ = !workers_.empty() && !cap_ && !s.simple[kMCountEnable >> 2];

    // vertex shader
    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    // stage textures, then the pixel shader made for them
    DrawTextures stages{};
    if (!BindStageTexturesAndTargets(s, stages)) { LOG_DROP("draw dropped: textures %08X %08X", s.textures[0], s.textures[1]); return; }
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    // depth ties as the console's (PixelHlsl): a z buffer tested, one sample a pixel (an exported depth is the
    // whole pixel's, which MSAA's edges would lose)
    pk.depth_ties = app::Get().depth_ties && bound_ds_ && Ms(*bound_ds_) <= 1 && s.rs[kRsZEnable] == 1;
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (sh.later) {
        later_.draws++;
        return;
    }
    if (!sh.vs_code || !sh.ps_code) { LOG_DROP("draw dropped: shaders vs %p ps %p handle %08X", (void*)sh.vs_code, (void*)sh.ps_code, s.vs_handle); return; }

    // pipeline
    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    // A visibility test's points are counted as the console counts them, one pixel each: expanded to a whole console
    // pixel at the host's scale a point covers 4 to 9 host pixels with where it falls, and TimeSplitters FP's light
    // coronas, tested with a few points each, blinked with the count.
    bool raw_points = a.topology == Topology::Points && s.simple[kMCountEnable >> 2];
    k.raw_points = raw_points;
    bool pso_later = false;
    ID3D12PipelineState* pso = EnsurePipeline(k, sh.vs_code, sh.ps_code, later_ok_ ? &pso_later : nullptr);
    if (pso_later) {
        later_.draws++;
        return;
    }
    if (!pso) { LOG_DROP("draw dropped: pipeline%s", ""); return; }
    if (app::Get().ao) NoteAmbientOcclusion(s, k);
    if (ext_) binds_ = {};  // the extension may set anything
    if (pso != binds_.pso) cl_->SetPipelineState(pso);
    binds_.pso = pso;

    // constants
    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    if (interp_n_ > 1) interp_key_ = InterpKey(s, prim, start, count, indices);
    DrawConstants c{};
    UploadConstants(s, sh, k, stages, size, c);
    if (ext_) binds_ = {};

    if (cap_) {
        CaptureDraw(s, prim, count, k, c, stages);
        EndDrawCapture(s, a, count, c);
        CaptureShaders(s, a, sh, pk);
    }

    // texture views and samplers, viewport and scissor, blend factor and stencil reference
    SetTextureViewsAndSamplers(s, a, stages);
    SetViewportScissor(s, size.w, size.h);
    SetBlendFactorStencilRef(s);

    // geometry, counted while the title runs a visibility test
    uint32_t query = BeginCountedDraw(s);
    DrawGeometry(a);
    EndCountedDraw(query, raw_points);
    // XBE_CAPTURE_DRAWS=<first>-<last>: a capture also saves the target after each of those draws
    if (cap_ && bound_rt_) {
        static const std::pair<long, long> each = [] {
            const char* e = std::getenv("XBE_CAPTURE_DRAWS");
            long lo = -1, hi = -1;
            if (e) std::sscanf(e, "%ld-%ld", &lo, &hi);
            return std::pair<long, long>{lo, hi};
        }();
        long i = long(cap_i_) - 1;
        if (i >= each.first && i <= each.second) {
            char name[32];
            snprintf(name, sizeof(name), "after_%04ld", i);
            // at the host's resolution, which shows what only shows upscaled; the draw binds its targets again
            Target& t = *bound_rt_;
            D3D12_RESOURCE_STATES* st;
            if (ID3D12Resource* res = Readable(t, st))
                CapSave({.res = res, .state = st, .fmt = t.fmt, .w = t.w, .h = t.h, .mul = t.range}, name);
            bound_rt_ = bound_ds_ = nullptr;
        }
    }
}

// a guest rect scaled to the target and clamped to it
D3D12_RECT ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    float sx = float(t.w) / t.gw;
    float sy = float(t.h) / t.gh;
    return {std::clamp<LONG>(LONG(x0 * sx), 0, t.w), std::clamp<LONG>(LONG(y0 * sy), 0, t.h),
            std::clamp<LONG>(LONG(x1 * sx), 0, t.w), std::clamp<LONG>(LONG(y1 * sy), 0, t.h)};
}

// a clear of t's memory, which t has taken; a target of another size takes it when next bound
void D3D12Backend::NoteDepthClear(Target& t, uint32_t flags, float z, uint32_t stencil) {
    DepthClear& c = depth_clears_[t.data];
    if (flags & kClearZ) {
        c.z_gen = t.took_z = ++clear_serial_;
        c.z = z;
        c.z_gw = t.gw;
        c.z_gh = t.gh;
    }
    if (flags & kClearStencil) {
        c.s_gen = t.took_s = ++clear_serial_;
        c.stencil = stencil;
        c.s_gw = t.gw;
        c.s_gh = t.gh;
    }
}

// the clears of t's memory made through a target of another size: one that covered t's corner clears all of t, a
// smaller one cleared memory t only partly holds and is passed over
void D3D12Backend::TakeDepthClear(Target& t) {
    auto it = depth_clears_.find(t.data);
    if (it == depth_clears_.end()) return;
    const DepthClear& c = it->second;
    D3D12_CLEAR_FLAGS cf{};
    if (t.took_z != c.z_gen) {
        if (t.gw <= c.z_gw && t.gh <= c.z_gh) cf |= D3D12_CLEAR_FLAG_DEPTH;
        t.took_z = c.z_gen;
    }
    if (t.took_s != c.s_gen) {
        if (t.gw <= c.s_gw && t.gh <= c.s_gh) cf |= D3D12_CLEAR_FLAG_STENCIL;
        t.took_s = c.s_gen;
    }
    if (!cf) return;
    if (cap_) fprintf(cap_, "%04u depth clear taken %08X %ux%u flags %X\n", cap_i_++, t.data, t.gw, t.gh, unsigned(cf));
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_), cf, c.z, UINT8(c.stencil), 0, nullptr);
}

// rs: the rects buffer, reused across targets
void D3D12Backend::ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs) {
    Mark(PassOf(t), TargetTag(t));
    rs.clear();
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            rs.push_back(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        rs.push_back(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    if (t.depth) {
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_CLEAR_FLAGS cf = D3D12_CLEAR_FLAGS(((c.flags & kClearZ) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                 ((c.flags & kClearStencil) ? D3D12_CLEAR_FLAG_STENCIL : 0));
        TakeDepthClear(t);
        float reversed = 1 - std::clamp(c.z, 0.0f, 1.0f);
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_), cf, reversed, UINT8(c.stencil),
                                   UINT(rs.size()), rs.data());
        NoteDepthClear(t, c.flags, reversed, c.stencil);  // as stored: reversed
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float col[4];
        Color(c.color, col);
        for (int i = 0; t.range != 1 && i < 3; i++) col[i] /= t.range;
        if ((c.flags & kClearTarget) == kClearTarget) {
            cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), col, UINT(rs.size()), rs.data());
        } else if (ID3D12Resource* copy = (draw_serial_++, t.fb_draw = 0, Feedback(t))) {
            // some channels only: the rest is kept, over the whole target
            float p[8] = {col[0], col[1], col[2], col[3]};
            for (int i = 0; i < 4; i++) p[4 + i] = (c.flags >> (4 + i)) & 1 ? 1.0f : 0.0f;
            Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            ID3D12Resource* const src[3] = {copy, nullptr, nullptr};
            const DXGI_FORMAT fmt[3] = {t.fmt, kBgra, kBgra};
            Pass(kMaskedClearHlsl, "psmain", t.fmt, Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h, src, fmt, p, -1,
                 Ms(t));
            t.fb_draw = 0;
        }
    }
    t.drawn = true;
    t.fb_draw = 0;  // a draw after it copies the target again
}

void D3D12Backend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                         uint32_t stencil) {
    PerfScope perf_clear{g_perf.clear};
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u clear rt %08X ds %08X flags %X color %08X z %.3f\n", cap_i_++, s.rt.data, s.zs.data, flags,
                color, z);
    }
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureDepth(s, rt ? rt : FindTarget(s.rt, false)) : nullptr;
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(rt ? s.rt : s.zs, gw, gh);
    std::vector<D3D12_RECT> rs;
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil};
    for (Target* t : {rt, ds}) {
        if (t) ClearTarget(*t, s, c, rs);
    }
}

// CopyRects' guest lists: src_rects RECTs, dst_points POINTs, all int32
constexpr uint32_t kGuestRectBytes = 16;
constexpr uint32_t kRectLeft = 0;
constexpr uint32_t kRectTop = 4;
constexpr uint32_t kRectRight = 8;
constexpr uint32_t kRectBottom = 12;
constexpr uint32_t kGuestPointBytes = 8;
constexpr uint32_t kPointX = 0;
constexpr uint32_t kPointY = 4;

struct GuestRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct GuestPoint {
    int32_t x;
    int32_t y;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * kGuestRectBytes;
    GuestRect g;
    g.left = int32_t(kernel::R32(r + kRectLeft));
    g.top = int32_t(kernel::R32(r + kRectTop));
    g.right = int32_t(kernel::R32(r + kRectRight));
    g.bottom = int32_t(kernel::R32(r + kRectBottom));
    return g;
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * kGuestPointBytes;
    GuestPoint g;
    g.x = int32_t(kernel::R32(p + kPointX));
    g.y = int32_t(kernel::R32(p + kPointY));
    return g;
}

// a drawn copy's rect in guest pixels: the source corners and where it lands
struct DrawnRect {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    float dx = 0;
    float dy = 0;
};

// without dst_points the rect lands where it is
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    GuestRect g = ReadGuestRect(src_rects, i);
    DrawnRect r;
    r.x0 = float(g.left);
    r.y0 = float(g.top);
    r.x1 = float(g.right);
    r.y1 = float(g.bottom);
    r.dx = r.x0;
    r.dy = r.y0;
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

// a guest coordinate read as unsigned, times the host scale
UINT ScaleUnsigned(int32_t v, float sc) { return UINT(uint32_t(v) * sc); }

// a drawn copy's taps round down a ratio a hair over a whole number
constexpr float kTapsSlack = 1e-3f;

// one Blit per rect: for formats that differ
void D3D12Backend::CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                              Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), dst_t.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    float src_scale = float(src_t.w) / src_t.gw;
    float dst_scale = float(dst_t.w) / dst_t.gw;
    float dst_scale_y = float(dst_t.h) / dst_t.gh;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = float(std::min(src_t.gw, dst_t.gw));
        float whole_h = float(std::min(src_t.gh, dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        D3D12_VIEWPORT vp{r.dx * dst_scale, r.dy * dst_scale_y, (r.x1 - r.x0) * dst_scale,
                          (r.y1 - r.y0) * dst_scale_y, 0, 1};
        LONG right = std::min(LONG(vp.TopLeftX + vp.Width), LONG(dst_t.w));
        LONG bottom = std::min(LONG(vp.TopLeftY + vp.Height), LONG(dst_t.h));
        D3D12_RECT scissor{LONG(vp.TopLeftX), LONG(vp.TopLeftY), right, bottom};
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
        cl_->RSSetViewports(1, &vp);
        cl_->RSSetScissorRects(1, &scissor);
        binds_ = {};
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        float mul = 1 / dst_t.range;
        BlitOptions o{.alpha = true, .taps = taps, .mul = mul, .rtv = dst_t.fmt, .samples = Ms(dst_t)};
        Blit(src_res, src_t.fmt, uv, o);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// one CopyTextureRegion per rect, clamped to both targets
void D3D12Backend::CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                                Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{dst_t.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{src_res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    float dst_scale = float(dst_t.w) / dst_t.gw;
    float dst_scale_y = float(dst_t.h) / dst_t.gh;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        D3D12_BOX b{0, 0, 0, std::min(src_t.w, dst_t.w), std::min(src_t.h, dst_t.h), 1};
        uint32_t dx = 0;
        uint32_t dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            b.left = ScaleUnsigned(r.left, dst_scale);
            b.top = ScaleUnsigned(r.top, dst_scale_y);
            b.right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            b.bottom = std::min(ScaleUnsigned(r.bottom, dst_scale_y), src_t.h);
            dx = b.left;
            dy = b.top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale_y);
            }
        }
        if (b.right <= b.left || b.bottom <= b.top || dx >= dst_t.w || dy >= dst_t.h) continue;
        b.right = std::min<UINT>(b.right, b.left + dst_t.w - dx);
        b.bottom = std::min<UINT>(b.bottom, b.top + dst_t.h - dy);
        cl_->CopyTextureRegion(&dl, dx, dy, 0, &sl, &b);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D12Backend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                        uint32_t dst_points) {
    PerfScope perf_copy{g_perf.copy};
    PassScope pass{*this, GpuPass::Copy};
    Target* src_t = FindTarget(src, false);
    if (cap_) {
        const char* from = "memory";
        if (src_t) from = src_t->drawn ? "target" : "idle target";
        fprintf(cap_, "%04u copy %08X %s to %08X\n", cap_i_++, src.data, from, dst.data);
    }
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "d3d12: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Resolve(*src_t);
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points};
    dst_t->fb_draw = 0;
    // a region copy takes the destination's scale for both sides: targets kept at different scales (the screen at the
    // output's resolution, a picture-in-picture or monitor texture at its own) are drawn across, each at its own
    bool same_scale = uint64_t(src_t->w) * dst_t->gw == uint64_t(dst_t->w) * src_t->gw &&
                      uint64_t(src_t->h) * dst_t->gh == uint64_t(dst_t->h) * src_t->gh;
    D3D12_RESOURCE_STATES* src_state;
    ID3D12Resource* src_res = Readable(*src_t, src_state);
    if (!src_res) return;
    // a multisampled side takes no region copy
    bool ms = Ms(*src_t) > 1 || Ms(*dst_t) > 1;
    if (src_t->fmt != dst_t->fmt || ms || (!same_scale && !src_t->depth && !dst_t->depth)) {
        // formats, samples or scales differ: drawn across
        CopyByDraw(*src_t, src_res, *src_state, *dst_t, l);
        return;
    }
    CopyByRegion(*src_t, src_res, *src_state, *dst_t, l);
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_copy_%08X", cap_i_, dst_t->data);
        CapSave(*dst_t, name);
    }
}

void D3D12Backend::ResizeSwap() {
    HWND hwnd = static_cast<HWND>(app::Window());
    RECT rc;
    GetClientRect(hwnd, &rc);
    uint32_t w = std::max<LONG>(rc.right - rc.left, 1);
    uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
    if (w == bb_w_ && h == bb_h_) return;
    DrainPresenter();
    std::lock_guard lock(swap_lock_);
    Flush();
    for (auto& f : frames_) {
        if (f.fence) WaitFence(f.fence);
    }
    refresh_ = RefreshRate();
    for (auto& b : bb_) b.Reset();
    swap_->ResizeBuffers(kFrames, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, SwapFlags());
    CreateBackBufferViews();
    bb_w_ = w;
    bb_h_ = h;
}

void D3D12Backend::FinishCapture(const Surface& front) {
    CapStep(Surface{});
    if (Target* f = FindTarget(front, false)) {
        CapSave(*f, "final");
        // as drawn, at the host's resolution: what only shows upscaled (hairline cracks, edges)
        D3D12_RESOURCE_STATES* st;
        if (ID3D12Resource* res = Readable(*f, st))
            CapSave({.res = res, .state = st, .fmt = f->fmt, .w = f->w, .h = f->h, .mul = f->range}, "final_host");
    }
    for (auto& t : cap_tex_) {
        char name[32];
        snprintf(name, sizeof(name), "tex_%08X", t.data);
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        CapSave({.res = t.res.Get(), .state = &st, .fmt = t.fmt, .w = t.w, .h = t.h}, name);
    }
    fclose(cap_);
    cap_ = nullptr;
    cap_tex_.clear();
    LOGI("d3d12: frame captured to %s", cap_dir_.string().c_str());
}

void D3D12Backend::StartCapture() {
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    char stamp[32];
    static uint32_t captures = 0;
    snprintf(stamp, sizeof(stamp), "%02u%02u%02u_%03u", tm.wHour, tm.wMinute, tm.wSecond, captures++);
    std::error_code ec;
    cap_dir_ = std::filesystem::weakly_canonical(app::ExeDir() / diag::CaptureDir() / stamp, ec);
    std::filesystem::create_directories(cap_dir_, ec);
    cap_ = _wfopen((cap_dir_ / "frame.txt").c_str(), L"w");
    cap_i_ = cap_rt_ = 0;
    cap_seen_.clear();
    cap_sh_.clear();
    cap_vs_.clear();
}

// ramp_ holds the red, green and blue ramps, 256 entries each
constexpr int kRampRed = 0;
constexpr int kRampGreen = 256;
constexpr int kRampBlue = 512;

void D3D12Backend::UploadGammaRamp() {
    constexpr uint32_t kRampBytes = 256 * 4;  // one BGRA texel per ramp entry
    ramp_dirty_ = false;
    Alloc a = Upload(kRampBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = ramp_[kRampBlue + i];
        p[1] = ramp_[kRampGreen + i];
        p[2] = ramp_[kRampRed + i];
        p[3] = 255;
        identity &= ramp_[kRampRed + i] == i && ramp_[kRampGreen + i] == i && ramp_[kRampBlue + i] == i;
    }
    gamma_ = !identity;
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{lut_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint.Offset = a.off;
    sl.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1, 1, kRampBytes};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

// into the target rh
void D3D12Backend::BlitFrame(Target& t, D3D12_CPU_DESCRIPTOR_HANDLE rh) {
    // at the aspect setting's shape, bars where the window is wider or taller
    float aspect = app::Aspect();
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    float uv[4] = {0, 0, 1, 1};
    D3D12_VIEWPORT vp{(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
    D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    binds_ = {};
    D3D12_RESOURCE_STATES* st;
    ID3D12Resource* res = Readable(t, st);
    if (!res) return;
    Transition(res, *st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    DXGI_FORMAT fmt = t.fmt;
    // SMAA at the size drawn, then shown as any picture is; not over the output's, which smooths edges itself
    if (SmaaOn() && t.w <= vw * 1.05f && t.h <= vh * 1.05f) {
        if (ID3D12Resource* aa = Smaa(t)) {
            res = aa;
            fmt = kBgra;
            cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
            bound_rt_ = bound_ds_ = nullptr;
            cl_->RSSetViewports(1, &vp);
            cl_->RSSetScissorRects(1, &scissor);
            binds_ = {};
        }
    }
    // a render scale under the output's: upscaled sharp, by the cubic filter's 16 taps; the low profile takes the
    // bilinear one
    bool cubic = !app::Get().low_profile && (t.w + 1 < vw || t.h + 1 < vh);
    // over the output's (supersampling): each pixel the average of what was drawn under it, which smooths the edges
    // FXAA would. A whole ratio (200%) takes one tap a drawn texel, its exact box; between (150%) the taps are spaced
    // at most half a texel, so their bilinear reads cover the footprint evenly instead of some texels twice
    float ratio = std::max(t.w / vw, t.h / vh);
    float whole = std::round(ratio);
    uint32_t taps = std::abs(ratio - whole) < 0.05f ? uint32_t(whole) : uint32_t(std::ceil(ratio * 2));
    // the brightness setting, as a power: 100% leaves the picture, 150% lifts a middle grey by about half
    float power = 100.0f / float(std::clamp<uint32_t>(app::Get().brightness, 50, 150));
    if (ratio > 1.05f) Blit(res, fmt, uv, {.gamma = gamma_, .taps = std::clamp(taps, 2u, 8u), .power = power});
    else Blit(res, fmt, uv, {.gamma = gamma_, .cubic = cubic, .fxaa = PostAa(), .power = power});
}

ID3D12Resource* D3D12Backend::Smaa(Target& t) {
    static const DXGI_FORMAT kFormats[3] = {DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, kBgra};
    if (smaa_w_ != t.w || smaa_h_ != t.h || !smaa_[2].res) {
        for (SmaaTarget& s : smaa_) {
            if (s.res) CurrentFrame().garbage.push_back(std::move(s.res));
            s.res.Reset();
        }
        for (int i = 0; i < 3; i++) {
            SmaaTarget& s = smaa_[i];
            if (s.view == UINT32_MAX) {
                if (free_rtv_.empty()) return nullptr;
                s.view = free_rtv_.back();
                free_rtv_.pop_back();
            }
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC td = Texture2DDesc(kFormats[i], t.w, t.h);
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv{};
            cv.Format = td.Format;
            s.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, s.state, &cv,
                                                     IID_PPV_ARGS(&s.res)))) {
                s.res.Reset();
                return nullptr;
            }
            dev_->CreateRenderTargetView(s.res.Get(), nullptr, Cpu(rtv_heap_.Get(), s.view, rtv_inc_));
        }
        smaa_w_ = t.w;
        smaa_h_ = t.h;
        LOGI("d3d12: SMAA at %ux%u", t.w, t.h);
    }
    D3D12_RESOURCE_STATES* st;
    ID3D12Resource* src = Readable(t, st);
    if (!src) return nullptr;
    Transition(src, *st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    static const char* kEntries[3] = {"edges", "weights", "blend"};
    const DXGI_FORMAT fmt[3] = {t.fmt, kFormats[0], kFormats[1]};
    const float p[8] = {};
    // each pass reads the picture and the passes before it
    for (int i = 0; i < 3; i++) {
        ID3D12Resource* in[3] = {src, i > 0 ? smaa_[0].res.Get() : nullptr, i > 1 ? smaa_[1].res.Get() : nullptr};
        if (i > 0) Transition(smaa_[i - 1].res.Get(), smaa_[i - 1].state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(smaa_[i].res.Get(), smaa_[i].state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Pass(kSmaaHlsl, kEntries[i], kFormats[i], Cpu(rtv_heap_.Get(), smaa_[i].view, rtv_inc_), t.w, t.h, in, fmt, p,
             -1, 1);
    }
    Transition(smaa_[2].res.Get(), smaa_[2].state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return smaa_[2].res.Get();
}

// The scene's draws (depth tested and written, on the frame's first such target) are counted; the first draw on it
// after 32 of them that writes no depth and tests none (the HUD, a glow, a screen pass) has the occlusion put in
// before it, once a frame. The HUD and the effects laid over the scene are left out that way.
void D3D12Backend::NoteAmbientOcclusion(const State& s, const PsoKey& k) {
    if (ao_frame_ != frame_) {
        ao_frame_ = frame_;
        ao_rt_ = ao_ds_ = nullptr;
        ao_draws_ = 0;
        ao_done_ = false;
    }
    if (ao_done_ || !bound_rt_) return;
    bool always = !k.zen || Cmp(k.zfunc) == D3D12_COMPARISON_FUNC_ALWAYS;
    if (k.has_ds && k.zen && k.zwrite && !always && bound_ds_ && s.rs[kRsZEnable] == 1) {
        if (!ao_rt_) ao_rt_ = bound_rt_, ao_ds_ = bound_ds_;
        if (bound_rt_ == ao_rt_ && bound_ds_ == ao_ds_) ao_draws_++;
        return;
    }
    if (k.zwrite || !always || bound_rt_ != ao_rt_ || ao_draws_ < 32) return;
    ao_done_ = true;
    if (AmbientOcclusion(s)) BindTargets(s);
}

// The projection the title keeps in c-36 to c-33 (TimeSplitters FP: rows of x, y, z and w) and the viewport's depth
// scale and offset (c-38.z, c-37.z) give the view distance back from the depth buffer; a projection of another shape
// leaves the picture as it is.
bool D3D12Backend::AmbientOcclusion(const State& s) {
    const auto& px = s.vsc[60];
    const auto& py = s.vsc[61];
    const auto& pz = s.vsc[62];
    const auto& pw = s.vsc[63];
    float scale = s.vsc[58][2], offset = s.vsc[59][2];
    bool shaped = px[0] != 0 && py[1] != 0 && pz[0] == 0 && pz[1] == 0 && pz[3] != 0 && pw[0] == 0 && pw[1] == 0 &&
                  std::abs(std::abs(pw[2]) - 1) < 1e-3f && pw[3] == 0 && scale > 0 && s.zscale > 0;
    static bool told = false;
    if (!shaped) {
        if (!told) LOGR("d3d12: ambient occlusion off: the projection is not where it is looked for");
        told = true;
        return false;
    }
    Target& rt = *const_cast<Target*>(ao_rt_);
    Target& ds = *const_cast<Target*>(ao_ds_);
    uint32_t aw = (rt.w + 1) / 2, ah = (rt.h + 1) / 2;
    if (ao_w_ != aw || ao_h_ != ah || !ao_.res) {
        if (ao_.res) CurrentFrame().garbage.push_back(std::move(ao_.res));
        ao_.res.Reset();
        if (ao_.view == UINT32_MAX) {
            if (free_rtv_.empty()) return false;
            ao_.view = free_rtv_.back();
            free_rtv_.pop_back();
        }
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_R8_UNORM, aw, ah);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = td.Format;
        ao_.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, ao_.state, &cv,
                                                 IID_PPV_ARGS(&ao_.res)))) {
            ao_.res.Reset();
            return false;
        }
        dev_->CreateRenderTargetView(ao_.res.Get(), nullptr, Cpu(rtv_heap_.Get(), ao_.view, rtv_inc_));
        ao_w_ = aw;
        ao_h_ = ah;
        LOGI("d3d12: ambient occlusion at %ux%u", aw, ah);
    }
    if (cap_) fprintf(cap_, "%04u ambient occlusion %08X\n", cap_i_++, rt.data);
    // z/w = (1 - d) * zscale / scale - offset / scale; the projection's z = a * view z + b, w = c * view z
    const float p[8] = {offset / scale + pz[2] / pw[2], s.zscale / scale, pz[3],      1 / std::abs(px[0]),
                        1 / std::abs(py[1]),            0.75f,           1.0f, float(app::Get().ao)};
    D3D12_RESOURCE_STATES* st;
    ID3D12Resource* depth = Readable(ds, st);
    if (!depth) return false;
    Transition(depth, *st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* in[3] = {depth, nullptr, nullptr};
    const DXGI_FORMAT fmt[3] = {kDepthRead, DXGI_FORMAT_R8_UNORM, kBgra};
    Transition(ao_.res.Get(), ao_.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Pass(kAoHlsl, "occlude", DXGI_FORMAT_R8_UNORM, Cpu(rtv_heap_.Get(), ao_.view, rtv_inc_), aw, ah, in, fmt, p, -1, 1);
    Transition(ao_.res.Get(), ao_.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    in[1] = ao_.res.Get();
    Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Pass(kAoHlsl, "apply", rt.fmt, Cpu(rtv_heap_.Get(), rt.view, rtv_inc_), rt.w, rt.h, in, fmt, p, kPassMultiply,
         Ms(rt));
    return true;
}

void D3D12Backend::RenderUi() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ID3D12DescriptorHeap* uh[] = {ui_heap_.Get()};
    cl_->SetDescriptorHeaps(1, uh);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl_.Get());
}

// into back buffer bi, or kFrames: into out_, left for copying
void D3D12Backend::DrawFrame(Target* t, uint32_t bi) {
    D3D12_RESOURCE_STATES bs = D3D12_RESOURCE_STATE_PRESENT;
    bool out = bi == kFrames;
    Mark(GpuPass::Final);
    ID3D12Resource* img = out ? out_.Get() : bb_[bi].Get();
    D3D12_RESOURCE_STATES& st = out ? out_state_ : bs;
    Transition(img, st, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), kRtvCount + bi, rtv_inc_);
    float black[4] = {0, 0, 0, 1};
    cl_->ClearRenderTargetView(rh, black, 0, nullptr);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    if (t) BlitFrame(*t, rh);
    RenderUi();
    Transition(img, st, out ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_PRESENT);
}

void D3D12Backend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 1024;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 300;
    // textures unused for five seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            CurrentFrame().garbage.push_back(it->second.res);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

// a new target takes over the resources of an idle one of its kind, cleared: a level's targets come at new addresses,
// and making them again (tens of MB each, multisampled) stalls the level's first frames
bool D3D12Backend::ReuseTarget(Target& t) {
    constexpr uint64_t kIdleFrames = 120;
    if (ext_) return false;
    uint32_t samples = t.gw == guest_w_ && t.gh == guest_h_ ? msaa_ : 1;
    for (auto it = targets_.begin(); it != targets_.end(); ++it) {
        Target& old = it->second;
        if (old.depth != t.depth || old.gw != t.gw || old.gh != t.gh || old.samples != samples) continue;
        auto used = target_used_.find(old.data);
        if (used != target_used_.end() && used->second + kIdleFrames >= frame_) continue;
        if (bound_rt_ == &old || bound_ds_ == &old) bound_rt_ = bound_ds_ = nullptr;
        if (draw_rt_ == &old || draw_ds_ == &old) draw_rt_ = draw_ds_ = nullptr;
        t.res = std::move(old.res);
        t.state = old.state;
        t.view = old.view;
        t.fb = std::move(old.fb);
        t.fb_state = old.fb_state;
        t.fb_view = old.fb_view;
        if (old.shrunk) CurrentFrame().garbage.push_back(old.shrunk);
        t.samples = old.samples;
        t.fmt = old.fmt;
        t.w = old.w;
        t.h = old.h;
        targets_.erase(it);
        if (t.depth) {
            Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_),
                                       D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
        } else {
            Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            float black[4] = {0, 0, 0, 0};
            cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), black, 0, nullptr);
        }
        return true;
    }
    return false;
}

// targets neither drawn nor sampled for half a minute go: each level puts its own at new addresses, and the old ones
// (multisampled at the screen's size, tens of MB each) would stay for the rest of the session
void D3D12Backend::ReleaseOldTargets() {
    constexpr uint64_t kSweepEvery = 60;
    constexpr uint64_t kIdleFrames = 1800;
    if (frame_ % kSweepEvery != 0) return;
    for (auto it = targets_.begin(); it != targets_.end();) {
        auto next = std::next(it);
        auto used = target_used_.find(it->second.data);
        if (used == target_used_.end() || used->second + kIdleFrames < frame_) {
            if (draw_rt_ == &it->second || draw_ds_ == &it->second) draw_rt_ = draw_ds_ = nullptr;
            ReleaseTarget(it);
        }
        it = next;
    }
    for (auto it = target_used_.begin(); it != target_used_.end();)
        it = it->second + kIdleFrames < frame_ ? target_used_.erase(it) : std::next(it);
}

// the frame's lists have run: their timestamps add up to the GPU's time on them
// the frame's GPU time in ms, also added to the measurement
double D3D12Backend::ReadGpuTime(Frame& f) {
    if (!time_map_) return 0;
    const uint64_t* q = time_map_ + uint32_t(&f - frames_) * kTimestamps;
    double ms = 0;
    for (uint32_t i = 0; i + 1 < f.queries; i += 2)
        if (q[i + 1] > q[i]) ms += double(q[i + 1] - q[i]) * tick_ms_;
    g_perf.gpu += ms;
    f.queries = 0;
    if (mark_map_) {
        const uint64_t* m = mark_map_ + uint32_t(&f - frames_) * kMarks;
        for (uint32_t i = 0; i + 1 < f.marks; i++)
            if (!f.mark_end[i] && m[i + 1] > m[i]) {
                double ms = double(m[i + 1] - m[i]) * tick_ms_;
                g_perf.pass_ms[size_t(f.mark_pass[i])] += ms;
                if (f.mark_tag[i]) g_perf.by_target[f.mark_tag[i]].first += ms;
            }
    }
    f.marks = f.list_marks = 0;
    return ms;
}

// The render scale fitted to the GPU's speed, once an install: the first video memory guess (FitToAdapter) knows
// nothing of how fast the GPU is. Over windows of five seconds of play the frames' GPU time is taken; a window that
// draws little (under 4 ms: menus, a fast GPU) tells nothing and is passed over. A window whose middle frame takes over
// 15 ms, with no room left for 60 frames a second, lowers the scale a step, never under the console's 480 lines. After
// five minutes of windows that tell, or the player setting the scale or MSAA themselves, it is done for good.
void D3D12Backend::FitRenderScale(double gpu_ms) {
    app::Settings& s = app::Get();
    if (s.render_fitted || gpu_ms <= 0) return;
    if (!fit_percent_) {
        fit_percent_ = s.render_percent;
        fit_msaa_ = s.msaa;
    }
    auto done = [&](const char* why) {
        s.render_fitted = true;
        fit_ms_ = {};
        LOGR("d3d12: render scale fitting done (%s), %u%%", why, s.render_percent);
        app::Save();
    };
    if (s.render_percent != fit_percent_ || s.msaa != fit_msaa_) return done("set by the player");
    constexpr size_t kWindow = 300;
    constexpr uint32_t kWindows = 60;
    fit_ms_.push_back(float(gpu_ms));
    if (fit_ms_.size() < kWindow) return;
    std::nth_element(fit_ms_.begin(), fit_ms_.begin() + kWindow / 2, fit_ms_.end());
    float middle = fit_ms_[kWindow / 2];
    fit_ms_.clear();
    if (middle < 4.0f) return;
    if (middle > 15.0f) {
        constexpr uint32_t kPercents[] = {100, 85, 75, 67, 50, 33, 25};
        uint32_t out = std::max(app::Output().height, 1u);
        uint32_t lowest = std::min(100u, (480 * 100 + out - 1) / out);
        uint32_t next = 0;
        for (uint32_t p : kPercents)
            if (p < s.render_percent && p >= lowest) {
                next = p;
                break;
            }
        if (!next) return done("as low as it goes");
        LOGR("d3d12: the GPU took %.1f ms a frame at render scale %u%%: lowered to %u%%", middle, s.render_percent,
             next);
        s.render_percent = fit_percent_ = next;
        app::Save();
        fit_windows_ = 0;
        return;
    }
    if (++fit_windows_ >= kWindows) done("room for 60 frames a second");
}

void D3D12Backend::ResetFrame() {
    Frame& f = CurrentFrame();
    if (f.fence && fence_->GetCompletedValue() < f.fence) {
        auto perf_wait = PerfClock::now();
        WaitFence(f.fence);
        g_perf.wait += PerfMs(perf_wait);
    }
    FitRenderScale(fit_partial_ms_ + ReadGpuTime(f));
    fit_partial_ms_ = 0;
    f.alloc->Reset();
    f.xalloc->Reset();
    f.garbage.clear();
    f.used = 0;
    f.srv = 0;
    cl_->Reset(f.alloc.Get(), nullptr);
    Begin();
}

void D3D12Backend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        // interpolating: the frames that reached the screen, the title's and those between
        uint64_t shown = shown_total_.load();
        fps_ = (interp_n_ > 1 ? double(shown - fps_shown_) : double(fps_n_)) / el;
        fps_shown_ = shown;
        fps_n_ = 0;
        fps_t_ = now;
    }
}

// The process's threads that used the most CPU since the last call, by name, each a percent of one core: which of them
// keeps the processor busy (and warm) while the frame waits.
void LogCpuByThread(double el_ms) {
    using Describe = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static Describe describe = reinterpret_cast<Describe>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    static std::unordered_map<DWORD, uint64_t> last;  // a thread's kernel and user time, 100 ns
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    std::unordered_map<DWORD, uint64_t> now;
    std::vector<std::pair<double, std::string>> busy;
    double total = 0;
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(h, &c, &e, &k, &u)) {
            uint64_t t = (uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
                         (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime);
            now[te.th32ThreadID] = t;
            auto it = last.find(te.th32ThreadID);
            double pct = it == last.end() ? 0 : double(t - it->second) / 1e4 / el_ms * 100;
            total += pct;
            if (pct >= 2) {
                std::string name = "thread " + std::to_string(te.th32ThreadID);
                PWSTR w = nullptr;
                if (describe && SUCCEEDED(describe(h, &w)) && w) {
                    if (*w) name.assign(w, w + wcslen(w));  // the names are ASCII
                    LocalFree(w);
                }
                busy.push_back({pct, name});
            }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    last = std::move(now);
    std::sort(busy.rbegin(), busy.rend());
    std::string split;
    for (size_t i = 0; i < std::min<size_t>(busy.size(), 8); i++) {
        char part[96];
        std::snprintf(part, sizeof(part), "%s%s %.0f%%", split.empty() ? "" : ", ", busy[i].second.c_str(),
                      busy[i].first);
        split += part;
    }
    LOGR("perf: cpu %.0f%% of a core in all | %s", total, split.c_str());
}

// true once a second, when it has logged
bool LogPerf() {
    static PerfClock::time_point perf_start = PerfClock::now();
    static uint64_t perf_frames = 0;
    // a measurement starting again later (F9) counts from now: no frame takes a quarter second, a gap that long is
    // the time between measurements
    static PerfClock::time_point last = PerfClock::now();
    if (PerfMs(last) > 250.0) {
        perf_start = PerfClock::now();
        perf_frames = 0;
        g_perf = Perf{};
    }
    last = PerfClock::now();
    perf_frames++;
    double el = PerfMs(perf_start);
    if (el >= 1000.0) {
        Perf& p = g_perf;
        double n = double(perf_frames);
        double host = p.draw + p.clear + p.copy + p.present + p.sync;
        LOGR("perf: %.1f fps %.2f ms/frame | host %.2f (draw %.2f: assemble %.2f bind %.2f [check %.2f, upload %.2f] "
             "compile %.2f; clear %.2f copy %.2f sync %.2f present %.2f [swap %.2f wait %.2f]) other %.2f | gpu %.2f",
             n * 1000.0 / el, el / n, host / n, p.draw / n, p.assemble / n, p.bind / n, p.sig / n, p.texup / n,
             p.compile / n, p.clear / n, p.copy / n, p.sync / n, p.present / n, p.swap / n, p.wait / n,
             (el - host) / n, p.gpu / n);
        LOGR("perf: title thread per frame: recording %.2f ms, waiting %.2f ms (%.1f waits: read-back %.2f "
             "resource %.2f fence %.2f flush %.2f copy %.2f, next frame %.2f) | read-backs waited for: %.1f lock, "
             "%.1f NOOVERWRITE",
             g_perf_capture_ns.exchange(0) / n / 1e6, g_perf_wait_ns.exchange(0) / n / 1e6,
             g_perf_waits.exchange(0) / n, g_perf_wait_kind_ns[0].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[1].exchange(0) / n / 1e6, g_perf_wait_kind_ns[2].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[3].exchange(0) / n / 1e6, g_perf_wait_kind_ns[4].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[5].exchange(0) / n / 1e6, g_perf_sync_block.exchange(0) / n,
             g_perf_sync_late.exchange(0) / n);
        LOGR("perf: per frame %.0f draws (%.0f raw) %.0fk verts %.1f MB upload %.1f overflow | %.0f checks %.2f MB "
             "hashed | %.1f tex uploads %.2f MB, %.1f new tex, %.1f new targets, %.1f target uploads, %.1f shrinks, "
             "%.1f read-backs %.1f late | %llu shaders %llu psos this second",
             p.draws / n, p.raw / n, p.verts / n / 1000.0, p.upload / n / 1048576.0, p.overflow / n, p.sigs / n,
             p.sig_bytes / n / 1048576.0, p.texups / n, p.texup_bytes / n / 1048576.0, p.newtex / n,
             p.targets / n, p.rtups / n, p.shrinks / n, p.syncs / n, p.lates / n,
             (unsigned long long)p.shaders, (unsigned long long)p.psos);
        std::string split;
        for (size_t i = 0; i < std::size(kGpuPassNames); i++) {
            if (p.pass_ms[i] < 0.005 * n && !p.pass_draws[i]) continue;
            char part[64];
            std::snprintf(part, sizeof(part), "%s%s %.2f (%.0f)", split.empty() ? "" : ", ", kGpuPassNames[i],
                          p.pass_ms[i] / n, p.pass_draws[i] / n);
            split += part;
        }
        LOGR("perf: gpu ms per frame by work (draws): %s", split.c_str());
        // the targets that took longest, guest size, then host size and samples; resolves count with their target
        std::vector<std::pair<double, uint64_t>> top;
        for (auto& [tag, v] : p.by_target)
            if (tag) top.push_back({v.first, tag});
        std::sort(top.rbegin(), top.rend());
        split.clear();
        for (size_t i = 0; i < std::min<size_t>(top.size(), 6); i++) {
            uint64_t tag = top[i].second;
            char part[96];
            std::snprintf(part, sizeof(part), "%s%s%llux%llu at %llux%llu x%llu %.2f (%.0f)", split.empty() ? "" : ", ",
                          tag >> 60 & 1 ? "depth " : "", tag & 0xFFF, tag >> 12 & 0xFFF, tag >> 24 & 0xFFFF,
                          tag >> 40 & 0xFFFF, tag >> 56 & 0xF, top[i].first / n, p.by_target[tag].second / n);
            split += part;
        }
        if (!split.empty()) LOGR("perf: gpu ms per frame by target (draws): %s", split.c_str());
        LogCpuByThread(el);
        diag::NoteSecond(n * 1000.0 / el, p.gpu / n);
        g_perf = Perf{};
        perf_start = PerfClock::now();
        perf_frames = 0;
        return true;
    }
    return false;
}

// the process's memory and the GPU's, with what the targets and textures hold of it
void D3D12Backend::LogMemory() {
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof(pm);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm));
    DXGI_QUERY_VIDEO_MEMORY_INFO vm{};
    ComPtr<IDXGIAdapter3> adapter;
    if (SUCCEEDED(factory_->EnumAdapterByLuid(dev_->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
        adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm);
    uint64_t target_bytes = 0, texture_bytes = 0;
    for (auto& [key, t] : targets_) target_bytes += uint64_t(t.w) * t.h * 4 * (Ms(t) + (t.fb ? 1 : 0));
    for (auto& [key, t] : textures_) {
        if (!t.res) continue;
        D3D12_RESOURCE_DESC d = t.res->GetDesc();
        uint64_t bytes = 0;
        dev_->GetCopyableFootprints(&d, 0, d.MipLevels * d.DepthOrArraySize, 0, nullptr, nullptr, nullptr, &bytes);
        texture_bytes += bytes;
    }
    constexpr double kMb = 1048576.0;
    LOGR("perf: memory: process %.0f MB working set, %.0f MB private | GPU %.0f MB of %.0f MB budget | %zu targets "
         "%.0f MB at %ux%u, %zu textures %.0f MB, %.0f MB upload rings",
         pm.WorkingSetSize / kMb, pm.PrivateUsage / kMb, vm.CurrentUsage / kMb, vm.Budget / kMb, targets_.size(),
         target_bytes / kMb, uint32_t(std::lround(guest_w_ * scale_.x)), uint32_t(std::lround(guest_h_ * scale_.y)),
         textures_.size(), texture_bytes / kMb, kFrames * kUploadSize / kMb);
}

void D3D12Backend::LogDebugMessages() {
    static uint32_t logged = 0;
    UINT64 n = info_->GetNumStoredMessages();
    for (UINT64 i = 0; i < n && logged < 300; i++) {
        SIZE_T size = 0;
        info_->GetMessage(i, nullptr, &size);
        std::vector<uint8_t> buf(size);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(info_->GetMessage(i, m, &size)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            LOGR("d3d12 debug: %.*s", int(m->DescriptionByteLength), m->pDescription);
            logged++;
        }
    }
    info_->ClearStoredMessages();
}

void D3D12Backend::Present(const Surface& front) {
    PerfScope perf_present{g_perf.present};
    if (info_) LogDebugMessages();
    // XBE_CAPTURE_AT=<seconds>: one capture that long after the first present, as F3 would
    static const double capture_at = [] {
        const char* s = std::getenv("XBE_CAPTURE_AT");
        return s ? std::atof(s) : -1.0;
    }();
    static const auto first_present = std::chrono::steady_clock::now();
    // XBE_CAPTURE_COUNT: that many frames in a row from then
    static int capture_left = [] {
        const char* s = std::getenv("XBE_CAPTURE_COUNT");
        return s ? std::max(std::atoi(s), 1) : 1;
    }();
    if (cap_) FinishCapture(front);
    if (capture_at >= 0 && capture_left > 0 &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - first_present).count() >= capture_at) {
        capture_left--;
        g_capture = true;
    }
    if (!cap_ && g_capture.exchange(false) && CaptureOn()) StartCapture();
    ResizeSwap();
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t) Resolve(*t);
    // XBE_SHOT_EVERY=<seconds>: the frame saved that often as shot_<ms>.bmp in XBE_CAPTURE_DIR, no window focus needed
    static const double shot_every = [] {
        const char* s = std::getenv("XBE_SHOT_EVERY");
        return s ? std::atof(s) : 0.0;
    }();
    // XBE_SHOT_FROM=<seconds>: the first one that late
    static double next_shot = [] {
        const char* s = std::getenv("XBE_SHOT_FROM");
        return s ? std::atof(s) : shot_every;
    }();
    double since = std::chrono::duration<double>(std::chrono::steady_clock::now() - first_present).count();
    if (t && !cap_ && shot_every > 0 && since >= next_shot && !diag::CaptureDir().empty()) {
        next_shot += shot_every;
        std::filesystem::path keep = cap_dir_;
        std::error_code ec;
        cap_dir_ = app::ExeDir() / diag::CaptureDir() / "shots";
        std::filesystem::create_directories(cap_dir_, ec);
        char name[32];
        snprintf(name, sizeof(name), "shot_%06d", int(since * 1000));
        CapSave(*t, name);
        cap_dir_ = keep;
    }
    if (ramp_dirty_) UploadGammaRamp();
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // images for the most frames ever shown at this rate, not this frame's few: made once, not every other frame
    if (interp_n_ > 1 && EnsureInterp(std::min(kInterpMax, uint32_t(std::ceil(interp_rate_ - 0.001))))) {
        DrawFrame(t, kFrames);
        InterpPresent(t);
        DeliverReports();
    } else {
        // the presenter done with the swap chain first
        DrainPresenter();
        std::lock_guard lock(swap_lock_);
        uint32_t bi = swap_->GetCurrentBackBufferIndex();
        DrawFrame(t, bi);
        Submit(false);
        DeliverReports();
        auto perf_swap = PerfClock::now();
        WaitLatency();
        PresentSwap();
        g_perf.swap += PerfMs(perf_swap);
    }

    present_fence_[presents_ % (kFrames + 1)].store(fence_value_);
    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    ReleaseOldTextures();
    ReleaseOldTargets();
    if (pso_lib_dirty_ && PerfClock::now() - pso_lib_stored_ > std::chrono::seconds(3)) SavePipelineLibrary();
    // pipelines the thread made ahead stored in the library: written back like the render thread's
    if (prebuild_stored_.exchange(false)) {
        pso_lib_dirty_ = true;
        pso_lib_stored_ = PerfClock::now();
    }
    LogBuilds();
    ResetFrame();
    CountFps();
    if (diag::Perf() && LogPerf()) LogMemory();
    ApplySettings();
    InterpFrameStart();
}

// the slot is not reused before the caller's wait: the title runs at most one present ahead
void D3D12Backend::WaitGpuFrame(uint64_t n) {
    uint64_t fence = present_fence_[n % (kFrames + 1)].load();
    if (fence > fence_->GetCompletedValue()) fence_->SetEventOnCompletion(fence, nullptr);
}

void D3D12Backend::ApplySettings() {
    app::Scale2 want = app::RenderScale(guest_w_, guest_h_);
    bool remake = ext_ && ext_->Settings();
    if (uint32_t latency = FrameLatency(); swap_ && latency != latency_set_) {
        latency_set_ = latency;
        swap_->SetMaximumFrameLatency(latency);
        LOGR("d3d12: %u frame%s queued for the display", latency, latency > 1 ? "s" : "");
    }
    uint32_t msaa = SceneMsaa();
    if (std::fabs(want.x - scale_.x) > 1e-4f || std::fabs(want.y - scale_.y) > 1e-4f || remake ||
        msaa != msaa_set_) {
        Flush();
        if (msaa != msaa_set_) {
            msaa_set_ = msaa;
            msaa_ = Samples(msaa);
        }
        for (auto& [key, tg] : targets_) {
            (tg.depth ? free_dsv_ : free_rtv_).push_back(tg.view);
            if (tg.fb_view != ~0u) free_dsv_.push_back(tg.fb_view);
        }
        targets_.clear();
        scale_ = want;
        if (ext_) ext_->Reset(*this);
        bound_rt_ = bound_ds_ = nullptr;
    }
}

// the target resolved, then shrunk with o when it is not guest sized BGRA; res null on failure
D3D12Backend::CopySource D3D12Backend::PrepareCopySource(Target& t, const ShrinkOptions& o) {
    Resolve(t);
    D3D12_RESOURCE_STATES* st;
    ID3D12Resource* res = Readable(t, st);
    CopySource out{res, nullptr};
    if (!res) return out;
    if (t.w != t.gw || t.h != t.gh || t.fmt != kBgra) {
        out.down = Shrink(res, *st, t.fmt, t.gw, t.gh, o);
        out.res = out.down.Get();
    } else {
        Transition(res, *st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    return out;
}

// long unused: the title freed it and other data is at this address now
bool D3D12Backend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return frame_ - used > kStaleFrames;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void D3D12Backend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn || TargetStale(data)) return;
    PerfScope perf_sync{g_perf.sync};
    g_perf.syncs++;
    if (diag::Perf()) {
        LOG_FIRST(16, "perf: read-back of %08X (%ux%u guest, %ux%u host)", data, it->second.gw, it->second.gh,
                  it->second.w, it->second.h);
    }
    Target& t = it->second;
    // src.down, when there is one, stays alive until ReadBack is done
    CopySource src = PrepareCopySource(t, {});
    if (!src.res) return;
    std::vector<uint8_t> px;
    if (!ReadBack(src.res, t.gw, t.gh, px)) return;
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, so a surface locked every frame gets the newest
// finished copy (a frame or two old) and the CPU never waits on the GPU; one locked now and then still waits in Sync
void D3D12Backend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool D3D12Backend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = fence_->GetCompletedValue();
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        bool newest = best < 0 || l.fence[i] > l.fence[best];
        if (finished && recent && newest) best = i;
    }
    if (best < 0) {
        if (diag::Perf()) {
            LOG_FIRST(16, "perf: late %08X nothing finished: done %llu now %llu, slots %llu/%llu %llu/%llu %llu/%llu",
                      data, (unsigned long long)done, (unsigned long long)now, (unsigned long long)l.fence[0],
                      (unsigned long long)l.frame[0], (unsigned long long)l.fence[1], (unsigned long long)l.frame[1],
                      (unsigned long long)l.fence[2], (unsigned long long)l.frame[2]);
        }
        return false;
    }
    if (l.fence[best] > l.delivered) {
        uint8_t* p = nullptr;
        // the last row is not padded to the pitch: a range to the end of the pitch fails to map
        D3D12_RANGE rr{0, size_t(l.row_pitch) * (l.gh - 1) + l.gw * 4};
        if (FAILED(l.rb[best]->Map(0, &rr, reinterpret_cast<void**>(&p)))) return false;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.row_pitch, l.gw * 4);
        D3D12_RANGE none{0, 0};
        l.rb[best]->Unmap(0, &none);
        l.delivered = l.fence[best];
    }
    return true;
}

// a slot set for another size: its buffers go with the frame
void D3D12Backend::ResetLate(Late& l, const Target& t) {
    auto& garbage = CurrentFrame().garbage;
    for (auto& r : l.rb) {
        if (r) garbage.push_back(r);
    }
    if (l.down) garbage.push_back(l.down);
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

// the target as a guest sized BGRA copy source, shrunk into l.down when it is not one already
ID3D12Resource* D3D12Backend::PrepareLateSource(Target& t, Late& l) {
    ShrinkOptions reuse{.into = l.down.Get(), .into_state = l.down_state};
    CopySource src = PrepareCopySource(t, reuse);
    if (src.down) {
        l.down = src.down;
        l.down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    return src.res;
}

// src into the readback buffer of the slot, made on first use
void D3D12Backend::RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src) {
    D3D12_RESOURCE_DESC rd = src->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    if (!l.rb[slot]) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&l.rb[slot])))) {
            return;
        }
    }
    D3D12_TEXTURE_COPY_LOCATION dl{l.rb[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    l.row_pitch = fp.Footprint.RowPitch;
    l.fence[slot] = fence_value_ + 1;  // the next Submit carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

void D3D12Backend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || TargetStale(data)) return;
    Target& t = it->second;
    PerfScope perf_sync{g_perf.sync};
    g_perf.lates++;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        if (diag::Perf()) LOG_FIRST(16, "perf: late %08X ring reset, %ux%u -> %ux%u", data, l.gw, l.gh, t.gw, t.gh);
        ResetLate(l, t);
    }
    // a copy for a later lock, unless the GPU is a whole ring behind
    uint32_t slot = l.next;
    if (l.fence[slot] > fence_->GetCompletedValue()) return;
    ID3D12Resource* src = PrepareLateSource(t, l);
    if (!src) return;
    RecordLateCopy(l, slot, src);
}

// filters src into a new w x h target, left as a copy source
ComPtr<ID3D12Resource> D3D12Backend::Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt,
                                            uint32_t w, uint32_t h, const ShrinkOptions& o) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = td.Format;
    ComPtr<ID3D12Resource> down = o.into;
    D3D12_RESOURCE_STATES down_state = o.into ? o.into_state : D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_perf.shrinks++;
    if (free_rtv_.empty()) return nullptr;
    if (down) {
        Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&down)))) {
        return nullptr;
    }
    // borrows the next free view without taking it, only for this recording
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), free_rtv_.back(), rtv_inc_);
    dev_->CreateRenderTargetView(down.Get(), nullptr, rh);
    Transition(src, st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SetBlitTarget(rh, w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(src, fmt, uv, {.alpha = true, .taps = o.taps, .mul = o.mul});
    Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    return down;
}

// a BGRA copy source to the CPU, rows packed; waits for the GPU
bool D3D12Backend::ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    ComPtr<ID3D12Resource> rb;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&rb)))) {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION dl{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Flush();
    uint8_t* p = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
    out.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++) std::memcpy(&out[size_t(y) * w * 4], p + size_t(y) * fp.Footprint.RowPitch, w * 4);
    rb->Unmap(0, nullptr);
    return true;
}

// BMP file and info header: byte offsets of the fields CapSave writes
constexpr int kBmpHeaderBytes = 54;
constexpr int kBmpFileSize = 2;
constexpr int kBmpPixelOffset = 10;
constexpr int kBmpInfoSize = 14;
constexpr int kBmpWidth = 18;
constexpr int kBmpHeight = 22;  // negative: rows top down
constexpr int kBmpPlanes = 26;
constexpr int kBmpBitCount = 28;
constexpr int kBmpImageSize = 34;
constexpr uint32_t kBmpInfoBytes = 40;

void D3D12Backend::CapSave(const CapImage& img, const char* name) {
    ComPtr<ID3D12Resource> down = Shrink(img.res, *img.state, img.fmt, img.w, img.h, {.mul = img.mul});
    std::vector<uint8_t> px;
    if (!down || !ReadBack(down.Get(), img.w, img.h, px)) return;
    FILE* f = _wfopen((cap_dir_ / (std::string(name) + ".bmp")).c_str(), L"wb");
    if (!f) return;
    uint8_t hd[kBmpHeaderBytes] = {'B', 'M'};  // BITMAPFILEHEADER and BITMAPINFOHEADER, top down 32 bit
    auto put = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    put(kBmpFileSize, kBmpHeaderBytes + uint32_t(px.size()));
    put(kBmpPixelOffset, kBmpHeaderBytes);
    put(kBmpInfoSize, kBmpInfoBytes);
    put(kBmpWidth, img.w);
    put(kBmpHeight, uint32_t(-int32_t(img.h)));
    hd[kBmpPlanes] = 1;
    hd[kBmpBitCount] = 32;
    put(kBmpImageSize, uint32_t(px.size()));
    fwrite(hd, 1, kBmpHeaderBytes, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

void D3D12Backend::CapSave(Target& t, const char* name) {
    DXGI_FORMAT fmt = t.depth ? kDepthRead : t.fmt;
    float mul = t.depth ? 1 : t.range;
    D3D12_RESOURCE_STATES* st;
    ID3D12Resource* res = Readable(t, st);
    if (!res) return;
    CapSave({.res = res, .state = st, .fmt = fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

// the frame moves to another target: save the one it leaves
void D3D12Backend::CapStep(const Surface& rt) {
    uint32_t d = rt.header ? mem::VirtualFromPhysical(rt.data) : 0;
    if (d == cap_rt_) return;
    auto it = targets_.find(TargetMapKey(cap_rt_, false));
    if (cap_rt_ && it != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X", cap_i_, cap_rt_);
        CapSave(it->second, name);
    }
    auto z = targets_.find(TargetMapKey(cap_rt_, true));
    if (cap_rt_ && z != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X_z", cap_i_, cap_rt_);
        CapSave(z->second, name);
    }
    cap_rt_ = d;
}

void D3D12Backend::Capture(Target& t, const char* tag, float mul) {
    if (!cap_) return;
    char name[32];
    snprintf(name, sizeof(name), "%04u_%s_%08X", cap_i_, tag, t.data);
    CapSave({.res = t.res.Get(), .state = &t.state, .fmt = t.fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

void D3D12Backend::SetGamma(const uint8_t* ramp) {
    std::memcpy(ramp_, ramp, sizeof(ramp_));
    ramp_dirty_ = true;
}

}  // namespace

// the display's refresh rate, 60 when unknown
uint32_t D3D12Backend::RefreshRate() const {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(app::Window()), MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
        dm.dmDisplayFrequency > 1)
        return dm.dmDisplayFrequency;
    return 60;
}

// frames shown for each the title draws: the setting, under vertical sync no more than the display refreshes in one
// (a fraction when the refresh is no multiple of the title's rate; within a twentieth of a whole number, that number)
double D3D12Backend::InterpRate() const {
    if (cap_) return 1;
    // XBE_FRAME_INTERP=<n>: that many, whatever the setting and the display
    static const char* force = std::getenv("XBE_FRAME_INTERP");
    if (force) return std::clamp<uint32_t>(uint32_t(std::atoi(force)), 1, kInterpMax);
    double r = std::clamp<uint32_t>(app::Get().frame_interp, 1, kInterpMax);
    if (app::Get().vsync) {
        double d = refresh_ * period_ms_ / 1000.0;
        if (std::fabs(d - std::round(d)) < 0.05) d = std::round(d);
        r = std::min(r, std::max(1.0, d));
    }
    return r;
}

// a new frame: its draws matched to the last one's, its constants and states kept when it is to be shown more than once
void D3D12Backend::InterpFrameStart() {
    interp_rate_ = InterpRate();
    interp_n_ = 1;
    interp_ts_ = 0;
    if (interp_rate_ > 1.001) {
        // the frames shown a step apart, carried over from frame to frame; one falling on this frame is this frame
        double step = 1.0 / interp_rate_;
        double& p = interp_phase_;
        if (p <= 0 || p > step + 0.01) p = step;
        uint32_t most = std::min(kInterpMax, uint32_t(std::ceil(interp_rate_ - 0.001)));
        while (p <= 1.001 && interp_ts_ < most) {
            interp_t_[interp_ts_++] = p >= 0.999 ? 1.0f : float(p);
            p += step;
        }
        p -= 1.0;
        interp_n_ = interp_ts_ + 1 - (interp_t_[interp_ts_ - 1] == 1.0f);
    }
    if (interp_n_ > 1) {
        // the constants: the arena the draws read, and this frame's slots, a block per frame shown
        D3D12_HEAP_PROPERTIES dh{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC bd = BufferDesc(kInterpBlock);
        if (!interp_arena_ && FAILED(dev_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &bd,
                                                                  D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                  IID_PPV_ARGS(&interp_arena_)))) {
            interp_n_ = 1;
        }
        Frame& f = CurrentFrame();
        if (interp_n_ > 1 && f.stage_blocks < interp_n_) {
            f.stage.Reset();
            f.stage_map = nullptr;
            f.stage_blocks = 0;
            D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
            D3D12_RESOURCE_DESC sd = BufferDesc(kInterpBlock * interp_n_);
            if (SUCCEEDED(dev_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &sd,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&f.stage))) &&
                SUCCEEDED(f.stage->Map(0, nullptr, reinterpret_cast<void**>(&f.stage_map)))) {
                f.stage_blocks = interp_n_;
            } else {
                LOGE("d3d12: no memory for frame interpolation");
                interp_n_ = 1;
            }
        }
    }
    interp_whole_ = interp_n_ > 1;
    interp_slots_ = 0;
    interp_frame_ = {};
    interp_match_.NextFrame();
    named_older_.swap(named_newer_);
    named_newer_.clear();
    interp_same_.clear();
    interp_shared_.clear();
    frame_states_.clear();
    track_states_ = interp_whole_;
}

// the GPU waited on in the middle of the frame: what ran cannot run again, the frame is shown once
void D3D12Backend::InterpSplit() {
    if (!interp_whole_) return;
    interp_whole_ = false;
    track_states_ = false;
    frame_states_.clear();
    interp_counts_.split++;
}

// a draw as told from the same one in the frame before: its program, geometry, first textures and targets
interp::Keys D3D12Backend::InterpKey(const State& s, uint32_t prim, uint32_t start, uint32_t count,
                                 const uint8_t* indices) const {
    // the indices by their first bytes, not where they are: the render thread hands each frame's draws their own copy
    uint64_t at = indices ? Hash(indices, std::min<size_t>(size_t(count) * 2, 256)) : 0;
    // buffers and textures by their data's address (header +4): the render thread hands the draws copies of their
    // headers, copied again whenever the title's own changes (its reference counts move every frame)
    auto data = [](uint32_t header) { return header ? kernel::R32(header + 4) : 0u; };
    uint32_t k[] = {s.vs_handle,          data(s.streams[0].vb),  data(s.streams[1].vb), s.streams[0].stride, prim,
                    start,                count,                  data(s.textures[0]),   data(s.textures[1]), s.rt.data,
                    s.zs.data,            uint32_t(at),           uint32_t(at >> 32)};
    std::memcpy(interp_fields_, k, sizeof(k));
    uint32_t shape[] = {k[0], k[3], k[4], k[6], k[7], k[8], k[9], k[10]};
    uint32_t batch[] = {k[0], k[3], k[4], k[9], k[10]};
    return {Hash(k, sizeof(k)), Hash(shape, sizeof(shape)), Hash(batch, sizeof(batch))};
}

// the draw's vertex constants: a slot of the arena, the frame's own in block 0 and each in-between frame's blended in
// the blocks after; the upload ring's past the slots or in a frame shown once
D3D12_GPU_VIRTUAL_ADDRESS D3D12Backend::InterpConstants(const State& s) {
    interp::Regs regs;
    std::memcpy(regs.data(), &s.vsc[kInterpFirst], sizeof(regs));
    // a skinned draw's bones matched and blended with the rest; its registers above are the projection alone, the
    // same for every character, so the bones tell its draws apart too
    InterpUse use;
    if ((s.vs_handle & 1) && s.vs && !s.vs->program.empty()) {
        if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
        auto [u, added] = interp_use_.try_emplace(s.vs->hash);
        if (added) u->second = {ReadsBones(s.vs->program), ReadsGroups(s.vs->program, kInterpFirst) | 1};
        use = u->second;
    }
    bool skinned = use.bones;
    interp::Bones bones;
    if (skinned) std::memcpy(bones.data(), &s.vsc[interp::kBoneFirst], sizeof(bones));
    int32_t older = interp_match_.Match(interp_key_, regs, skinned ? &bones : nullptr, use.groups);
    uint64_t same_key = Hash(regs.data(), sizeof(regs));
    if (skinned) same_key = Hash(bones.data(), sizeof(bones), same_key);
    auto [same, fresh] = interp_same_.try_emplace(same_key, older);
    // drawn as the first of them was, blended or not: the first pass lays the depth the others must meet
    if (!fresh) older = same->second;
    // the last frame's registers: the matched draw's, its named groups replaced by the last frame's of their name
    // (a draw matched with no other is blended by its named groups alone, the rest left as they are)
    interp::Regs older_regs = older >= 0 ? interp_match_.Older(older) : regs;
    interp::Bones older_bones;
    if (skinned) {
        const interp::Bones* ob = older >= 0 ? interp_match_.OlderBones(older) : nullptr;
        older_bones = ob ? *ob : bones;
    }
    uint32_t named = 0;
    // this frame's matrix of a name kept, and the last frame's of it (null: none, or no one's)
    auto keep = [&](uint32_t id, const std::array<float, 4>* now) -> const Mat4* {
        auto [it, fresh] = named_newer_.try_emplace(id);
        if (fresh) std::memcpy(it->second.m.data(), now, sizeof(Mat4));
        else if (std::memcmp(it->second.m.data(), now, sizeof(Mat4))) it->second.twice = true;
        interp_counts_.named++;
        auto old = named_older_.find(id);
        if (old == named_older_.end() || old->second.twice) return nullptr;
        interp_counts_.named_found++;
        return &old->second.m;
    };
    // (the projection, group 0, the same way: the camera a frame has is one, whatever draw was matched)
    bool exact = older >= 0 && fresh && interp_match_.LastTier() == 0;
    // how each of the draw's matrices (the projection's group not among them) found its last frame's, for the log:
    // by its name, by a draw with the same one, by this draw's exact match, by its guess (shape, batch), not at all
    uint32_t paired[5] = {};
    for (uint32_t g = 0; g < interp::kGroups; g++) {
        uint32_t reg = kInterpFirst + g * 4;
        if (uint32_t id = g ? s.vsc_id[reg / 4] : 0)
            if (const Mat4* m = keep(id, &regs[g * 4])) {
                std::memcpy(&older_regs[g * 4], m->data(), sizeof(Mat4));
                named++;
                paired[0]++;
                continue;
            }
        if (g && !((use.groups >> g) & 1)) continue;
        static const Mat4 zero{};
        if (!std::memcmp(&regs[g * 4], zero.data(), sizeof(Mat4))) continue;
        uint64_t key = Hash(&regs[g * 4], sizeof(Mat4), g);
        if (auto it = interp_shared_.find(key); it != interp_shared_.end()) {
            std::memcpy(&older_regs[g * 4], it->second.data(), sizeof(Mat4));
            named++;
            if (g) paired[1]++;
            continue;
        }
        if (exact) std::memcpy(interp_shared_[key].data(), &older_regs[g * 4], sizeof(Mat4));
        if (g) paired[older < 0 ? 4 : exact ? 2 : 3]++;
    }
    // a palette's bones by their names when every one of them was found, else all of them the matched draw's: some
    // by name and the rest by slot were two pairings in one piece, and bones that seemed to jump (gone through
    // whole to the newer frame, the piece flickering beside its blended neighbours)
    bool bones_by_name = false;
    if (skinned) {
        const Mat4* found[interp::kBoneGroups] = {};
        uint32_t bones_named = 0, bones_found = 0;
        for (uint32_t g = 0; g < interp::kBoneGroups; g++) {
            uint32_t id = s.vsc_id[(interp::kBoneFirst + g * 4) / 4];
            if (!id) continue;
            bones_named++;
            found[g] = keep(id, &bones[g * 4]);
            bones_found += found[g] != nullptr;
        }
        bones_by_name = bones_named && bones_found == bones_named;
        for (uint32_t g = 0; g < interp::kBoneGroups && bones_named; g++) {
            // past a named palette's bones the registers hold an earlier draw's: left as they are, not moved
            if (!s.vsc_id[(interp::kBoneFirst + g * 4) / 4])
                std::memcpy(&older_bones[g * 4], &bones[g * 4], sizeof(Mat4));
            else if (bones_by_name)
                std::memcpy(&older_bones[g * 4], found[g]->data(), sizeof(Mat4));
        }
        if (bones_by_name) named++;
    }
    float projection[4][4];
    std::memcpy(projection, regs.data(), sizeof(projection));
    bool is3d = interp::Perspective(projection);
    // where draws that are not 3D at the blended registers keep a projection: logged, one draw in 64 looked at
    static uint32_t look = 0;
    if (!is3d && (look++ & 63) == 0) {
        static std::set<uint32_t> found;
        for (uint32_t r = 0; r + 4 <= 192 && found.size() < 16; r++) {
            float m[4][4];
            std::memcpy(m, &s.vsc[r], sizeof(m));
            if (interp::Perspective(m) && found.insert(r).second)
                LOGR("interp: a projection at c%d (vs %08X)", int(r) - 96, s.vs_handle);
        }
    }
    Frame& f = CurrentFrame();
    if (!interp_whole_ || interp_slots_ >= kInterpDraws || f.stage_blocks < interp_n_) {
        if (interp_whole_) interp_counts_.full++;
        Alloc vc = Upload(kVscBytes, 256);
        std::memcpy(vc.cpu, s.vsc.data(), kVscBytes);
        return vc.gpu;
    }
    uint32_t slot = interp_slots_++;
    uint8_t* at = f.stage_map + uint64_t(slot) * kVscBytes;
    std::memcpy(at, s.vsc.data(), kVscBytes);
    interp::Outcome o;
    bool bones_cut = false;
    for (uint32_t j = 1; j < interp_n_; j++) {
        uint8_t* to = at + j * kInterpBlock;
        std::memcpy(to, s.vsc.data(), kVscBytes);
        // screen-space draws (HUD, menus) as the frame has them: their registers are no matrices to blend
        if ((older < 0 && !named) || !is3d) continue;
        // XBE_INTERP_T=<t>: every frame between at that blend, a test of the redraw (1: the newer frame's matrices,
        // so any difference from the title's own image is the redraw's; 0: the older's, what does not follow them)
        static const char* forced = std::getenv("XBE_INTERP_T");
        float t = forced ? float(std::atof(forced)) : interp_t_[j - 1];
        static bool said = forced && (LOGR("interp: XBE_INTERP_T, every frame between at %g", t), true);
        (void)said;
        interp::Regs blended;
        o = interp::Blend(older_regs, regs, t, blended, use.groups);
        std::memcpy(to + kInterpFirst * 16, blended.data(), sizeof(blended));
        if (skinned) {
            interp::Bones blended_bones;
            interp::Outcome ob = interp::BlendBones(older_bones, bones, t, blended_bones, bones_by_name);
            std::memcpy(to + interp::kBoneFirst * 16, blended_bones.data(), sizeof(blended_bones));
            bones_cut |= ob.cut && bones_by_name;  // by slot a jump is someone else's bone, nothing of this draw
            o.moved |= ob.moved;
        }
    }
    // the first 3D draw of two frames every five seconds, its key's parts: what changes between frames when nothing matches
    if (is3d && interp_frame_.draws3d == 0 && interp_counts_.frames < 2 && interp_frames_logged_ < 40) {
        interp_frames_logged_++;
        const uint32_t* k = interp_fields_;
        LOGR("interp: first 3D draw %s: vs %X vb %X/%X stride %u prim %u start %u count %u tex %X/%X rt %X zs %X idx "
             "%X%08X", older >= 0 ? "matched" : "new", k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], k[8], k[9],
             k[10], k[12], k[11]);
    }
    if (is3d) {
        interp_frame_.draws3d++;
        if (older >= 0) interp_frame_.matched++;
        if (older >= 0) interp_counts_.tiers[interp_match_.LastTier()]++;
        if (skinned) {
            interp_counts_.skinned++;
            interp_counts_.skinned_tiers[older >= 0 ? interp_match_.LastTier() : interp::kTiers]++;
            interp_counts_.bones_cut += bones_cut;
            interp_counts_.by_name += bones_by_name;
        }
        // a piece whose bones jumped counts with the cut draws: a shot cut to another is most of them, and the
        // frame held whole as the newer has it (InterpPresent), not its pieces each in another place
        if (o.cut || bones_cut) interp_frame_.cut++;
        for (int i = 0; i < 5; i++) interp_counts_.paired[i] += paired[i];
        if (o.kept) interp_frame_.kept++;
    }
    if (o.moved) interp_frame_.moved++;
    return interp_arena_->GetGPUVirtualAddress() + uint64_t(slot) * kVscBytes;
}

// out_ and the images shown at the back buffers' size, for n frames shown a title frame
bool D3D12Backend::EnsureInterp(uint32_t n) {
    if (!interp_arena_) return false;
    if (out_ && shown_w_ == bb_w_ && shown_h_ == bb_h_ && shown_n_ >= n) return true;
    DrainPresenter();
    Frame& f = CurrentFrame();
    if (out_) f.garbage.push_back(out_);
    out_.Reset();
    for (auto& set : shown_)
        for (auto& img : set) {
            if (img) f.garbage.push_back(img);
            img.Reset();
        }
    shown_n_ = 0;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(kBgra, bb_w_, bb_h_);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                             IID_PPV_ARGS(&out_)))) {
        return false;
    }
    out_state_ = D3D12_RESOURCE_STATE_COPY_SOURCE;
    dev_->CreateRenderTargetView(out_.Get(), nullptr, Cpu(rtv_heap_.Get(), kRtvCount + kFrames, rtv_inc_));
    td.Flags = D3D12_RESOURCE_FLAG_NONE;
    for (auto& set : shown_)
        for (uint32_t i = 0; i < n; i++)
            if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                     nullptr, IID_PPV_ARGS(&set[i])))) {
                out_.Reset();
                return false;
            }
    shown_w_ = bb_w_;
    shown_h_ = bb_h_;
    shown_n_ = n;
    return true;
}

void D3D12Backend::BeginExtra() { xl_->Reset(CurrentFrame().xalloc.Get(), nullptr); }

void D3D12Backend::RunList(ID3D12GraphicsCommandList* l) {
    ID3D12CommandList* lists[] = {l};
    queue_->ExecuteCommandLists(1, lists);
}

// the arena given the block's constants (a buffer: promoted for the copy and back to common after the list)
void D3D12Backend::FillArena(uint32_t block) {
    xl_->CopyBufferRegion(interp_arena_.Get(), 0, CurrentFrame().stage.Get(), uint64_t(block) * kInterpBlock,
                          uint64_t(interp_slots_) * kVscBytes);
}

static D3D12_RESOURCE_BARRIER StateBarrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

// out_ into an image to show, which stays a copy source between
void D3D12Backend::CopyShown(ID3D12Resource* img) {
    D3D12_RESOURCE_BARRIER b = StateBarrier(img, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    xl_->ResourceBarrier(1, &b);
    xl_->CopyResource(img, out_.Get());
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    xl_->ResourceBarrier(1, &b);
}

// every texture the frame moved back to the state it found it in, for the frame's list to run again
void D3D12Backend::RestoreStates() {
    std::vector<D3D12_RESOURCE_BARRIER> bs;
    for (auto& [r, st] : frame_states_) {
        // buffers go back to common after each list by themselves
        if (st.first == st.second || r->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) continue;
        bs.push_back(StateBarrier(r, st.second, st.first));
    }
    if (!bs.empty()) xl_->ResourceBarrier(UINT(bs.size()), bs.data());
}

// the frame's list run once for each frame shown, then the images handed to the presenter with their times
void D3D12Backend::InterpPresent(Target*) {
    using clock = std::chrono::steady_clock;
    auto ms = [](double v) { return std::chrono::duration_cast<clock::duration>(std::chrono::duration<double, std::milli>(v)); };
    Frame& f = CurrentFrame();
    uint32_t images = interp_ts_;
    bool own_shown = interp_t_[images - 1] == 1.0f;  // falling between two of the title's, the frame itself is not
    // the title's period, and the timeline its frames are shown on: a period after the last, pulled toward now
    auto now = clock::now();
    if (last_present_ != clock::time_point{}) {
        double dt = std::chrono::duration<double, std::milli>(now - last_present_).count();
        if (dt > 4 && dt < 100) period_ms_ += (dt - period_ms_) * 0.05;
    }
    last_present_ = now;
    auto period = ms(period_ms_);
    auto next = base_ + period;
    if (base_ == clock::time_point{} || now > next + period / 2 || now + period / 2 < next) base_ = now;
    else base_ = next + (now - next) / 10;
    double slot_ms = period_ms_ / interp_rate_;
    auto slot = ms(slot_ms);
    // XBE_INTERP_DELAY=<ms>: from the title's present to the first frame shown, half a frame shown's time otherwise
    static const char* delay_env = std::getenv("XBE_INTERP_DELAY");
    auto delay = delay_env ? ms(std::atof(delay_env)) : slot / 2;
    // a frame shown at where it falls between the last and this one, the one a slot from the start shown first
    auto when = [&](float t) { return base_ + delay + ms(period_ms_ * t) - slot; };

    const InterpFrame& fr = interp_frame_;
    // XBE_INTERP_ALWAYS: every whole frame drawn again, moving or not (a test of the runs themselves)
    static const bool always = std::getenv("XBE_INTERP_ALWAYS") != nullptr;
    bool blend = interp_whole_ && ((fr.moved && fr.draws3d && fr.matched * 10 >= fr.draws3d * 6 &&
                                    fr.cut * 2 <= fr.matched) ||
                                   always);
    interp_counts_.frames++;
    interp_counts_.held += !blend;
    interp_counts_.draws3d += fr.draws3d;
    interp_counts_.matched += fr.matched;
    interp_counts_.cut += fr.cut;
    interp_counts_.moved += fr.moved;
    interp_counts_.kept += fr.kept;

    uint32_t set = shown_set_;
    shown_set_ = (shown_set_ + 1) % kShownSets;
    StartPresenter();
    {
        std::unique_lock lock(present_lock_);
        present_cv_.wait(lock, [&] { return pending_[set] == 0; });
    }
    CloseList();
    std::vector<Showing> shows;
    if (blend) {
        for (uint32_t j = 1; j < interp_n_; j++) {
            BeginExtra();
            FillArena(j);
            xl_->Close();
            RunList(xl_.Get());
            RunList(cl_.Get());
            BeginExtra();
            CopyShown(shown_[set][j - 1].Get());
            RestoreStates();
            xl_->Close();
            RunList(xl_.Get());
            // the last shown never dropped, the title's own or not
            bool last = !own_shown && j + 1 == interp_n_;
            shows.push_back({shown_[set][j - 1].Get(), when(interp_t_[j - 1]), slot_ms, last, set});
        }
    }
    if (interp_whole_ && interp_slots_) {
        BeginExtra();
        FillArena(0);
        xl_->Close();
        RunList(xl_.Get());
    }
    RunList(cl_.Get());
    // held, the frame itself in the last one's place
    bool own = !blend || own_shown;
    if (own) {
        BeginExtra();
        CopyShown(shown_[set][images - 1].Get());
        xl_->Close();
        RunList(xl_.Get());
    }
    queue_->Signal(fence_.Get(), ++fence_value_);
    f.fence = fence_value_;
    // held, the frame itself in every slot, so the display keeps its pace instead of dropping to the title's rate
    if (!blend)
        for (uint32_t j = 0; j + 1 < images; j++)
            shows.push_back({shown_[set][images - 1].Get(), when(interp_t_[j]), slot_ms, false, set});
    if (own) shows.push_back({shown_[set][images - 1].Get(), when(interp_t_[images - 1]), slot_ms, true, set});
    {
        std::lock_guard lock(present_lock_);
        for (const Showing& s : shows) pq_.push_back(s);
        pending_[set] += uint32_t(shows.size());
    }
    present_cv_.notify_all();
    interp_whole_ = false;
    track_states_ = false;
    InterpLog();
}

// every five seconds while interpolating
void D3D12Backend::InterpLog() {
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - interp_log_t_).count();
    if (el < 5) return;
    InterpCounts& c = interp_counts_;
    double frames = double(std::max<uint64_t>(c.frames, 1));
    LOGR("interp: %.2fx, %.1f title frames and %.1f shown a second (%llu dropped); %llu held, %llu split; %.0f 3D draws "
         "a frame, %.0f%% matched (exact %.0f%%, shape %.0f%%, batch %.0f%%), %.0f%% cut, %.0f%% not taken apart, %.0f moved; "
         "%llu draws past the slots",
         interp_rate_, c.frames / el, shown_count_.exchange(0) / el, (unsigned long long)dropped_count_.exchange(0),
         (unsigned long long)c.held, (unsigned long long)c.split, c.draws3d / frames,
         c.draws3d ? 100.0 * c.matched / c.draws3d : 0.0, c.matched ? 100.0 * c.tiers[0] / c.matched : 0.0,
         c.matched ? 100.0 * c.tiers[1] / c.matched : 0.0, c.matched ? 100.0 * c.tiers[2] / c.matched : 0.0,
         c.matched ? 100.0 * c.cut / c.matched : 0.0, c.matched ? 100.0 * c.kept / c.matched : 0.0,
         c.moved / frames, (unsigned long long)c.full);
    uint64_t shown = std::max<uint64_t>(shown_total_.load() - interp_log_shown_, 1);
    interp_log_shown_ = shown_total_.load();
    LOGR("interp: %.1f ms waited for the display's room a frame shown", wait_us_.exchange(0) / 1000.0 / shown);
    if (c.skinned) {
        double n = double(c.skinned);
        LOGR("interp: %.0f skinned draws a frame: exact %.0f%%, shape %.0f%%, batch %.0f%%, new %.0f%%; most bones jumped in "
             "%.0f%%, paired by name %.0f%%",
             n / frames, 100 * c.skinned_tiers[0] / n, 100 * c.skinned_tiers[1] / n, 100 * c.skinned_tiers[2] / n,
             100 * c.skinned_tiers[interp::kTiers] / n, 100 * c.bones_cut / n, 100 * c.by_name / n);
    }
    if (uint64_t all = c.paired[0] + c.paired[1] + c.paired[2] + c.paired[3] + c.paired[4]) {
        double n = double(all);
        LOGR("interp: matrices paired by name %.1f%%, by a draw with the same one %.1f%%, by an exact match %.1f%%, "
             "by a guess %.1f%%, not at all %.1f%%",
             100 * c.paired[0] / n, 100 * c.paired[1] / n, 100 * c.paired[2] / n, 100 * c.paired[3] / n,
             100 * c.paired[4] / n);
    }
    LOGR("interp: %.0f named matrices a frame, %.0f%% of them found in the frame before", c.named / frames,
         c.named ? 100.0 * c.named_found / c.named : 0.0);
    c = {};
    interp_log_t_ = now;
}

void D3D12Backend::PresentSwap() {
    bool vsync = app::Get().vsync;
    HRESULT hr = swap_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(hr)) LOG_FIRST(4, "d3d12: present failed %08lX, device %08lX", hr, dev_->GetDeviceRemovedReason());
}

void D3D12Backend::StartPresenter() {
    if (presenter_.joinable()) return;
    for (auto& a : palloc_) dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, palloc_[0].Get(), nullptr, IID_PPV_ARGS(&pl_));
    pl_->Close();
    dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pfence_));
    pevent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    presenter_quit_ = false;
    presenter_ = std::thread([this] { PresenterLoop(); });
}

void D3D12Backend::StopPresenter() {
    if (!presenter_.joinable()) return;
    {
        std::lock_guard lock(present_lock_);
        presenter_quit_ = true;
    }
    present_cv_.notify_all();
    presenter_.join();
}

// every image handed over shown and its copy done on the GPU
void D3D12Backend::DrainPresenter() {
    if (!presenter_.joinable()) return;
    std::unique_lock lock(present_lock_);
    present_cv_.wait(lock, [&] { return pq_.empty() && !presenter_busy_; });
    uint64_t done = pfence_value_;
    lock.unlock();
    if (pfence_->GetCompletedValue() < done) {
        pfence_->SetEventOnCompletion(done, pevent_);
        WaitForSingleObject(pevent_, INFINITE);
    }
}

// the image copied into the back buffer and presented
void D3D12Backend::ShowImage(ID3D12Resource* img) {
    std::lock_guard lock(swap_lock_);
    uint32_t i = palloc_next_++ % kPresenterLists;
    if (pfence_->GetCompletedValue() < palloc_fence_[i]) {
        pfence_->SetEventOnCompletion(palloc_fence_[i], pevent_);
        WaitForSingleObject(pevent_, INFINITE);
    }
    palloc_[i]->Reset();
    pl_->Reset(palloc_[i].Get(), nullptr);
    ID3D12Resource* bb = bb_[swap_->GetCurrentBackBufferIndex()].Get();
    D3D12_RESOURCE_BARRIER b = StateBarrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    pl_->ResourceBarrier(1, &b);
    pl_->CopyResource(bb, img);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    pl_->ResourceBarrier(1, &b);
    pl_->Close();
    ID3D12CommandList* lists[] = {pl_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    PresentSwap();
    {
        std::lock_guard plock(present_lock_);
        queue_->Signal(pfence_.Get(), ++pfence_value_);
        palloc_fence_[i] = pfence_value_;
    }
    shown_count_++;
    shown_total_++;
}

// each image at its time: copied into the back buffer and presented; an in-between frame more than half its time late
// is dropped. Under vertical sync the display's own refreshes pace it instead (timed by the clock alone, the images
// drift against refreshes a hair slower or faster, and every few seconds the swap chain's queue filled and a run of
// them went at once; waiting on the output's vertical blank, the present after it often missed the next one)
void D3D12Backend::PresenterLoop() {
    using clock = std::chrono::steady_clock;
    for (;;) {
        if (app::Get().vsync && latency_wait_) {
            // each time the display has room, the oldest image waiting, in order; more than three waiting (the title a
            // hair faster than the display), the oldest dropped. Picking the image due by the clock instead, the
            // title's frames a millisecond early or late made two due at one refresh and none at the next
            {
                std::unique_lock lock(present_lock_);
                present_cv_.wait(lock, [&] { return presenter_quit_ || !pq_.empty(); });
                if (pq_.empty()) return;
            }
            // the room waited for only with an image to fill it: each wait is a present, or the title's own presents
            // (frames shown once) would find the room taken
            WaitLatency();
            Showing s;
            uint32_t freed[kShownSets] = {};
            bool any = false;
            {
                std::lock_guard lock(present_lock_);
                while (pq_.size() > 3) {
                    dropped_count_++;
                    freed[pq_.front().set]++;
                    any = true;
                    pq_.pop_front();
                }
                s = pq_.front();
                pq_.pop_front();
                presenter_busy_ = true;
                for (uint32_t i = 0; i < kShownSets; i++) pending_[i] -= freed[i];
            }
            if (any) present_cv_.notify_all();
            ShowImage(s.img);
            {
                std::lock_guard lock(present_lock_);
                pending_[s.set]--;
                presenter_busy_ = false;
            }
            present_cv_.notify_all();
            continue;
        }
        Showing s;
        {
            std::unique_lock lock(present_lock_);
            present_cv_.wait(lock, [&] { return presenter_quit_ || !pq_.empty(); });
            if (pq_.empty()) return;
            s = pq_.front();
            pq_.pop_front();
            presenter_busy_ = true;
        }
        app::SleepUntil(s.at);
        auto late = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double, std::milli>(s.slot_ms / 2));
        if (!s.real && clock::now() > s.at + late) {
            dropped_count_++;
        } else {
            WaitLatency();
            ShowImage(s.img);
        }
        {
            std::lock_guard lock(present_lock_);
            pending_[s.set]--;
            presenter_busy_ = false;
        }
        present_cv_.notify_all();
    }
}

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<D3D12Backend>(); }

}  // namespace gpu
