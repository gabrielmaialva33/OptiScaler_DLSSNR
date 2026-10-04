#include <dlssnr/DlssNr_KernelProfile.h>

#include <cassert>
#include <cstdio>
#include <vector>

using namespace DlssNr::KernelProfile;

void TestClassification()
{
    // Real kernel names confirmed in shipping nvngx_dlssnr.dll
    const auto info1 = Classify("cc_tinlayout_fused_swin_1h_32_1_fp8");
    assert(info1.group == 2); // swin_1h_32
    assert(info1.fp8 == true);
    assert(std::string(kGroupNames[info1.group]) == "swin_1h_32");

    const auto info2 = Classify("cc_split_swin_16h");
    assert(info2.group == 6); // split_swin_16h
    assert(info2.fp8 == false);

    const auto info3 = Classify("cc_dec_input_upsample");
    assert(info3.group == 10); // dec_input_upsample
    assert(info3.fp8 == false);

    const auto info4 = Classify("cc_tinlayout_fused_pre_block_swin_1h_32_1_fp8");
    assert(info4.group == 0); // pre_block (precedes swin_1h_32)
    assert(info4.fp8 == true);

    const auto info5 = Classify("cc_vit_1d_repack_2d_to_1d_fp8");
    assert(info5.group == 7); // vit
    // The second ViT family in the model's strings (attention, ffn, qkv, projection) is ViT too, not "other".
    assert(Classify("cc_vit_attention_fp8").group == 7 && Classify("cc_vit_ffn_expand_chained").group == 7);
    assert(info5.fp8 == true);

    const auto info6 = Classify("unknown_custom_gemm");
    assert(info6.group == kOtherGroupIndex);
    assert(info6.fp8 == false);

    const auto info7 = Classify(nullptr);
    assert(info7.group == kOtherGroupIndex);
    assert(info7.fp8 == false);
}

void TestMetricsAggregation()
{
    MetricsAggregator agg;

    double gMsA[kGroupCount] = {};
    unsigned gKernA[kGroupCount] = {};
    gMsA[7] = 2.0; // vit takes 2.0 ms
    gMsA[2] = 8.0; // swin takes 8.0 ms
    gKernA[7] = 8;
    gKernA[2] = 32;

    agg.Add(10.0, gMsA, gKernA, 40, 0, "cc_tinlayout_fused_swin_1h_32_1_fp8", "");

    double gMsB[kGroupCount] = {};
    unsigned gKernB[kGroupCount] = {};
    gMsB[7] = 2.5;
    gMsB[2] = 7.5;
    gKernB[7] = 8;
    gKernB[2] = 32;

    agg.Add(10.0, gMsB, gKernB, 40, 0, "cc_tinlayout_fused_swin_1h_32_1_fp8", "");

    const auto report = agg.BuildReport();
    assert(report.evaluationsSampled == 2);
    assert(report.totalMeanMs == 10.0);
    assert(report.totalP95Ms == 10.0);

    // Per-evaluate counts (average per evaluate across the sampled window)
    assert(report.fp8Count == 40);
    assert(report.fp16Count == 0);
    assert(report.groups[7].kernelCount == 8);
    assert(report.groups[2].kernelCount == 32);

    assert(report.groups[7].meanMs == 2.25);
    assert(report.groups[7].p95Ms == 2.50);
    assert(std::fabs(report.groups[7].sharePercent - 22.5) < 1e-4);

    const auto text = report.Format();
    assert(text.find("vit: 2.25 ms [max 2.50 ms] (22.5%, x8)") != std::string::npos);

    // Aggregator reset empties all window entries
    agg.Reset();
    assert(agg.Count() == 0);
    assert(agg.BuildReport().evaluationsSampled == 0);
}

int main()
{
    TestClassification();
    TestMetricsAggregation();
    std::printf("PASS: kernel profile classification and metrics aggregation\n");
    return 0;
}
