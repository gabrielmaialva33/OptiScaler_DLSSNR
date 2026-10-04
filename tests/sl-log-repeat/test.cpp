// The Streamline log repeat filter against the line that prompted it, on a fake clock.
#include <hooks/Streamline_LogRepeat.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace StreamlineLogRepeat;
using namespace std::chrono_literals;

static int failures = 0;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

// Verbatim from witcher3-rt-20260929/OptiScaler_84628303030.log, as trimStreamlineLog hands it over:
// the same site from two threads, 111 ms apart.
static const char* kFirst = "[09-59-04][streamline][error][tid:1980][10s:105ms:628us]rsync.cpp:660[setDynamicMFGParams] "
                            "RSYNC: setDynamicMFGParams failed with status 1";
static const char* kSecond = "[09-59-04][streamline][error][tid:1684][10s:256ms:390us]rsync.cpp:660[setDynamicMFGParams] "
                             "RSYNC: setDynamicMFGParams failed with status 1";

int main()
{
    // The site is what follows the bracketed prefix, and only the prefix is cut
    CHECK(Site(kFirst) == "rsync.cpp:660[setDynamicMFGParams] RSYNC: setDynamicMFGParams failed with status 1");
    CHECK(Site(kFirst) == Site(kSecond));
    CHECK(Site("no prefix at all") == "no prefix at all");
    CHECK(Site("[unterminated") == "[unterminated");
    CHECK(Site("") == "");

    {
        Filter filter;
        const auto t0 = Clock::time_point {} + 1h;

        // The first line is never swallowed
        auto d = filter.Observe(kFirst, t0);
        CHECK(d.write && d.folded == 0);

        // 75 a second for 9.9 s, from two threads: nothing written
        int written = 0;
        for (int i = 1; i < 743; ++i)
        {
            d = filter.Observe(i % 2 ? kSecond : kFirst, t0 + i * 13333us);
            written += d.write;
        }
        CHECK(written == 0);

        // Past the interval: one line, carrying every repeat it stands for
        d = filter.Observe(kFirst, t0 + 10s);
        CHECK(d.write && d.folded == 742);

        // And the count starts again from there
        d = filter.Observe(kSecond, t0 + 10s + 1ms);
        CHECK(!d.write);
        d = filter.Observe(kSecond, t0 + 20s);
        CHECK(d.write && d.folded == 1);

        // A long silence, then the same text: written, nothing folded
        d = filter.Observe(kFirst, t0 + 5min);
        CHECK(d.write && d.folded == 0);

        // Different text from the same file, and a different status, are other sites: written at once
        d = filter.Observe("[10-00-00][streamline][error][tid:1][1s:0ms:0us]rsync.cpp:660[setDynamicMFGParams] RSYNC: "
                           "setDynamicMFGParams failed with status 2",
                           t0 + 5min + 1ms);
        CHECK(d.write && d.folded == 0);
        CHECK(filter.Sites() == 2);
    }

    {
        // The table is bounded and fails open: past MaxSites every new site is written, every time
        Filter filter;
        const auto t0 = Clock::time_point {} + 1h;

        for (size_t i = 0; i < MaxSites; ++i)
            CHECK(filter.Observe("site " + std::to_string(i), t0).write);

        CHECK(filter.Sites() == MaxSites);
        CHECK(filter.Observe("one more", t0).write);
        CHECK(filter.Observe("one more", t0 + 1ms).write);
        CHECK(filter.Sites() == MaxSites);

        // A remembered site is still limited
        CHECK(!filter.Observe("site 0", t0 + 1ms).write);
    }

    if (failures != 0)
    {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }

    std::printf("PASS: Streamline log repeat filter: prefix cut to the site, one line per 10 s with the folded "
                "count, new sites at once, bounded table fails open\n");
    return EXIT_SUCCESS;
}
