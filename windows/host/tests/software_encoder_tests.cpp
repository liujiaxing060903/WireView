#include "../src/desktop_encoder.cpp"

int main(int argc, char** argv) {
    using namespace wireview::host;
    try {
        unsigned attempts{};
        const int degraded = with_encoder_fallback([&](EncoderMode mode) {
            ++attempts;
            if (mode != EncoderMode::software) { throw EncoderFailure("injected MFT 0x8000FFFF"); }
            return 7; // Never convert a failed/degraded return into success.
        });
        if (attempts != 3U || degraded != 7) { throw std::runtime_error("fallback order/result mismatch"); }
        std::vector<EncoderMode> modes;
        const int amf_fallback = with_encoder_fallback([&](EncoderMode mode) {
            modes.push_back(mode);
            if (mode == EncoderMode::amf_gpu) { throw EncoderFailure("injected AMF failure"); }
            return 9;
        }, true);
        if (modes != std::vector<EncoderMode>{EncoderMode::amf_gpu, EncoderMode::gpu} || amf_fallback != 9) {
            throw std::runtime_error("AMD fallback must try GPU Media Foundation before memory/software");
        }
        attempts = 0U;
        try {
            with_encoder_fallback([&](EncoderMode) -> int { ++attempts; throw std::runtime_error("transport failure"); });
            throw std::logic_error("transport error swallowed");
        } catch (const std::runtime_error& error) {
            if (attempts != 1U || std::string_view(error.what()) != "transport failure") { throw; }
        }
        attempts = 0U;
        try {
            with_encoder_fallback([&](EncoderMode) -> int { ++attempts; throw EncoderFailure("last real failure"); });
            throw std::logic_error("all-mode errors swallowed");
        } catch (const EncoderFailure& error) {
            if (attempts != 3U || std::string_view(error.what()) != "last real failure") { throw; }
        }
        if (argc == 2 && std::string_view(argv[1]) == "--desktop-software-benchmark") {
            return run_desktop_encode_attempt(std::chrono::seconds(3), {}, 640U, 400U, 60U, {}, EncoderMode::software);
        }
        if (argc == 2 && std::string_view(argv[1]) == "--desktop-hardware-memory-benchmark") {
            return run_desktop_encode_attempt(std::chrono::seconds(3), {}, 640U, 400U, 60U, {}, EncoderMode::hardware_memory);
        }
        ComApartment apartment;
        MediaFoundationSession mf;
        const auto activation = find_hardware_encoder({}, true);
        HardwareAvcEncoder encoder(activation.Get(), nullptr, 160U, 96U, 60U, true);
        encoder.start();
        unsigned submitted{}, encoded{}, keys{};
        bool draining{};
        LONGLONG previous = -1;
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (Clock::now() < deadline) {
            const auto event = encoder.next_event();
            if (event == METransformNeedInput) {
                if (submitted == 8U) { encoder.drain(); draining = true; continue; }
                ComPtr<IMFMediaBuffer> buffer;
                constexpr DWORD size = 160U * 96U * 3U / 2U;
                check_hresult(MFCreateMemoryBuffer(size, &buffer), "test input allocation");
                BYTE* bytes{};
                check_hresult(buffer->Lock(&bytes, nullptr, nullptr), "test input lock");
                std::memset(bytes, 90, 160U * 96U);
                std::memset(bytes + 160U * 96U, 128, size - 160U * 96U);
                buffer->Unlock();
                buffer->SetCurrentLength(size);
                if (submitted == 4U) { encoder.force_keyframe(); }
                encoder.submit_buffer(buffer.Get(), submitted * 166'666LL, 166'666LL);
                ++submitted;
            } else if (event == METransformHaveOutput) {
                const auto output = encoder.take_output();
                if (!output) { continue; }
                if (output->bytes.empty() || output->timestamp <= previous || !output->annex_b) {
                    throw std::runtime_error("invalid encoded sample/ordering/framing");
                }
                previous = output->timestamp;
                ++encoded;
                keys += output->keyframe ? 1U : 0U;
            } else if (event == METransformDrainComplete) { break; }
        }
        std::cout << "Software H.264 real MFT: submitted=" << submitted << " encoded=" << encoded << " keys=" << keys << '\n';
        return draining && encoded == 8U && keys >= 2U ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
