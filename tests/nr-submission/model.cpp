#include "OptiScaler/dlssnr/DlssNr_SubmissionModel.h"
#include <cassert>
#include <iostream>
#include <mutex>
#include <thread>

using namespace DlssNr::Submission::Detail;

int main()
{
    std::array<uint64_t, MaxQueues> fences {};
    auto completed = [&](size_t queue) { return fences[queue]; };
    auto submit = [](Model& m, uintptr_t list, size_t queue, uint64_t value, bool success = true)
    {
        auto e = m.BeforeExecute(list);
        assert(e);
        Model::AfterExecute(e, queue, value, success);
    };
    unsigned cases = 0;
    {
        Model m; Usage use;
        auto e = m.Track(1, use, completed);
        assert(e && !IsComplete(*e, completed) && !IsReady(*e, completed));
        auto snapshot = use;
        assert(snapshot.epochs.back() == e);
        Model::AfterReset(e, true);
        assert(!IsReady(*e, completed)); // missing/delayed submit never becomes proof
        ++cases;
    }
    {
        Model m; Usage use;
        auto e = m.Track(1, use, completed);
        submit(m, 1, 0, 1); fences[0] = 1;
        assert(IsComplete(*e, completed) && !IsReady(*e, completed));
        auto pending = m.BeforeExecute(1); // duplicate execute invalidates prior completion
        assert(!IsComplete(*e, completed));
        Model::AfterExecute(pending, 0, 2, true);
        assert(!IsComplete(*e, completed));
        Model::AfterReset(e, false);
        fences[0] = 2;
        assert(IsComplete(*e, completed) && !IsReady(*e, completed));
        Model::AfterReset(e, true);
        assert(IsReady(*e, completed));
        ++cases;
    }
    {
        Model m; Usage use;
        auto e = m.Track(2, use, completed);
        submit(m, 2, 0, 3); submit(m, 2, 1, 7);
        Model::AfterReset(e, true);
        fences[0] = 3; fences[1] = 6;
        assert(!IsReady(*e, completed));
        fences[1] = 7;
        assert(IsReady(*e, completed));
        fences[1] = UINT64_MAX;
        assert(!IsReady(*e, completed));
        fences[1] = 7;
        ++cases;
    }
    {
        Model m; Usage use;
        auto old = m.Track(3, use, completed);
        auto pending = m.BeforeExecute(3);
        Model::AfterReset(old, true); // reset after execute but before Signal notification
        assert(!IsReady(*old, completed));
        auto fresh = m.Track(3, use, completed);
        assert(fresh != old && !fresh->submitted);
        Model::AfterExecute(pending, 0, 9, true);
        assert(!IsReady(*old, completed) && !fresh->submitted);
        fences[0] = 9;
        assert(IsReady(*old, completed) && !IsComplete(*fresh, completed));
        ++cases;
    }
    {
        Model m; Usage use;
        auto e = m.Track(4, use, completed);
        submit(m, 4, 0, 10, false);
        assert(!m.Track(4, use, completed));
        Model::AfterReset(e, true); fences[0] = 100;
        assert(!IsReady(*e, completed));
        ++cases;
    }
    {
        Model m; Usage use;
        for (size_t i = 1; i <= MaxUses; ++i)
            assert(m.Track(i, use, completed));
        assert(!m.Track(MaxUses + 1, use, completed));
        // A checkpoint cannot lose unresolved uses under pressure.
        for (const auto& e : use.epochs)
            assert(e && !e->sealed);
        ++cases;
    }
    {
        Model m;
        std::array<Usage, MaxEpochs / MaxUses + 1> uses {};
        for (size_t i = 0; i < MaxEpochs; ++i)
            assert(m.Track(i + 1, uses[i / MaxUses], completed));
        assert(!m.Track(MaxEpochs + 1, uses.back(), completed));
        ++cases;
    }
    {
        Model m; Usage use;
        for (uint64_t frame = 1; frame <= 3000; ++frame)
        {
            auto e = m.Track(5, use, completed);
            assert(e);
            submit(m, 5, 0, frame);
            Model::AfterReset(e, true); fences[0] = frame;
            assert(IsReady(*e, completed));
        }
        ++cases;
    }
    {
        // Actual concurrent reset/notify interleavings under the production lock discipline.
        Model m; Usage use; std::mutex lock;
        for (uint64_t frame = 1; frame <= 1000; ++frame)
        {
            auto e = m.Track(6, use, completed);
            auto pending = m.BeforeExecute(6);
            std::thread reset([&] { std::lock_guard guard(lock); Model::AfterReset(e, true); });
            std::thread notify([&] { std::lock_guard guard(lock); Model::AfterExecute(pending, 2, frame, true); });
            reset.join(); notify.join();
            fences[2] = frame;
            assert(IsReady(*e, completed));
        }
        ++cases;
    }
    std::cout << "PASS: " << cases << " submission lifetime cases; 3000 retirements; 1000 concurrent reset/notify pairs\n";
}
