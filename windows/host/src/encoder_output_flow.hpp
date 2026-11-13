#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mferror.h>

namespace wireview::host {
// Each ProcessOutput attempt must allocate using the latest stream info.
// NEED_MORE_INPUT is flow control, including immediately after STREAM_CHANGE.
template<class Process, class Renegotiate>
HRESULT process_encoder_output(Process process, Renegotiate renegotiate) {
    for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
        const HRESULT result = process();
        if (result != MF_E_TRANSFORM_STREAM_CHANGE) { return result; }
        renegotiate();
    }
    return MF_E_TRANSFORM_STREAM_CHANGE;
}
} // namespace wireview::host
