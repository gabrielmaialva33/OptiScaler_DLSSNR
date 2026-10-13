#include <dlssnr/DlssNr_PresentSync.h>

#include <cassert>
#include <cstdio>

int main()
{
    using DlssNr::PresentSyncInputs;
    using DlssNr::PresentSyncWanted;

    // Auto, nothing that paces the queue: the flip is on the write-back's own queue, so no CPU wait.
    assert(!PresentSyncWanted(std::nullopt, PresentSyncInputs {}));

    // Auto with any sign of frame generation keeps the wait, each on its own.
    {
        PresentSyncInputs in;
        in.fgHook = true;
        assert(PresentSyncWanted(std::nullopt, in));
    }
    {
        PresentSyncInputs in;
        in.fgOutput = true;
        assert(PresentSyncWanted(std::nullopt, in));
    }
    {
        PresentSyncInputs in;
        in.dlssgSeen = true;
        assert(PresentSyncWanted(std::nullopt, in));
    }
    {
        // A game that ships DLSS-G keeps the wait even where OptiScaler never saw its options.
        PresentSyncInputs in;
        in.dlssgLoaded = true;
        assert(PresentSyncWanted(std::nullopt, in));
    }

    // An explicit setting wins either way.
    {
        PresentSyncInputs fg;
        fg.fgHook = fg.fgOutput = fg.dlssgSeen = fg.dlssgLoaded = true;
        assert(!PresentSyncWanted(false, fg));
        assert(PresentSyncWanted(true, PresentSyncInputs {}));
    }

    std::printf("PASS: present sync -- auto waits only with frame generation (hook, output, DLSS-G seen or loaded), "
                "an explicit setting wins\n");
    return 0;
}
