#include <dlssnr/DlssNr_KernelProfile.h>

#include <cassert>
#include <cstdio>
#include <vector>

using namespace DlssNr::KernelProfile;

void TestClassification()
{
    // Exact and prefix matching
    const auto info1 = Classify("cc_vit_1d_repack_2d_to_1d_fp8");
    assert(info1.group == 7); // vit_1d
    assert(info1.fp8 == true);
    assert(std::string(kGroupNames[info1.group]) == "vit_1d");

    const auto info2 = Classify("pre_block_swin_1h_32");
    assert(info2.group == 0); // pre_block (precedes swin_1h_32)
    assert(info2.fp8 == false);

    const auto info3 = Classify("swin_1h_32_layer_0_fp8");
    assert(info3.group == 2); // swin_1h_32
    assert(info3.fp8 == true);

    const auto info4 = Classify("split_swin_16h_kernel");
    assert(info4.group == 6); // split_swin_16h (precedes swin)
    assert(info4.fp8 == false);

    const auto info5 = Classify("unknown_custom_gemm");
    assert(info5.group == kOtherGroupIndex);
    assert(info5.fp8 == false);

    const auto info6 = Classify(nullptr);
    assert(info6.group == kOtherGroupIndex);
    assert(info6.fp8 == false);
}

void TestMetricsAggregation()
{
    MetricsAggregator agg;

    double gMsA[kGroupCount] = {};
    unsigned gKernA[kGroupCount] = {};
    gMsA[7] = 2.0; // vit_1d takes 2.0 ms
    gMsA[2] = 8.0; // swin takes 8.0 ms
    gKernA[7] = 8;
    gKernA[2] = 32;

    agg.Add(10.0, gMsA, gKernA, 40, 0, "cc_vit_1d_fp8", "");

    double gMsB[kGroupCount] = {};
    unsigned gKernB[kGroupCount] = {};
    gMsB[7] = 2.5;
    gMsB[2] = 7.5;
    gKernB[7] = 8;
    gKernB[2] = 32;

    agg.Add(10.0, gMsB, gKernB, 40, 0, "cc_vit_1d_fp8", "");

    const auto report = agg.BuildReport();
    assert(report.evaluationsSampled == 2);
    assert(report.totalMeanMs == 10.0);
    assert(report.fp8Count == 80);
    assert(report.fp16Count == 0);
    assert(report.groups[7].kernelCount == 16);
    assert(report.groups[7].meanMs == 2.25);
    assert(std::fabs(report.groups[7].sharePercent - 22.5) < 1e-4);

    const auto text = report.Format();
    assert(text.find("vit_1d: 2.25 ms (22.5%, x16)") != std::string::npos);
}

int main()
{
    TestClassification();
    TestMetricsAggregation();
    std::printf("PASS: kernel profile classification and metrics aggregation\n");
    return 0;
}
