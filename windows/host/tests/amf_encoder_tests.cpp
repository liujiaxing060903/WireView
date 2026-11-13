#include "../src/desktop_encoder.cpp"

int main() {
    using namespace wireview::host;
    try {
        ComApartment apartment;
        auto target = find_capture_target({});
        if (target.adapter_description.VendorId != 0x1002U) { return 77; }
        auto d3d = create_video_device(target.adapter.Get());
        std::vector<std::uint8_t> pixels(2560U * 1600U * 3U / 2U, 128U);
        D3D11_TEXTURE2D_DESC description{};
        description.Width = 2560U;
        description.Height = 1600U;
        description.MipLevels = 1U;
        description.ArraySize = 1U;
        description.Format = DXGI_FORMAT_NV12;
        description.SampleDesc.Count = 1U;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
        const D3D11_SUBRESOURCE_DATA initial{pixels.data(), 2560U, 0U};
        std::array<ComPtr<ID3D11Texture2D>, 3> textures;
        struct Release final : amf::AMFSurfaceObserver {
            std::atomic<bool> free{true};
            std::atomic<unsigned> count{};
            void AMF_STD_CALL OnSurfaceDataRelease(amf::AMFSurface*) override { free.store(true); ++count; }
        };
        std::array<Release, 3> releases;
        for (auto& texture : textures) {
            check_hresult(d3d.device->CreateTexture2D(&description, &initial, &texture), "test NV12 texture");
        }
        HardwareAvcEncoder encoder(nullptr, d3d.device.Get(), 2560U, 1600U, 120U, true);
        encoder.start();
        constexpr unsigned frame_count = 130U;
        unsigned submitted{}, encoded{}, keys{};
        LONGLONG previous = -1;
        bool draining{}, complete{};
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < deadline && !complete) {
            const auto event = encoder.next_event();
            if (event == METransformNeedInput && !draining) {
                if (submitted == frame_count) { encoder.drain(); draining = true; continue; }
                if (submitted == 4U) { encoder.force_keyframe(); }
                std::size_t slot{};
                while (slot < releases.size() && !releases[slot].free.load()) { ++slot; }
                if (slot == releases.size()) { throw std::runtime_error("AMD requested input before releasing its texture pool"); }
                releases[slot].free.store(false);
                std::fill(pixels.begin(), pixels.begin() + 2560U * 1600U,
                    static_cast<std::uint8_t>(30U + submitted));
                d3d.context->UpdateSubresource(textures[slot].Get(), 0U, nullptr, pixels.data(), 2560U, 0U);
                encoder.submit(textures[slot].Get(), submitted * 83'333LL, 83'333LL, &releases[slot]);
                ++submitted;
            } else if (event == METransformHaveOutput) {
                const auto output = encoder.take_output();
                if (output.bytes.empty() || !output.annex_b ||
                    output.timestamp <= previous || output.timestamp != encoded * 83'333LL) {
                    throw std::runtime_error("AMD output lost its frame, timestamp, ordering, or Annex B framing");
                }
                if ((encoded == 0U || encoded == 4U) && !output.keyframe) {
                    throw std::runtime_error("AMD first/recovery frame was not an IDR");
                }
                previous = output.timestamp;
                keys += output.keyframe ? 1U : 0U;
                ++encoded;
            } else if (event == METransformDrainComplete) { complete = true; }
            else if (event == MEUnknown) { encoder.wait_for_event(50U); }
        }
        unsigned released{};
        for (const auto& release : releases) { released += release.count.load(); }
        std::cout << "AMD AMF: submitted=" << submitted << " encoded=" << encoded << " IDRs=" << keys
            << " inputReleases=" << released << '\n';
        return complete && submitted == frame_count && encoded == frame_count && keys >= 3U && released == frame_count ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
