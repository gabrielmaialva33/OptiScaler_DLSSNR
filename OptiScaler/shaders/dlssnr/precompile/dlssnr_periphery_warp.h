// Peripheral compression's mapping between the frame and the model's packed input, one axis at a time.
// dlssnr/design/peripheral-compression.md.
//
// The radius curve, its inverse and the side selection are ported from PeripheralWarp ABI v2 at
// 64902dd6a02460e5f6b778504ec2a4005faf4d9c, through wilsjo2's external/peripheral_warp/SpatialWarp.h.
// Copyright (c) 2026 Yuri Grib (BeliyG3). MIT licence: see Licenses/PeripheralWarp_LICENSE.txt.
// The logic is theirs line for line; the names and the C/HLSL packaging are this tree's.
//
// One file for the shader (precompile/dlssnr_periphery.hlsl) and the host (DlssNr_Periphery.h, and through
// it tests/nr-periphery), so the test runs the shader's own arithmetic rather than a copy of it. Plain
// C/HLSL subset: no templates, no references, no arrays in the struct (a cbuffer would pad each element to
// 16 bytes), no texture types outside the macros below.
//
// The includer defines, before including this file:
//   PW_FN              function qualifier (nothing in HLSL, `inline` in C++)
//   PW_ABS(v)          |v| for a float
//   PW_FLOOR(v)        floor of a float, as a float
//   PW_CEIL(v)         ceiling of a float, as a float
//   PW_MINF(a, b)      float minimum; PW_MAXF(a, b) float maximum
//   PW_MINI(a, b)      int minimum; PW_MAXI(a, b) int maximum
// and, to get PeripheryAreaAverage (the colour pack's filter), also:
//   PW_COLOUR             the type averaged (float3 in the shader; anything with += and * float)
//   PW_COLOUR_ZERO        its zero
//   PW_LOAD_COLOUR(i, j)  the frame's colour at texel (i, j), already inside the image
// The area section has a guard of its own, so a host that included this file without PW_COLOUR can define
// it and include the file again.

#ifndef DLSSNR_PERIPHERY_WARP_H
#define DLSSNR_PERIPHERY_WARP_H

// One axis of the layout, as the shader reads it. Sixteen floats, so it is four whole cbuffer rows and the
// C++ struct and the HLSL cbuffer agree on every offset. "Neg" is the side before the band's middle (left,
// top), "Pos" the side after it.
struct PeripheryAxis
{
    float bandCenter;     // the frame pixel at the middle of the full-density band
    float workCenter;     // the packed pixel it lands on
    float scale;          // packed pixels per raw work pixel: the global working scale, as model / raw
    float halfSpanNeg;    // frame pixels from the band's middle to the frame's edge, on each side
    float halfSpanPos;
    float centerNeg;      // the band's half-width over that side's half-span: r <= this is unchanged
    float centerPos;
    float workNeg;        // that side's packed extent over its half-span
    float workPos;
    float compressionNeg; // k: the periphery's packed share of its frame share
    float compressionPos;
    float edgeSlopeNeg;   // k squared: the curve's slope at the frame's edge, and the line's beyond it
    float edgeSlopePos;
    float pad0;
    float pad1;
    float pad2;
};

// The packed radius of radius r, both over the side's half-span (1 is the frame's edge). The identity inside
// the band; a rational shoulder whose slope is 1 at the band's edge and k^2 at the frame's; a straight line
// of slope k^2 past the edge, so motion that leaves the frame still has somewhere to land.
PW_FN float PeripheryPackRadius(float r, float center, float work, float compression, float edgeSlope)
{
    r = PW_ABS(r);
    if (work >= 1.0f || center >= 1.0f || r <= center)
        return r;
    if (r > 1.0f)
        return work + (r - 1.0f) * edgeSlope;
    const float t = (r - center) / (1.0f - center);
    return center + (work - center) * t / (compression + (1.0f - compression) * t);
}

// The exact inverse of PeripheryPackRadius.
PW_FN float PeripheryUnpackRadius(float r, float center, float work, float compression, float edgeSlope)
{
    r = PW_ABS(r);
    if (work >= 1.0f || center >= 1.0f || r <= center)
        return r;
    if (r > work)
        return 1.0f + (r - work) / edgeSlope;
    const float yr = (r - center) / (work - center);
    const float t = compression * yr / (1.0f - (1.0f - compression) * yr);
    return center + (1.0f - center) * t;
}

// A frame position (pixels, continuous, centres at +0.5) to the packed position it lands on.
PW_FN float PeripheryPack(float nativePixel, PeripheryAxis a)
{
    const float delta = nativePixel - a.bandCenter;
    const bool positive = delta >= 0.0f;
    const float span = positive ? a.halfSpanPos : a.halfSpanNeg;
    const float packed =
        PeripheryPackRadius(PW_ABS(delta) / span, positive ? a.centerPos : a.centerNeg,
                            positive ? a.workPos : a.workNeg, positive ? a.compressionPos : a.compressionNeg,
                            positive ? a.edgeSlopePos : a.edgeSlopeNeg);
    return a.workCenter + (positive ? 1.0f : -1.0f) * packed * span * a.scale;
}

// A packed position to the frame position it came from.
PW_FN float PeripheryUnpack(float packedPixel, PeripheryAxis a)
{
    const float delta = packedPixel - a.workCenter;
    const bool positive = delta >= 0.0f;
    const float span = positive ? a.halfSpanPos : a.halfSpanNeg;
    const float radius =
        PeripheryUnpackRadius(PW_ABS(delta) / (span * a.scale), positive ? a.centerPos : a.centerNeg,
                              positive ? a.workPos : a.workNeg, positive ? a.compressionPos : a.compressionNeg,
                              positive ? a.edgeSlopePos : a.edgeSlopeNeg);
    return a.bandCenter + (positive ? 1.0f : -1.0f) * radius * span;
}

// A motion vector in frame pixels, at frame position nativePixel, re-expressed in packed pixels by moving both
// of its ends through the mapping. Unchanged inside the band, shorter where the periphery is squeezed, and
// finite for an end past the frame's edge. The vector must already be in FRAME pixels: the game's scale for
// low-resolution vectors is in render pixels (design note, "Two defects").
PW_FN float PeripheryPackMotion(float nativePixel, float nativeMotion, PeripheryAxis a)
{
    return PeripheryPack(nativePixel + nativeMotion, a) - PeripheryPack(nativePixel, a);
}

// The texel of a guide region that frame position nativePixel reads: the position mapped into the region,
// floored. With nothing compressed this is DlssNr::GuideMatch::PointSource, except where the exact answer
// sits on a texel edge and float rounding picks the other side of it.
PW_FN int PeripheryGuideTexel(float nativePixel, float nativeExtent, int regionBase, int regionExtent)
{
    const int texel = (int) PW_FLOOR(nativePixel / nativeExtent * (float) regionExtent);
    return regionBase + PW_MINI(PW_MAXI(texel, 0), regionExtent - 1);
}

#endif // DLSSNR_PERIPHERY_WARP_H

#if defined(PW_COLOUR) && !defined(DLSSNR_PERIPHERY_WARP_AREA_H)
#define DLSSNR_PERIPHERY_WARP_AREA_H

// The widest footprint the colour pack integrates, in frame texels per axis. Compression is at least 0.5 and
// the working scale at least 0.25, so a packed texel covers at most 1 / (0.25 * 0.25) = 16 frame pixels and
// touches 17 texels; this bound is never reached by a valid layout and exists so an invalid constant cannot
// run the loop away.
#define PERIPHERY_MAX_SPAN 32

// The exact area average of the frame over [x0, x1) x [y0, y1) (frame pixels, x0 < x1, y0 < y1): every
// texel weighted by how much of it the rectangle covers, divided by the weights. The downsample's box
// integral (mode 2 of dlssnr.hlsl, hhkbble's) with arbitrary bounds; given a packed texel's warped footprint,
// [Unpack(i), Unpack(i + 1)], it is the colour pack, and with nothing compressed those bounds are mode 2's.
PW_FN PW_COLOUR PeripheryAreaAverage(float x0, float x1, float y0, float y1, int nativeWidth, int nativeHeight)
{
    const int i0 = (int) PW_FLOOR(x0);
    const int i1 = PW_MINI((int) PW_CEIL(x1) - 1, i0 + PERIPHERY_MAX_SPAN - 1);
    const int j0 = (int) PW_FLOOR(y0);
    const int j1 = PW_MINI((int) PW_CEIL(y1) - 1, j0 + PERIPHERY_MAX_SPAN - 1);

    PW_COLOUR acc = PW_COLOUR_ZERO;
    float weights = 0.0f;

    for (int j = j0; j <= j1; ++j)
    {
        const int jj = PW_MINI(PW_MAXI(j, 0), nativeHeight - 1);
        const float wy = PW_MAXF(PW_MINF(y1, (float) j + 1.0f) - PW_MAXF(y0, (float) j), 0.0f);

        for (int i = i0; i <= i1; ++i)
        {
            const int ii = PW_MINI(PW_MAXI(i, 0), nativeWidth - 1);
            const float w = PW_MAXF(PW_MINF(x1, (float) i + 1.0f) - PW_MAXF(x0, (float) i), 0.0f) * wy;
            acc += PW_LOAD_COLOUR(ii, jj) * w;
            weights += w;
        }
    }

    return acc * (1.0f / PW_MAXF(weights, 1e-12f));
}

#endif // DLSSNR_PERIPHERY_WARP_AREA_H
