#include "desktop_rotation.hpp"
#include <array>
#include <iostream>

int main() {
    using Microsoft::WRL::ComPtr;
    using wireview::host::DesktopRotation;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0U, D3D11_SDK_VERSION, &device, nullptr, &context))) { return 1; }
    const std::array<std::uint32_t, 6> pixels{0xff110000, 0xff220000, 0xff330000, 0xff440000, 0xff550000, 0xff660000};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 3U; desc.Height = 2U; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1U;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    D3D11_SUBRESOURCE_DATA data{pixels.data(), 12U, 0U};
    ComPtr<ID3D11Texture2D> source;
    if (FAILED(device->CreateTexture2D(&desc, &data, &source))) { return 1; }
    const std::array<std::array<unsigned, 6>, 4> expected{{
        {0, 1, 2, 3, 4, 5}, {3, 0, 4, 1, 5, 2}, {5, 4, 3, 2, 1, 0}, {2, 5, 1, 4, 0, 3}}};
    try {
        for (unsigned turn = 0U; turn < 4U; ++turn) {
            DesktopRotation rotation(device.Get(), context.Get(), 3U, 2U, desc.Format,
                static_cast<DXGI_MODE_ROTATION>(DXGI_MODE_ROTATION_IDENTITY + turn));
            rotation.update(source.Get());
            rotation.texture()->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0U; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) { return 1; }
            context->CopyResource(staging.Get(), rotation.texture());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped))) { return 1; }
            bool ok = true;
            for (unsigned i = 0U; i < 6U; ++i) {
                const auto* row = reinterpret_cast<const std::uint32_t*>(
                    static_cast<const BYTE*>(mapped.pData) + (i / desc.Width) * mapped.RowPitch);
                if (row[i % desc.Width] != pixels[expected[turn][i]]) { ok = false; }
            }
            context->Unmap(staging.Get(), 0U);
            if (!ok) { std::cerr << "pixel rotation mismatch: " << turn << '\n'; return 1; }
        }
        std::cout << "WARP actual texture rotation 0/90/180/270: passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
