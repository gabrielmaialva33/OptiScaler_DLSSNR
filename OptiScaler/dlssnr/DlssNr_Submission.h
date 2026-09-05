#pragma once

#include "DlssNr_SubmissionModel.h"
#include <d3d12.h>
#include <memory>

namespace DlssNr::Submission
{
using Usage = Detail::Usage;

// Opt-in only. Call before recording ANY use of the retained object/model. False
// refuses the work. Keep one Usage per resource generation and copy it at retirement.
bool Track(ID3D12GraphicsCommandList* list, Usage& usage);
bool Ready(const Usage& usage);
void Prune(Usage& usage);

// Completion observed so far, NOT permission to destroy: a list may execute again.
bool Completed(const Usage& usage);
bool Active();
const char* LastRefusal();

// Hook boundary: capture exact epochs BEFORE the original ExecuteCommandLists,
// then Signal AFTER it returns. The batch serializes tracked queue submissions so
// monotonically increasing fence values cannot be signaled out of order.
class Batch
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    Batch(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
    ~Batch();
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
    void Submitted();
};
} // namespace DlssNr::Submission
