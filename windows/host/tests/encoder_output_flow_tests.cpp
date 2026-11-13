#include "encoder_output_flow.hpp"
#include <array>
#include <iostream>

int main() {
    using wireview::host::process_encoder_output;
    std::array<HRESULT, 2> replies{MF_E_TRANSFORM_STREAM_CHANGE, MF_E_TRANSFORM_NEED_MORE_INPUT};
    unsigned calls{}, renegotiations{};
    const auto result = process_encoder_output([&] { return replies.at(calls++); }, [&] { ++renegotiations; });
    bool ok = result == MF_E_TRANSFORM_NEED_MORE_INPUT && calls == 2U && renegotiations == 1U;
    replies[1] = E_UNEXPECTED;
    calls = 0U;
    ok = ok && process_encoder_output([&] { return replies.at(calls++); }, [] {}) == E_UNEXPECTED;
    calls = 0U;
    ok = ok && process_encoder_output([&] { ++calls; return MF_E_TRANSFORM_STREAM_CHANGE; }, [] {}) ==
        MF_E_TRANSFORM_STREAM_CHANGE && calls == 3U;
    std::cout << "MFT renegotiation / flow control / bounded failures: " << (ok ? "passed" : "FAILED") << '\n';
    return ok ? 0 : 1;
}
