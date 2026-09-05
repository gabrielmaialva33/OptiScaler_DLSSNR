#include "../../OptiScaler/dlssnr/DlssNr_Chain.h"
#include "../../OptiScaler/dlssnr/DlssNr_SubmissionModel.h"
#include <cassert>
#include <iostream>
using namespace std::chrono_literals;

int main()
{
    unsigned cases = 0;
    // Production routing: original proxy is immutable, odd/even answers and partial failure.
    for (unsigned count = 1; count <= 3; ++count)
    {
        DlssNr::Chain::Routing<int> route(10, 20, 30);
        for (unsigned i = 0; i < count; ++i)
        {
            assert(route.Input() != route.Output());
            assert(route.Output() != route.original);
            route.Success();
        }
        assert(route.completed == count);
        assert(route.answer == (count % 2 ? 20 : 30));
        assert(route.original == 10);
        ++cases;
    }
    for (unsigned failAt = 0; failAt < 3; ++failAt)
    {
        DlssNr::Chain::Routing<int> route(10, 20, 30);
        for (unsigned i = 0; i < failAt; ++i) route.Success();
        const int lastGood = route.answer;
        // Failed outputs are deliberately not committed, even if the model wrote partially.
        assert(route.answer == lastGood);
        assert(route.completed == failAt);
        ++cases;
    }
    DlssNr::Chain::Schedule schedule;
    DlssNrPassSnapshot snapshot;
    const auto start = std::chrono::steady_clock::time_point(1s);
    assert(!schedule.Stable(snapshot, start));
    assert(!schedule.Stable(snapshot, start + 499ms));
    assert(schedule.Stable(snapshot, start + 500ms)); ++cases;
    assert(!schedule.Evaluate(0));
    assert(schedule.Evaluate(100));
    for (unsigned n = 0; n < 1000; ++n) assert(!schedule.Evaluate(100));
    assert(schedule.Evaluate(101)); ++cases;
    assert(schedule.Create(100, start));
    assert(!schedule.Create(100, start + 1s));
    assert(!schedule.Create(101, start + 499ms));
    assert(schedule.Create(101, start + 500ms)); ++cases;
    snapshot.Count = 3;
    assert(!schedule.Stable(snapshot, start + 1s));
    assert(schedule.Stable(snapshot, start + 1500ms));
    snapshot.Individual = true;
    snapshot.Settings[1].Intensity = 0.4f;
    assert(!schedule.Stable(snapshot, start + 2s));
    assert(!schedule.Stable(snapshot, start + 2499ms));
    assert(schedule.Stable(snapshot, start + 2500ms)); ++cases;
    snapshot.Count = 1;
    assert(!schedule.Stable(snapshot, start + 3s));
    assert(snapshot.Settings[1].Intensity == 0.4f); ++cases;

    constexpr uint64_t GiB = 1ull << 30;
    assert(!DlssNr::Chain::Admit(0, 0, 0, 0));
    assert(!DlssNr::Chain::Admit(8*GiB, 8*GiB, GiB, 0));
    assert(!DlssNr::Chain::Admit(8*GiB, 7*GiB, GiB, 0));
    assert(DlssNr::Chain::Admit(24*GiB, 8*GiB, GiB, 100ull << 20)); ++cases;

    // Actual completion model, composed with the observed-present requirement.
    namespace S = DlssNr::Submission::Detail;
    uint64_t completed = 0;
    auto completion = [&](size_t) { return completed; };
    S::Model submissions;
    S::Usage use;
    auto created = submissions.Track(1, use, completion);
    assert(created);
    assert(!S::IsComplete(*created, completion));
    auto submitted = submissions.BeforeExecute(1);
    S::Model::AfterExecute(submitted, 0, 1, true);
    assert(!S::IsComplete(*created, completion));
    completed = 1;
    const uint64_t createdFrame = 10;
    assert(!(10 > createdFrame && S::IsComplete(*created, completion)));
    assert(11 > createdFrame && S::IsComplete(*created, completion));
    assert(!S::IsReady(*created, completion));
    S::Model::AfterReset(created, true);
    assert(S::IsReady(*created, completion)); ++cases;
    auto failed = submissions.Track(2, use, completion);
    submitted = submissions.BeforeExecute(2);
    S::Model::AfterExecute(submitted, 0, 0, false);
    S::Model::AfterReset(failed, true);
    assert(!S::IsReady(*failed, completion)); ++cases;
    std::cout << "PASS: " << cases << " production chain/scheduling/admission/completion cases; 1000 same-present rejects\n";
}
