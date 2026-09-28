// The per-pixel rule of the static-overlay mask, shared by dlssnr_uimask.hlsl and the host test
// tests/nr-uimask-rule, so the test runs the shader's own decision rather than a copy of it. Plain
// C/HLSL subset: no templates, no references, no texture types. dlssnr/design/hud-protection.md has the
// reasoning, including why each gate exists.
//
// The includer defines, before including this file:
//   UM_FN            function qualifier (nothing in HLSL, `inline` in C++)
//   UM_OUT(T)        an out parameter (`out T` in HLSL, `T&` in C++)
//   UM_UNROLL        a loop hint (`[unroll]` in HLSL, nothing in C++)
//   UM_LUMA(x, y)    this frame's luma at (x, y), clamped to the image
//   UM_CHANGE(x, y)  |this frame's luma - last frame's| at (x, y), clamped
//   UM_PROT(x, y)    last frame's protection at (x, y), clamped, 0..1
//   UM_STREAK(x, y)  last frame's candidate streak at (x, y), clamped, whole frames
// and saturate/min/max/abs for floats.

#ifndef DLSSNR_UIMASK_RULE_H
#define DLSSNR_UIMASK_RULE_H

struct UiMaskParams
{
    float staticEps;  // own luma change below which the pixel counts as unchanged
    float coreEps;    // the same, for every pixel within 2 px of it
    float motionTau;  // luma change above which a sample on a side counts as moving
    float detailMin;  // local contrast a pixel needs to be taken for interface
    float decay;      // per-frame decay of the protection once the evidence stops
    float dropTau;    // own luma change above which the protection is dropped at once
    float streakMin;  // consecutive candidate frames before a new pixel is protected
    float supportMin; // protected pixels (last frame) a pixel needs in its 5x5 to be exported
    float sidesMin;   // axis sides (right, left, down, up) whose surroundings must move
};

// One side moves when any of its three samples, at 6, 14 and 24 px along that axis, changed. Axis only:
// a diagonal from a pixel beside a tall silhouette lands on the silhouette, and the sand next to a moving
// coat would then see moving surroundings on three sides.
UM_FN bool UiMaskSideMoving(int x, int y, int dx, int dy, float tau)
{
    return UM_CHANGE(x + dx * 6, y + dy * 6) > tau || UM_CHANGE(x + dx * 14, y + dy * 14) > tau ||
           UM_CHANGE(x + dx * 24, y + dy * 24) > tau;
}

UM_FN void UiMaskPixel(int x, int y, bool valid, UiMaskParams P, UM_OUT(float) prot, UM_OUT(float) streak,
                       UM_OUT(float) mask)
{
    if (!valid)
    {
        prot = 0.0f;
        streak = 0.0f;
        mask = 0.0f;
        return;
    }

    const float cur = UM_LUMA(x, y);
    const float own = UM_CHANGE(x, y);

    // A still core: nothing within 2 px changed. Interface over a moving scene keeps its own pixels still
    // around a glyph's inner edges (fill against outline); scenery beside a moving silhouette does not,
    // because the silhouette is inside that core.
    bool coreStill = true;
    UM_UNROLL for (int cy = -2; cy <= 2; ++cy)
    {
        UM_UNROLL for (int cx = -2; cx <= 2; ++cx)
            coreStill = coreStill && UM_CHANGE(x + cx, y + cy) < P.coreEps;
    }

    // The surroundings move on (nearly) every side, as a scene panning behind an overlay does. Scenery
    // beside something that moves sees it on one side, two at a corner.
    float sides = 0.0f;
    sides += UiMaskSideMoving(x, y, 1, 0, P.motionTau) ? 1.0f : 0.0f;
    sides += UiMaskSideMoving(x, y, -1, 0, P.motionTau) ? 1.0f : 0.0f;
    sides += UiMaskSideMoving(x, y, 0, 1, P.motionTau) ? 1.0f : 0.0f;
    sides += UiMaskSideMoving(x, y, 0, -1, P.motionTau) ? 1.0f : 0.0f;

    const float detail = max(max(abs(cur - UM_LUMA(x + 1, y)), abs(cur - UM_LUMA(x - 1, y))),
                             max(abs(cur - UM_LUMA(x, y + 1)), abs(cur - UM_LUMA(x, y - 1))));

    const bool candidate = own < P.staticEps && coreStill && sides >= P.sidesMin && detail > P.detailMin;

    // Entry hysteresis: a pixel must be a candidate for streakMin frames in a row before it is protected.
    // An already protected one re-arms at once, so a pause in the scene's motion does not restart it.
    const float prevProt = UM_PROT(x, y);
    const float newStreak = candidate ? min(UM_STREAK(x, y) + 1.0f, 255.0f) : 0.0f;

    float p = prevProt * P.decay;
    if (own > P.dropTau)
        p = 0.0f; // the pixel itself changed a lot: whatever was protected here is gone
    if (candidate && (newStreak >= P.streakMin || prevProt > 0.5f))
        p = 1.0f;

    prot = p;
    streak = newStreak;

    // Export: grow one pixel over glyph edges from last frame's protection (so the growth never feeds
    // back), and only where last frame's protection has support -- at least supportMin protected pixels
    // in the 5x5. An isolated protected pixel (a grain of scenery that passed every gate) is dropped; a
    // glyph's edge, a line of them, is kept.
    float grown = 0.0f;
    float support = 0.0f;
    UM_UNROLL for (int sy = -2; sy <= 2; ++sy)
    {
        UM_UNROLL for (int sx = -2; sx <= 2; ++sx)
        {
            const float q = UM_PROT(x + sx, y + sy);
            support += q > 0.5f ? 1.0f : 0.0f;
            if (sx >= -1 && sx <= 1 && sy >= -1 && sy <= 1)
                grown = max(grown, q);
        }
    }

    float m = saturate((max(p, grown * P.decay) - 0.25f) * 2.0f);
    if (support < P.supportMin || own > P.dropTau)
        m = 0.0f;

    mask = m;
}

#endif
