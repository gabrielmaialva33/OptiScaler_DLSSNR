# nr-kernel-profile

Host unit test for the DLSS-NR NvAPI CUDA kernel classification and aggregation (`OptiScaler/dlssnr/DlssNr_KernelProfile.h`).

Run: `python3 tests/nr-kernel-profile/run.py` (needs `g++` with C++20, ASan and UBSan).

Exercises:
- Kernel symbol prefix classification (pre_block, swin_*, vit_1d, cg2r_*, fallback to "other");
- Identification of FP8 kernel symbol variants (`_fp8`);
- Per-group sample timing aggregation, mean and p95 calculations, and percentage breakdown formatting.
