cbuffer Params : register(b0)
{
    uint outWidth;
    uint outHeight;
    float t;
    uint invertedDepth;
    float mvScaleX;
    float mvScaleY;
    float mvWidth;
    float mvHeight;
    float depthWidth;
    float depthHeight;
    float staticThreshold;
    uint debugView;
    uint hasHudless;
    float uiThreshold;
    uint pad0;
    uint pad1;
};

Texture2D<float4> PrevColor : register(t0);
Texture2D<float4> CurColor : register(t1);
Texture2D<float4> Velocity : register(t2);
Texture2D<float4> Depth : register(t3);
Texture2D<float4> HudlessPrev : register(t4);
Texture2D<float4> HudlessCur : register(t5);
RWTexture2D<float4> Output : register(u0);
SamplerState Linear : register(s0);

float2 MotionAt(float2 uv)
{
    const int2 hi = int2(depthWidth, depthHeight) - 1;
    const int2 dc = clamp(int2(uv * float2(depthWidth, depthHeight)), int2(0, 0), hi);
    int2 best = dc;
    float bestD = invertedDepth != 0 ? -1.0 : 2.0;

    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const int2 c = clamp(dc + int2(x, y), int2(0, 0), hi);
            const float d = Depth.Load(int3(c, 0)).r;
            const bool nearer = invertedDepth != 0 ? d > bestD : d < bestD;

            if (nearer)
            {
                bestD = d;
                best = c;
            }
        }
    }

    const float2 buv = (float2(best) + 0.5) / float2(depthWidth, depthHeight);
    const int2 mc = clamp(int2(buv * float2(mvWidth, mvHeight)), int2(0, 0), int2(mvWidth, mvHeight) - 1);
    const float2 mv = Velocity.Load(int3(mc, 0)).xy * float2(mvScaleX, mvScaleY);
    return mv / float2(mvWidth, mvHeight);
}

float MaxOf(float3 v) { return max(v.r, max(v.g, v.b)); }

bool Inside(float2 uv) { return all(uv >= 0.0) && all(uv <= 1.0); }

// Unchanged between the two frames, relative to its brightness.
bool StaticAt(int2 c)
{
    const float4 a = PrevColor.Load(int3(c, 0));
    const float4 b = CurColor.Load(int3(c, 0));
    const float scale = max(MaxOf(a.rgb), MaxOf(b.rgb)) + 0.02;
    return MaxOf(abs(b.rgb - a.rgb)) / scale <= staticThreshold * 2.0;
}

// HUD: a colour that stands still where the scene under it moves (more than a pixel between the frames).
bool HudAt(float2 uv)
{
    const int2 c = clamp(int2(uv * float2(outWidth, outHeight)), int2(0, 0), int2(outWidth, outHeight) - 1);
    const float2 sceneMotion = MotionAt(uv) * float2(outWidth, outHeight);
    return dot(sceneMotion, sceneMotion) > 1.0 && StaticAt(c);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outWidth || id.y >= outHeight)
        return;

    const float2 uv = (float2(id.xy) + 0.5) / float2(outWidth, outHeight);
    const float4 prevHere = PrevColor.Load(int3(id.xy, 0));
    const float4 curHere = CurColor.Load(int3(id.xy, 0));

    // HUD-less frames: where this frame differs from its HUD-less copy is HUD, shown as it is now.
    if (hasHudless != 0)
    {
        const float4 sceneHere = HudlessCur.Load(int3(id.xy, 0));
        const float uiScale = max(MaxOf(curHere.rgb), MaxOf(sceneHere.rgb)) + 0.02;

        if (MaxOf(abs(curHere.rgb - sceneHere.rgb)) / uiScale > uiThreshold)
        {
            Output[id.xy] = curHere;
            return;
        }
    }

    const float3 delta = abs(curHere.rgb - prevHere.rgb);
    const float scale = max(MaxOf(curHere.rgb), MaxOf(prevHere.rgb)) + 0.02;

    if (MaxOf(delta) / scale <= staticThreshold)
    {
        Output[id.xy] = curHere;
        return;
    }

    float2 m = MotionAt(uv);

    [unroll] for (int i = 0; i < 3; ++i)
        m = MotionAt(uv - m * (1.0 - t));

    const float2 q = uv - m * (1.0 - t);
    const float2 p = uv + m * t;

    float4 fromCur;
    float4 fromPrev;
    bool curIsHud = false;
    bool prevIsHud = false;

    if (hasHudless != 0)
    {
        // The scene alone: nothing of the HUD can be dragged along.
        fromCur = HudlessCur.SampleLevel(Linear, q, 0);
        fromPrev = HudlessPrev.SampleLevel(Linear, p, 0);
    }
    else
    {
        fromCur = CurColor.SampleLevel(Linear, q, 0);
        fromPrev = PrevColor.SampleLevel(Linear, p, 0);
        curIsHud = HudAt(q);
        prevIsHud = HudAt(p);
    }

    float w = t;

    if (!Inside(p))
        w = 1.0;
    else if (!Inside(q))
        w = 0.0;
    else if (curIsHud && !prevIsHud)
        w = 0.0;
    else if (prevIsHud && !curIsHud)
        w = 1.0;
    else if (curIsHud && prevIsHud)
    {
        // HUD on both sides: the pixel itself, as this frame shows it.
        Output[id.xy] = curHere;
        return;
    }
    else
    {
        const float norm = max(MaxOf(fromCur.rgb), MaxOf(fromPrev.rgb)) + 0.02;
        const float disagree = MaxOf(abs(fromCur.rgb - fromPrev.rgb)) / norm;

        // Screen-anchored: the pixel matches itself in the last frame far better than the scene moved along its
        // motion does -- a HUD, transparent ones included (their colour moves with the scene behind them, so
        // they never look still). Shown as this frame shows it.
        const float anchored = MaxOf(abs(curHere.rgb - prevHere.rgb)) /
                               (max(MaxOf(curHere.rgb), MaxOf(prevHere.rgb)) + 0.02);

        if (hasHudless == 0 && disagree > 0.15 && anchored < disagree * 0.35)
        {
            Output[id.xy] = curHere;
            return;
        }

        // The two frames disagree (one sees what the other does not, or one sample is a piece of HUD fetched
        // from beside the pixel): trust the sample closer to what is really at this pixel now.
        const float toCur = MaxOf(abs(fromCur.rgb - curHere.rgb));
        const float toPrev = MaxOf(abs(fromPrev.rgb - curHere.rgb));
        const float side = toCur <= toPrev ? 1.0 : 0.0;
        w = lerp(t, side, saturate((disagree - 0.1) / 0.2));
    }

    float4 result = lerp(fromPrev, fromCur, w);

    if (debugView != 0)
        result.rgb *= float3(0.6, 1.0, 0.6);

    Output[id.xy] = result;
}
