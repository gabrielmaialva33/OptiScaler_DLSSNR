// The per-pixel rules of DLSS-NR's model cadence: how the edit the model made on its last frame is moved onto
// the frames in between, and what stops it landing on the wrong surface. dlssnr/design/model-cadence.md has
// the reasoning. Two things run this file: the cadence pass (dlssnr_cadence.hlsl beside it) and the host test
// tests/nr-cadence, so the test runs the shader's own decisions rather than a copy of them.
//
// The depth mismatch, the colour gate, the validated chain step, the 9-tap acceptance, the ring fill with its
// "no match, no fill" rule, the fill's luma ratio and chroma, the clamped Catmull-Rom read and the off-picture
// vectors for the model are taken from BeliyG3's optimizer-fps-dlss5 (core/shaders/temporal_mapping.hlsli,
// temporal_reproject.hlsli, temporal_residual.hlsli, commit 3636e6d8; MIT, Copyright (c) 2026 Yuri Grib
// (BeliyG3); Licenses/OptimizerFps_LICENSE.txt). The edge ramp that spares still pixels, the scaling with the
// working size and the box-averaged coarse residual are not theirs.
//
// Plain C/HLSL subset: no templates, no references, no texture types, no swizzles beyond .x .y .z. Every
// position is in working pixels with pixel centres at +0.5; a vector points from this frame back to an earlier
// one (q = p + v), as the game's DLSS vectors do.
//
// The includer defines, before including this file:
//   CR_FN                 function qualifier (nothing in HLSL, `inline` in C++)
//   CR_UNROLL, CR_LOOP    loop hints (`[unroll]`/`[loop]` in HLSL, nothing in C++)
//   CR_DEPTH_NOW(tx, ty)  this frame's depth at a working texel, clamped to the image        -> float
//   CR_DEPTH_THEN(tx, ty) the depth snapshot of the last model frame                         -> float
//   CR_PROXY_NOW(tx, ty)  this frame's proxy (what the model would be shown)                 -> float3
//   CR_PROXY_THEN(tx, ty) the proxy snapshot of the last model frame                         -> float3
//   CR_MOTION(tx, ty)     this frame's motion in working pixels, this frame -> the last one  -> float2
//   CR_CHAIN_PREV(tx, ty) the chain as of the last carried frame                             -> float2
//   CR_RESIDUAL(tx, ty)   the stored edit: the model's answer - its proxy                    -> float3
//   CR_RESIDUAL_LOW(tx, ty) the coarse edit, at a coarse texel                               -> float3
//   CR_UI(tx, ty)         this frame's UI alpha, 0..1 (read only when haveUi is set)         -> float
// In C++ also float2, float3 (members x, y, z; arithmetic with each other and with float) and dot, abs, min,
// max, clamp, floor, frac, saturate, smoothstep, cos, sin and isfinite for floats. Name macro parameters
// anything but x and y (see static_overlay_rule.h for why).

#ifndef DLSSNR_CADENCE_RULE_H
#define DLSSNR_CADENCE_RULE_H

struct CadenceParams
{
    int width; // the model's working size
    int height;
    int lowWidth; // the coarse edit's size
    int lowHeight;
    int lowBlock;     // working pixels per coarse texel, per axis
    int haveUi;       // 1 when CR_UI is this frame's UI alpha
    float depthTol;   // relative depth mismatch at which a sample starts to be rejected
    float colourTol;  // the colour gate's tolerance, display-referred
    float scale;      // working size / frame size, clamped: BeliyG3's taps and rings are native pixels
    float depthStepX; // the depth snapshot's guide texel spacing, in working pixels (>= 1)
    float depthStepY;
};

CR_FN int CadenceTexel(float v, int extent) { return clamp((int) floor(v), 0, extent - 1); }

CR_FN float CadenceLuma(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

CR_FN float3 CadenceMin3(float3 a, float3 b) { return float3(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)); }

CR_FN float3 CadenceMax3(float3 a, float3 b) { return float3(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)); }

CR_FN bool CadenceFinite2(float2 v) { return isfinite(v.x) && isfinite(v.y); }

CR_FN bool CadenceFinite3(float3 v) { return isfinite(v.x) && isfinite(v.y) && isfinite(v.z); }

// How far apart two depth samples are, as a fraction, whatever the buffer's encoding (BeliyG3,
// PwTDepthMismatch). The plain relative difference reads distance ratios from a linear or reversed-Z buffer;
// a conventional 0..1 buffer keeps everything past a few metres within a few percent of 1, so there the
// ratio is read from 1 - d as well and the larger one counts.
CR_FN float CadenceDepthMismatch(float a, float b)
{
    if (!isfinite(a) || !isfinite(b))
        return 1e9f;
    float mismatch = abs(a - b) / max(max(abs(a), abs(b)), 1e-6f);
    if (a >= 0.0f && a <= 1.0f && b >= 0.0f && b <= 1.0f)
        mismatch = max(mismatch, abs(a - b) / max(max(1.0f - a, 1.0f - b), 1e-6f));
    return mismatch;
}

// Against the snapshot around q: the best of its four texels at the guide's spacing (BeliyG3,
// PwTDepthMismatchAround). Render-resolution guides are jittered, so a texel on an edge belongs to one
// surface one frame and the other the next; if any of the four shows this surface, the last model frame had
// it here.
CR_FN float CadenceDepthMismatchAround(float depthNow, float qx, float qy, CadenceParams P)
{
    if (!isfinite(depthNow))
        return 1e9f;
    const int x0 = CadenceTexel(qx - 0.5f * P.depthStepX, P.width);
    const int x1 = CadenceTexel(qx + 0.5f * P.depthStepX, P.width);
    const int y0 = CadenceTexel(qy - 0.5f * P.depthStepY, P.height);
    const int y1 = CadenceTexel(qy + 0.5f * P.depthStepY, P.height);
    float best = CadenceDepthMismatch(depthNow, CR_DEPTH_THEN(x0, y0));
    best = min(best, CadenceDepthMismatch(depthNow, CR_DEPTH_THEN(x1, y0)));
    best = min(best, CadenceDepthMismatch(depthNow, CR_DEPTH_THEN(x0, y1)));
    best = min(best, CadenceDepthMismatch(depthNow, CR_DEPTH_THEN(x1, y1)));
    return best;
}

// Whether two proxy colours show the same thing, 1 (same) .. 0 (something else) (BeliyG3, PwTColourGate).
// Chromaticity first -- a change of lighting moves a surface's luma but hardly its hue, a disocclusion moves
// the hue -- then luma at twice the tolerance, for same-hue disocclusions and moving shadows. The 0.06 floor
// keeps a dark pixel's noise from swinging its normalised colour.
CR_FN float CadenceColourGate(float3 c, float3 cf, float tol)
{
    const float3 nc = c / (c.x + c.y + c.z + 0.06f);
    const float3 nf = cf / (cf.x + cf.y + cf.z + 0.06f);
    const float chroma = abs(nc.x - nf.x) + abs(nc.y - nf.y) + abs(nc.z - nf.z);
    const float lc = CadenceLuma(c);
    const float lf = CadenceLuma(cf);
    const float relL = abs(lc - lf) / (max(lc, lf) + 0.02f);
    return (1.0f - smoothstep(0.75f * tol, 1.5f * tol, chroma)) * (1.0f - smoothstep(2.0f * tol, 4.0f * tol, relL));
}

// The chain at a position, bilinear, held inside the image.
CR_FN float2 CadenceChainAt(float px_, float py_, CadenceParams P)
{
    const float cx = clamp(px_, 0.5f, (float) P.width - 0.5f) - 0.5f;
    const float cy = clamp(py_, 0.5f, (float) P.height - 0.5f) - 0.5f;
    const int bx = (int) floor(cx);
    const int by = (int) floor(cy);
    const float fx = cx - (float) bx;
    const float fy = cy - (float) by;
    const int bx1 = min(bx + 1, P.width - 1);
    const int by1 = min(by + 1, P.height - 1);
    const float2 top = CR_CHAIN_PREV(bx, by) * (1.0f - fx) + CR_CHAIN_PREV(bx1, by) * fx;
    const float2 bottom = CR_CHAIN_PREV(bx, by1) * (1.0f - fx) + CR_CHAIN_PREV(bx1, by1) * fx;
    return top * (1.0f - fy) + bottom * fy;
}

// One link of the chain back to the last model frame (BeliyG3, PSAccumulate): this frame's vector plus the
// chain where it points. The first carried frame after a model frame has no chain yet and starts from its own
// vector. The link is validated: the chain is inherited through occluders -- a pixel a lip just uncovered
// takes over the lip's displacement and lands on the lip -- so if the last model frame does not show this
// pixel's surface at the chain's end, the chain restarts from this frame's vector and the reprojection then
// rejects it honestly.
CR_FN float2 CadenceChainStep(int px, int py, bool chainStart, CadenceParams P)
{
    const float cx = (float) px + 0.5f;
    const float cy = (float) py + 0.5f;
    float2 mv = CR_MOTION(px, py);
    if (!CadenceFinite2(mv))
        mv = float2(0.0f, 0.0f); // a NaN vector would poison the chain until the next model frame
    float2 acc = mv;
    if (!chainStart)
    {
        acc = mv + CadenceChainAt(cx + mv.x, cy + mv.y, P);
        if (!CadenceFinite2(acc))
            acc = mv;

        const float qx = cx + acc.x;
        const float qy = cy + acc.y;
        if (qx >= 0.0f && qy >= 0.0f && qx < (float) P.width && qy < (float) P.height)
        {
            bool ok = CadenceDepthMismatchAround(CR_DEPTH_NOW(px, py), qx, qy, P) <= 2.0f * P.depthTol;
            if (ok)
                ok = CadenceColourGate(CR_PROXY_NOW(px, py),
                                       CR_PROXY_THEN(CadenceTexel(qx, P.width), CadenceTexel(qy, P.height)),
                                       P.colourTol) > 0.05f;
            if (!ok)
                acc = mv;
        }
    }
    return acc;
}

// The vector handed to the model on the model frame that follows carried frames (BeliyG3, PSModelMotion).
// The model's own history is as old as the last model frame and it finds it through these vectors; where the
// chain does not end on this pixel's own surface, the vector leaves the picture instead, so the model treats
// the pixel as newly uncovered rather than blending in whatever stood there.
CR_FN float2 CadenceModelMotion(int px, int py, float2 acc, CadenceParams P)
{
    const float2 none = float2(4.0f * (float) P.width, 0.0f);
    if (!CadenceFinite2(acc))
        return none;
    const float qx = (float) px + 0.5f + acc.x;
    const float qy = (float) py + 0.5f + acc.y;
    if (qx < 0.0f || qy < 0.0f || qx >= (float) P.width || qy >= (float) P.height)
        return none;
    if (CadenceDepthMismatchAround(CR_DEPTH_NOW(px, py), qx, qy, P) > 2.0f * P.depthTol)
        return none;
    if (CadenceColourGate(CR_PROXY_NOW(px, py), CR_PROXY_THEN(CadenceTexel(qx, P.width), CadenceTexel(qy, P.height)),
                          P.colourTol) <= 0.05f)
        return none;
    return acc;
}

// The edit at a position: Catmull-Rom over 4x4 texels, clamped to the four texels the position lies between,
// so a step in the edit cannot ring (BeliyG3, PwTSampleCatmullRom, with all sixteen taps). At a texel centre
// it is that texel, exactly.
CR_FN float3 CadenceResidualCatmullRom(float px_, float py_, CadenceParams P)
{
    const float tx = clamp(px_, 0.5f, (float) P.width - 0.5f) - 0.5f;
    const float ty = clamp(py_, 0.5f, (float) P.height - 0.5f) - 0.5f;
    const int bx = (int) floor(tx);
    const int by = (int) floor(ty);
    const float fx = tx - (float) bx;
    const float fy = ty - (float) by;

    float wx[4];
    float wy[4];
    wx[0] = fx * (-0.5f + fx * (1.0f - 0.5f * fx));
    wx[1] = 1.0f + fx * fx * (-2.5f + 1.5f * fx);
    wx[2] = fx * (0.5f + fx * (2.0f - 1.5f * fx));
    wx[3] = fx * fx * (-0.5f + 0.5f * fx);
    wy[0] = fy * (-0.5f + fy * (1.0f - 0.5f * fy));
    wy[1] = 1.0f + fy * fy * (-2.5f + 1.5f * fy);
    wy[2] = fy * (0.5f + fy * (2.0f - 1.5f * fy));
    wy[3] = fy * fy * (-0.5f + 0.5f * fy);

    float3 sum = float3(0.0f, 0.0f, 0.0f);
    CR_UNROLL for (int j = 0; j < 4; ++j)
    {
        const int sy = clamp(by - 1 + j, 0, P.height - 1);
        CR_UNROLL for (int i = 0; i < 4; ++i)
        {
            const int sx = clamp(bx - 1 + i, 0, P.width - 1);
            sum = sum + CR_RESIDUAL(sx, sy) * (wx[i] * wy[j]);
        }
    }

    const int x1 = min(bx + 1, P.width - 1);
    const int y1 = min(by + 1, P.height - 1);
    const float3 a = CR_RESIDUAL(bx, by);
    const float3 b = CR_RESIDUAL(x1, by);
    const float3 c = CR_RESIDUAL(bx, y1);
    const float3 d = CR_RESIDUAL(x1, y1);
    const float3 lo = CadenceMin3(CadenceMin3(a, b), CadenceMin3(c, d));
    const float3 hi = CadenceMax3(CadenceMax3(a, b), CadenceMax3(c, d));
    return CadenceMin3(CadenceMax3(sum, lo), hi);
}

// The edit at a position, bilinear.
CR_FN float3 CadenceResidualBilinear(float px_, float py_, CadenceParams P)
{
    const float tx = clamp(px_, 0.5f, (float) P.width - 0.5f) - 0.5f;
    const float ty = clamp(py_, 0.5f, (float) P.height - 0.5f) - 0.5f;
    const int bx = (int) floor(tx);
    const int by = (int) floor(ty);
    const float fx = tx - (float) bx;
    const float fy = ty - (float) by;
    const int x1 = min(bx + 1, P.width - 1);
    const int y1 = min(by + 1, P.height - 1);
    const float3 top = CR_RESIDUAL(bx, by) * (1.0f - fx) + CR_RESIDUAL(x1, by) * fx;
    const float3 bottom = CR_RESIDUAL(bx, y1) * (1.0f - fx) + CR_RESIDUAL(x1, y1) * fx;
    return top * (1.0f - fy) + bottom * fy;
}

// The coarse edit at a working position, bilinear between coarse texels.
CR_FN float3 CadenceLowAt(float px_, float py_, CadenceParams P)
{
    const float block = (float) P.lowBlock;
    const float tx = clamp(px_ / block, 0.5f, (float) P.lowWidth - 0.5f) - 0.5f;
    const float ty = clamp(py_ / block, 0.5f, (float) P.lowHeight - 0.5f) - 0.5f;
    const int bx = (int) floor(tx);
    const int by = (int) floor(ty);
    const float fx = tx - (float) bx;
    const float fy = ty - (float) by;
    const int x1 = min(bx + 1, P.lowWidth - 1);
    const int y1 = min(by + 1, P.lowHeight - 1);
    const float3 top = CR_RESIDUAL_LOW(bx, by) * (1.0f - fx) + CR_RESIDUAL_LOW(x1, by) * fx;
    const float3 bottom = CR_RESIDUAL_LOW(bx, y1) * (1.0f - fx) + CR_RESIDUAL_LOW(x1, y1) * fx;
    return top * (1.0f - fy) + bottom * fy;
}

// What a model frame stores: the answer minus the proxy it was given. A NaN from the model stores nothing.
CR_FN float3 CadenceResidual(float3 answer, float3 proxy)
{
    const float3 r = answer - proxy;
    return CadenceFinite3(r) ? r : float3(0.0f, 0.0f, 0.0f);
}

// One coarse texel: the plain mean of the edit over its block (BeliyG3 took sixteen bilinear taps).
CR_FN float3 CadenceCoarse(int lx, int ly, CadenceParams P)
{
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float n = 0.0f;
    const int x0 = lx * P.lowBlock;
    const int y0 = ly * P.lowBlock;
    CR_LOOP for (int j = 0; j < P.lowBlock; ++j)
    {
        CR_LOOP for (int i = 0; i < P.lowBlock; ++i)
        {
            const int sx = x0 + i;
            const int sy = y0 + j;
            if (sx < P.width && sy < P.height)
            {
                sum = sum + CR_RESIDUAL(sx, sy);
                n += 1.0f;
            }
        }
    }
    return n > 0.0f ? sum / n : float3(0.0f, 0.0f, 0.0f);
}

// A carried frame's answer at one pixel: the proxy now plus the last edit moved here, where the last model
// frame showed this surface, and a fill where it did not (BeliyG3, PwTReproject, without the older passes,
// the expected depth and the silhouette rules). acc is this frame's chain at the pixel.
//
// out = proxy + (1 - ui) * (accept * edit(q) + (1 - accept) * fill), clamped to [0, max(1, proxy)].
CR_FN float3 CadenceSynthesize(int px, int py, float2 acc, CadenceParams P)
{
    const float W = (float) P.width;
    const float H = (float) P.height;
    const float cx = (float) px + 0.5f;
    const float cy = (float) py + 0.5f;
    const float3 c = CR_PROXY_NOW(px, py);
    const float dc = CR_DEPTH_NOW(px, py);

    float2 d = acc;
    if (!CadenceFinite2(d))
        d = float2(0.0f, 0.0f);
    const float qx = cx + d.x;
    const float qy = cy + d.y;

    // Where the chain left the picture there is nothing to carry; near the edge the decision ramps, but only
    // where the chain moved q nearer the edge than the pixel is -- a still pixel by the border is carried
    // whole (BeliyG3 ramped every pixel within 16 px of the edge).
    const bool inside = qx >= 0.0f && qy >= 0.0f && qx < W && qy < H;
    const float distQ = min(min(qx, W - qx), min(qy, H - qy));
    const float distP = min(min(cx, W - cx), min(cy, H - cy));
    const float ramp = 16.0f * P.scale;
    const float wEdge = inside ? saturate(distQ / max(min(ramp, distP), 1e-3f)) : 0.0f;

    // Acceptance at the pixel and eight neighbours 3 px out, averaged, so a per-pixel decision does not draw
    // a line that jitters from frame to frame; then gated by the centre's own result, because the
    // neighbourhood softens a decision and must not overturn one (BeliyG3, 9-tap acceptance).
    float tapX[9] = { 0.0f, 3.0f, -3.0f, 0.0f, 0.0f, 3.0f, -3.0f, 3.0f, -3.0f };
    float tapY[9] = { 0.0f, 0.0f, 0.0f, 3.0f, -3.0f, 3.0f, 3.0f, -3.0f, -3.0f };
    float tapW[9] = { 0.28f, 0.12f, 0.12f, 0.12f, 0.12f, 0.06f, 0.06f, 0.06f, 0.06f };
    float w = 0.0f;
    float wTotal = 0.0f;
    float wCentre = 0.0f;
    CR_UNROLL for (int k = 0; k < 9; ++k)
    {
        const float ox = tapX[k] * P.scale;
        const float oy = tapY[k] * P.scale;
        const int pkx = CadenceTexel(clamp(cx + ox, 0.0f, W - 1.0f), P.width);
        const int pky = CadenceTexel(clamp(cy + oy, 0.0f, H - 1.0f), P.height);
        const float qkx = clamp(qx + ox, 0.0f, W - 1.0f);
        const float qky = clamp(qy + oy, 0.0f, H - 1.0f);
        float wk = 1.0f - smoothstep(P.depthTol, 2.0f * P.depthTol,
                                     CadenceDepthMismatchAround(CR_DEPTH_NOW(pkx, pky), qkx, qky, P));
        wk *= CadenceColourGate(CR_PROXY_NOW(pkx, pky),
                                CR_PROXY_THEN(CadenceTexel(qkx, P.width), CadenceTexel(qky, P.height)), P.colourTol);
        w += wk * tapW[k];
        wTotal += tapW[k];
        if (k == 0)
            wCentre = wk;
    }
    w = w / wTotal; // exactly 1 when every tap passes
    w *= saturate(2.0f * wCentre);
    w *= wEdge;

    const float qcx = clamp(qx, 0.5f, W - 0.5f);
    const float qcy = clamp(qy, 0.5f, H - 0.5f);
    float3 edit = CadenceResidualCatmullRom(qcx, qcy, P);

    // The fill: the model's local tone where the vectors point, scaled by the brightness now over then -- the
    // correction scales with what it corrected (BeliyG3: teeth uncovered by a lip came out 10 % too pale).
    const float lumaNow = max(CadenceLuma(c), 1e-3f);
    const float lumaThen =
        max(CadenceLuma(CR_PROXY_THEN(CadenceTexel(qcx, P.width), CadenceTexel(qcy, P.height))), 1e-3f);
    float3 fill = CadenceLowAt(qcx, qcy, P) * clamp(lumaNow / lumaThen, 0.25f, 4.0f);

    if (w < 0.5f)
    {
        // A disocclusion: at q the last model frame showed the occluder. Look around q for points whose depth
        // then matches this pixel's depth now -- the same surface -- in four rings of eight, turned per pixel
        // so the occluder's outline becomes fine noise rather than a sawtooth; weighted by the depth match and
        // a looser colour gate.
        float3 sum = float3(0.0f, 0.0f, 0.0f);
        float n = 0.0f;
        float matched = 0.0f;
        const float turn = 0.785398f * frac(52.9829189f * frac(0.06711056f * cx + 0.00583715f * cy));
        CR_UNROLL for (int ring = 1; ring <= 4; ++ring)
        {
            const float radius = 12.0f * (float) (1 << ring) * P.scale;
            CR_UNROLL for (int t = 0; t < 8; ++t)
            {
                const float angle = (float) t * 0.785398f + ((ring & 1) == 0 ? 0.392699f : 0.0f) + turn;
                const float rx = clamp(qcx + cos(angle) * radius, 0.5f, W - 0.5f);
                const float ry = clamp(qcy + sin(angle) * radius, 0.5f, H - 0.5f);
                const int tx = CadenceTexel(rx, P.width);
                const int ty = CadenceTexel(ry, P.height);
                float wk =
                    1.0f - smoothstep(P.depthTol, 2.0f * P.depthTol, CadenceDepthMismatch(dc, CR_DEPTH_THEN(tx, ty)));
                const float3 cfk = CR_PROXY_THEN(tx, ty);
                const float gate = CadenceColourGate(c, cfk, 2.0f * P.colourTol);
                wk *= gate;
                matched += wk;
                const float ratio = clamp(lumaNow / max(CadenceLuma(cfk), 1e-3f), 0.25f, 4.0f);
                // Where the colour matches, that spot's own edit rather than the block's mean.
                const float3 rk = CadenceLowAt(rx, ry, P) * (1.0f - gate) + CadenceResidualBilinear(rx, ry, P) * gate;
                sum = sum + rk * (ratio * wk);
                n += wk;
            }
        }
        fill = n > 1e-3f ? sum / n : float3(0.0f, 0.0f, 0.0f);

        // No match, no fill: nothing around shows this surface, so whatever the rings found is the occluder's
        // own edit, and on the floor behind a dark jacket it drew the jacket as a pale silhouette. The pixel
        // shows the game's frame until the model looks again.
        fill = fill * saturate(matched / 0.15f);
    }

    // The fill carries the colour of whatever stood around q; keep its luma and apply it along this pixel's
    // own chroma.
    const float lf = CadenceLuma(fill);
    fill = c * (lf / lumaNow);

    // A NaN in the edit or the fill would survive a zero weight: never let it reach the frame.
    if (!CadenceFinite3(edit))
        edit = float3(0.0f, 0.0f, 0.0f);
    if (!CadenceFinite3(fill))
        fill = float3(0.0f, 0.0f, 0.0f);

    float3 add = fill * (1.0f - w) + edit * w;

    // UI protection: a pixel taken for interface keeps the frame as it is, as the model leaves it on a model
    // frame, rather than receiving scenery's carried edit.
    if (P.haveUi != 0)
        add = add * (1.0f - saturate(CR_UI(px, py)));

    const float3 o = c + add;
    return float3(clamp(o.x, 0.0f, max(1.0f, c.x)), clamp(o.y, 0.0f, max(1.0f, c.y)), clamp(o.z, 0.0f, max(1.0f, c.z)));
}

#endif
