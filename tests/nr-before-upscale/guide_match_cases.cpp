// The reduced-scale guide rules (OptiScaler/shaders/dlssnr/DlssNr_GuideMatch.h), pinned with the
// numbers of the cases they were written for. design/reduced-scale-guides.md.
#include <shaders/dlssnr/DlssNr_GuideMatch.h>

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace DlssNr::GuideMatch;

static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

int main()
{
    // Onimusha, as jlrouzies-fr measured it: 4K output, DLSS Performance (render 1920x1080), low-resolution
    // vectors with a game scale of 1920, model at 70% = 2688x1512. The guides are SMALLER than the model,
    // which is the upscaler's ordinary contract: no resample, and the scale stays the game's. The old
    // conversion gave 1344 -- every moving pixel reprojected halfway.
    assert(!Wanted(true, true, 2688, 1512, 1920, 1080, 1920, 1080));
    assert(MotionReference(true, 1920, 3840) == 1920);
    assert(Near(ModelMotionScale(1920.0f, 1920, 1920), 1920.0f));
    assert(Near(LegacyMotionScale(1920.0f, 2688, 3840), 1344.0f));

    // This tree's Cyberpunk shape at WorkingScale 0.5: 3440x1440 output, DLSS Quality (render 2293x960),
    // model 1720x720. The guides are LARGER than the model: resampled to 1720x720, and the scale is the
    // game's re-expressed for that texture. The old conversion was a third short.
    assert(Wanted(true, true, 1720, 720, 2293, 960, 2293, 960));
    assert(Near(ModelMotionScale(2293.0f, 1720, 2293), 1720.0f));
    assert(Near(LegacyMotionScale(2293.0f, 1720, 3440), 1146.5f));

    // DLSS Performance at WorkingScale 0.5: render 1720x720 is exactly the model. Nothing to resample, and
    // the old conversion halved every vector -- the case where only the scale was wrong.
    assert(!Wanted(true, true, 1720, 720, 1720, 720, 1720, 720));
    assert(Near(ModelMotionScale(1720.0f, 1720, 1720), 1720.0f));
    assert(Near(LegacyMotionScale(1720.0f, 1720, 3440), 860.0f));

    // A present host's synthesized motion: a display-size field in display pixels (scale 1, not
    // low-resolution), model at 0.5. Resampled; the scale comes out where the old one was, 0.5 -- for
    // display-resolution vectors only the guides were wrong.
    assert(MotionReference(false, 1720, 3440) == 3440);
    assert(Wanted(true, true, 1720, 720, 3440, 1440, 3440, 1440));
    assert(Near(ModelMotionScale(1.0f, 1720, 3440), 0.5f));
    assert(Near(LegacyMotionScale(1.0f, 1720, 3440), 0.5f));

    // Supersampling 2x: guides far smaller than the model, never resampled; the scale stays the game's
    // where the old conversion doubled it.
    assert(!Wanted(true, true, 6880, 2880, 2293, 960, 2293, 960));
    assert(Near(ModelMotionScale(2293.0f, 2293, 2293), 2293.0f));
    assert(Near(LegacyMotionScale(2293.0f, 6880, 3440), 4586.0f));

    // One axis larger is enough; switched off or without the bytecode, never.
    assert(Wanted(true, true, 1720, 720, 1700, 800, 1700, 700));
    assert(!Wanted(false, true, 1720, 720, 2293, 960, 2293, 960));
    assert(!Wanted(true, false, 1720, 720, 2293, 960, 2293, 960));
    assert(!Wanted(true, true, 0, 720, 2293, 960, 2293, 960));

    // A zero reference is a caller with nothing measured: the game's scale passes through.
    assert(Near(ModelMotionScale(3.0f, 100, 0), 3.0f));
    assert(Near(LegacyMotionScale(3.0f, 100, 0), 3.0f));

    // The point sample. Equal extents are the identity; 2:1 reads the second texel of each pair (the
    // destination centre lands on the pair's boundary and floors up); an offset region is offset.
    for (uint32_t d = 0; d < 1720; ++d)
        assert(PointSource(d, 1720, 0, 1720) == d);
    assert(PointSource(0, 1720, 0, 3440) == 1);
    assert(PointSource(1, 1720, 0, 3440) == 3);
    assert(PointSource(0, 1720, 10, 3440) == 11);

    // Every destination of a downsample and of an upsample reads inside its region, in order.
    const uint32_t regions[][4] = { { 1720, 2293, 7, 0 }, { 720, 960, 0, 0 }, { 2688, 1920, 0, 0 }, { 5, 3, 2, 0 } };
    for (const auto& r : regions)
    {
        uint32_t previous = 0;
        for (uint32_t d = 0; d < r[0]; ++d)
        {
            const uint32_t s = PointSource(d, r[0], r[2], r[1]);
            assert(s >= r[2] && s < r[2] + r[1]);
            assert(d == 0 || s >= previous);
            previous = s;
        }
    }
    assert(PointSource(2687, 2688, 0, 1920) == 1919);

    // Degenerate extents never divide by zero.
    assert(PointSource(3, 0, 4, 10) == 4);
    assert(PointSource(3, 10, 4, 0) == 4);

    std::puts("guide match: reduced-scale guide and motion-scale rules hold");
    return 0;
}
