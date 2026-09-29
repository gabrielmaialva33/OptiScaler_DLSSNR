// The UI layer's own mask, for synthesized frame generation: which pixels the layer FFX composes over generated
// frames takes from the real frame. A superset of the strict static-overlay mask (static_overlay_rule.h beside this
// file), which stays the depth FSR-FG is handed. Frame generation only: DLSS-NR's pass does not include this file,
// so nothing here can move its bytecode. Design: dlssnr/design/synthesized-frame-generation.md, "The HUD layer's own
// mask: recall and a margin". Run by synth_overlay_detect.hlsl (its SYNTH_OVERLAY_LAYER permutation), by
// synth_overlay_layer.hlsl (the band's weight only), and on the CPU by tests/fg-synth-policy.
//
// Three steps:
//  1. Seeds. A pixel still against its anchor for stillFrames frames in a row, with contrast, while last frame's
//     tile map shows the scene moving on at least sidesMin of the four axis sides within sideTiles tiles, two
//     perpendicular orientations of static structure in the 5x5 tiles around, and at least cameraShare of the
//     sampled textured tiles moving. A seed holds for holdFrames frames while it stays still, and is gone the moment
//     it changes. No still core: that is what kept 1 px outlines and thin elements out of the strict mask.
//  2. Growth. The distance from the nearest seed through pixels still for stillFrames frames, advanced one 5x5 step
//     (2 px) a frame; a pixel within reach px is in the layer's core, with the strict mask.
//  3. The band, in synth_overlay_layer.hlsl: the core dilated by margin / 2 px and feathered to margin + 1 px.
//
// The tile map: one texel per 8x8 thread group of the detect pass, written by that group each frame and read by
// the next frame's groups, so every tile test here is a frame late. It holds counts, exact in 8 bits: the tile's
// pixels that moved, those that are textured, the orientation bits of its steady structure with UL_TILE_CORE, and
// its pixels inside the image. A share is compared as count >= share * pixels, which is exact too, so the CPU and
// the GPU agree on a tile at exactly the threshold (they did not, with shares stored rounded).
//
// Plain C/HLSL subset, as static_overlay_rule.h: no templates, references, vector or texture types. saturate,
// min, max and abs come from the includer. Every includer defines UL_FN (nothing in HLSL, `inline` in C++). The
// seed and growth functions are compiled only when UL_TILE_MOVED is defined, and then these must be too:
//   UL_OUT(T)                 an out parameter (`out T` in HLSL, `T&` in C++)
//   UL_UNROLL                 a loop hint (`[unroll]` in HLSL, nothing in C++)
//   UL_TILE_MOVED(qx, qy)     last frame's tile map at tile (qx, qy), clamped to the map: its pixels that moved
//   UL_TILE_TEXTURED(qx, qy)  the same tile: its pixels that are textured
//   UL_TILE_PIXELS(qx, qy)    the same tile: its pixels inside the image
//   UL_TILE_BITS(qx, qy)      the same tile: its orientation bits and UL_TILE_CORE, an int 0..31
//   UL_DIST_PREV(qx, qy)      last frame's grown distance at pixel (qx, qy), whole pixels; 255, none, outside the
//                             image or without a history
// Name the macros' parameters anything but x and y: a function-like macro substitutes a swizzle too
// (synth_overlay_detect.hlsl has the story).

#ifndef SYNTH_STATIC_OVERLAY_LAYER_H
#define SYNTH_STATIC_OVERLAY_LAYER_H

// Orientation bits (UiLayerOrientation, the tile map's third channel).
#define UL_HORIZONTAL 1
#define UL_VERTICAL 2
#define UL_DIAGONAL_DOWN 4 // through up-left and down-right
#define UL_DIAGONAL_UP 8   // through down-left and up-right
#define UL_ORIENTATIONS 15
// And in the same channel, not an orientation: the tile holds some of this frame's core. The layer pass skips a
// group whose band reaches no such tile.
#define UL_TILE_CORE 16

// The tile map's tile, one thread group of the detect pass, and the camera test's grid of samples over it.
#define UL_TILE 8
#define UL_CAMERA_GRID 16
#define UL_CAMERA_SAMPLES (UL_CAMERA_GRID * UL_CAMERA_GRID)

// The largest band the layer pass supports (synth_overlay_layer.hlsl sizes its group-shared memory for it), and
// the growth's cap, which the state's 8-bit channel carries.
#define UL_MAX_MARGIN 24
#define UL_NO_DISTANCE 255.0f

struct UiLayerParams
{
    float stillEps;      // luma difference from the anchor below which a pixel counts as still
    float stillFrames;   // still frames in a row before a pixel seeds or carries growth
    float holdFrames;    // frames a seed holds, while still, once the evidence stops
    float reach;         // px of growth from a seed
    float detailMin;     // contrast a seed needs, and a structure to have an orientation
    float texturedMin;   // contrast for a pixel to count as textured in the tile map
    float movingTau;     // luma change above which a pixel counts as moving in the tile map
    float movingShare;   // share of a tile's pixels that must move for the tile to count as moving
    float texturedShare; // share of a tile's pixels that must be textured for the tile to count as textured
    float cameraShare;   // share of the sampled textured tiles that must be moving for the camera to move
    int sideTiles;       // the side test's reach, in tiles
    int sidesMin;        // axis sides that must see a moving tile within the reach
};

// The shipped values. The figures shared with the strict rule are its own (SynthOverlay_Dx12.cpp): stillEps is its
// coreEps, detailMin its detailMin, movingTau its motionTau. A tile moves when half its pixels do: a panning scene
// moves nearly all of them, sparse rain or a few leaves do not. The side reach is about a sixth of the frame's
// height (120 px at 720p, 240 px at 1440p), clamped to 8..32 tiles; three sides of four, so a panel wider than the
// reach, which sees motion only above and below and on its near side, still seeds end to end.
UL_FN UiLayerParams UiLayerShipped(int height)
{
    UiLayerParams P;
    P.stillEps = 0.012f;
    P.stillFrames = 8.0f;
    P.holdFrames = 90.0f;
    P.reach = 48.0f;
    P.detailMin = 0.15f;
    P.texturedMin = 0.04f;
    P.movingTau = 0.02f;
    P.movingShare = 0.5f;
    P.texturedShare = 0.25f;
    P.cameraShare = 0.5f;
    P.sideTiles = max(8, min(32, height / 48));
    P.sidesMin = 3;
    return P;
}

// Whether a pixel is steady this frame, from its luma now and last frame's state: still against its anchor for
// stillFrames frames in a row (UiLayerPixel's own test, for the includer to ask of a neighbour).
UL_FN bool UiLayerSteady(bool history, float luma, float prevStill, float prevAnchor, UiLayerParams P)
{
    return history && abs(luma - prevAnchor) < P.stillEps && min(prevStill + 1.0f, 255.0f) >= P.stillFrames;
}

// The steady mask's bits, one per neighbour, in UiLayerOrientation's order.
#define UL_N_LEFT 1
#define UL_N_RIGHT 2
#define UL_N_UP 4
#define UL_N_DOWN 8
#define UL_N_UP_LEFT 16
#define UL_N_UP_RIGHT 32
#define UL_N_DOWN_LEFT 64
#define UL_N_DOWN_RIGHT 128

// A neighbour's difference as evidence that the structure runs through it (along) or crosses it (across). Only a
// steady neighbour is evidence: static structure is still on both sides of its edge. A neighbour that moves gives a
// direction nothing to run along (1, the largest difference) and nothing to differ from (0).
UL_FN float UiLayerAlong(float c, float n, int steady, int bit) { return (steady & bit) != 0 ? abs(c - n) : 1.0f; }
UL_FN float UiLayerAcross(float c, float n, int steady, int bit) { return (steady & bit) != 0 ? abs(c - n) : 0.0f; }

// The orientation of a pixel's static structure from its luma, its eight neighbours' and which of them are steady
// (UL_N_* bits): the one direction it changes least along, when it changes at least twice as much, and by more than
// detailMin, across it. One-sided differences (the larger of the two sides), so a 1 px line counts: a central
// difference across it is zero.
//  - One direction per pixel: beside a straight edge the diagonal neighbours lie in whatever texture borders it, and
//    testing each diagonal on its own gave a straight scenery edge both of them.
//  - Steady neighbours only. The GPU harness's darker content, panning 4 px a frame, showed why (2026-09-29): smooth
//    brick stays within stillEps of its anchor while a mortar joint slides up beside it, and the edge between them
//    read as a vertical structure across a pan whose only still structure is horizontal. Counting a moving
//    neighbour as the pixel's own value instead gave its direction a spurious "along", and mortar both diagonals.
UL_FN int UiLayerOrientation(float c, float l, float r, float u, float d, float ul, float ur, float dl, float dr,
                             int steady, float detailMin)
{
    const float alongH = max(UiLayerAlong(c, l, steady, UL_N_LEFT), UiLayerAlong(c, r, steady, UL_N_RIGHT));
    const float alongV = max(UiLayerAlong(c, u, steady, UL_N_UP), UiLayerAlong(c, d, steady, UL_N_DOWN));
    const float alongDown =
        max(UiLayerAlong(c, ul, steady, UL_N_UP_LEFT), UiLayerAlong(c, dr, steady, UL_N_DOWN_RIGHT));
    const float alongUp = max(UiLayerAlong(c, ur, steady, UL_N_UP_RIGHT), UiLayerAlong(c, dl, steady, UL_N_DOWN_LEFT));

    // The axes first, so a tie goes to them.
    int bit = UL_HORIZONTAL;
    float along = alongH;
    float across = max(UiLayerAcross(c, u, steady, UL_N_UP), UiLayerAcross(c, d, steady, UL_N_DOWN));
    if (alongV < along)
    {
        bit = UL_VERTICAL;
        along = alongV;
        across = max(UiLayerAcross(c, l, steady, UL_N_LEFT), UiLayerAcross(c, r, steady, UL_N_RIGHT));
    }
    if (alongDown < along)
    {
        bit = UL_DIAGONAL_DOWN;
        along = alongDown;
        across = max(UiLayerAcross(c, ur, steady, UL_N_UP_RIGHT), UiLayerAcross(c, dl, steady, UL_N_DOWN_LEFT));
    }
    if (alongUp < along)
    {
        bit = UL_DIAGONAL_UP;
        along = alongUp;
        across = max(UiLayerAcross(c, ul, steady, UL_N_UP_LEFT), UiLayerAcross(c, dr, steady, UL_N_DOWN_RIGHT));
    }
    return (across > detailMin && along < 0.5f * across) ? bit : 0;
}

// Two perpendicular orientations: interface (crosshair arms, glyph strokes, the corners of a frame), where a
// straight scenery edge running along the motion, the one static structure a pan leaves, has a single one.
UL_FN bool UiLayerPerpendicular(int bits)
{
    return (bits & (UL_HORIZONTAL | UL_VERTICAL)) == (UL_HORIZONTAL | UL_VERTICAL) ||
           (bits & (UL_DIAGONAL_DOWN | UL_DIAGONAL_UP)) == (UL_DIAGONAL_DOWN | UL_DIAGONAL_UP);
}

// The band's weight d px from the core along one axis, for a margin of `margin` px: 1 up to margin / 2, then
// linearly down to 0 at margin + 1. The layer's alpha is the largest core * weight(|dx|) * weight(|dy|).
UL_FN float UiLayerBandWeight(int d, int margin)
{
    const int solid = margin / 2;
    if (d <= solid)
        return 1.0f;
    return saturate(float(margin + 1 - d) / float(margin + 1 - solid));
}

// Growth carries a pixel into the layer's core while it is within reach of a seed.
UL_FN bool UiLayerGrown(float dist, UiLayerParams P) { return dist <= P.reach; }

#ifdef UL_TILE_MOVED

// Whether last frame's tile map shows the scene moving on side (dx, dy) of tile (tx, ty): a moving tile at a
// distance first, first + step, ... up to P.sideTiles. The shader spreads the distances over threads; the host
// test takes them all (first 1, step 1). A tile outside the map is not motion: the image border moves nothing.
UL_FN bool UiLayerSideMoving(int tx, int ty, int dx, int dy, int first, int step, int tilesW, int tilesH,
                             UiLayerParams P)
{
    bool moving = false;
    for (int dist = first; dist <= P.sideTiles; dist += step)
    {
        const int qx = tx + dx * dist;
        const int qy = ty + dy * dist;
        const bool inside = qx >= 0 && qy >= 0 && qx < tilesW && qy < tilesH;
        if (inside && UL_TILE_MOVED(qx, qy) >= P.movingShare * UL_TILE_PIXELS(qx, qy))
            moving = true;
    }
    return moving;
}

// Tile (qx, qy)'s orientation bits, none outside the map. The test ORs the 5x5 tiles around a group's own.
UL_FN int UiLayerTileBits(int qx, int qy, int tilesW, int tilesH)
{
    const bool inside = qx >= 0 && qy >= 0 && qx < tilesW && qy < tilesH;
    return inside ? (UL_TILE_BITS(qx, qy) & UL_ORIENTATIONS) : 0;
}

// Sample i of the camera test: the tile at the centre of cell i of a 16x16 grid over the map. textured: the tile
// is textured; moving: it is textured and moving.
UL_FN void UiLayerCameraSample(int i, int tilesW, int tilesH, UiLayerParams P, UL_OUT(float) textured,
                               UL_OUT(float) moving)
{
    const int qx = ((i % UL_CAMERA_GRID) * 2 + 1) * tilesW / (2 * UL_CAMERA_GRID);
    const int qy = ((i / UL_CAMERA_GRID) * 2 + 1) * tilesH / (2 * UL_CAMERA_GRID);
    const float pixels = UL_TILE_PIXELS(qx, qy);
    textured = UL_TILE_TEXTURED(qx, qy) >= P.texturedShare * pixels ? 1.0f : 0.0f;
    moving = (textured > 0.5f && UL_TILE_MOVED(qx, qy) >= P.movingShare * pixels) ? 1.0f : 0.0f;
}

// The camera moves when at least P.cameraShare of the sampled textured tiles moved. Flat tiles (sky) count for
// neither side, so a pan over a skyline still reads as one; a still scene with rain or water does not.
UL_FN bool UiLayerCameraMoving(float moving, float textured, UiLayerParams P)
{
    return textured > 0.0f && moving >= P.cameraShare * textured;
}

// One pixel's layer state for this frame.
//   history      last frame's layer state is this pixel's (false on the first frame, after a reset, and on the
//                first frame the layer runs); without it the pixel starts over
//   seedable     the group's tile tests passed (four sides, perpendicular orientations, a moving camera); false
//                without a history
//   luma         the pixel's luma now
//   contrast     its largest difference to a 4-neighbour
//   prevStill, prevHold, prevAnchor   last frame's state at the pixel
// Out: the still frames in a row, the anchor they are measured against, the seed's remaining hold, and the grown
// distance (UL_NO_DISTANCE: none).
//
// Still is against the anchor, the luma when the still run began, not against the frame before: every frame of
// the run is within stillEps of the anchor, so within twice that of every other, which is |I_t - I_{t-k}| small for
// every k up to the run's length. A frame-to-frame test lets a slow drift through, and a pan over smooth or dark
// texture moves each pixel by less than stillEps a frame (the GPU harness's darker content panning 4 px a frame
// did: 2026-09-29, "Built and measured").
UL_FN void UiLayerPixel(int x, int y, bool history, bool seedable, float luma, float contrast, float prevStill,
                        float prevHold, float prevAnchor, UiLayerParams P, UL_OUT(float) still, UL_OUT(float) anchor,
                        UL_OUT(float) hold, UL_OUT(float) dist)
{
    const bool kept = history && abs(luma - prevAnchor) < P.stillEps;
    still = kept ? min(prevStill + 1.0f, 255.0f) : 0.0f;
    anchor = kept ? prevAnchor : luma;
    const bool steady = still >= P.stillFrames; // never without a history, so prevHold is only read with one

    // A seed where the tests pass; it holds while the pixel stays steady, and a change drops it at once.
    hold = 0.0f;
    if (steady)
        hold = (seedable && contrast > P.detailMin) ? P.holdFrames : max(prevHold - 1.0f, 0.0f);

    // One growth step from last frame's distances, through steady pixels only.
    float d = UL_NO_DISTANCE;
    if (steady)
    {
        if (hold > 0.0f)
        {
            d = 0.0f;
        }
        else
        {
            UL_UNROLL for (int sy = -2; sy <= 2; ++sy)
            {
                UL_UNROLL for (int sx = -2; sx <= 2; ++sx)
                    d = min(d, UL_DIST_PREV(x + sx, y + sy) + float(max(abs(sx), abs(sy))));
            }
        }
    }
    dist = min(d, UL_NO_DISTANCE);
}

// What one pixel adds to this frame's tile map: whether it moved, whether it is textured, and the orientation of
// its structure while steady (a moving edge is not static structure). valid: there was a previous frame; without
// one, nothing moved.
UL_FN void UiLayerTileContribution(bool valid, float own, float contrast, float still, int orientation,
                                   UiLayerParams P, UL_OUT(float) moved, UL_OUT(float) textured, UL_OUT(int) bits)
{
    moved = (valid && own > P.movingTau) ? 1.0f : 0.0f;
    textured = contrast > P.texturedMin ? 1.0f : 0.0f;
    bits = still >= P.stillFrames ? orientation : 0;
}

#endif // UL_TILE_MOVED

#endif
