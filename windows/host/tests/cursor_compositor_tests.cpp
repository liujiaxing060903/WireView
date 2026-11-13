#include "../src/desktop_encoder.cpp"

int main() {
    using namespace wireview::host;
    try {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "test GPU device");
        D3D11_TEXTURE2D_DESC description{};
        description.Width = 320;
        description.Height = 240;
        description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        std::vector<std::uint32_t> background(320 * 240, 0xff000000U);
        D3D11_SUBRESOURCE_DATA data{background.data(), 320 * 4, 0};
        ComPtr<ID3D11Texture2D> desktop;
        check_hresult(device->CreateTexture2D(&description, &data, &desktop), "test desktop");
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;
        check_hresult(device->CreateTexture2D(&description, nullptr, &readback), "test readback");

        const auto bounds = [&](CursorCompositor& compositor) {
            context->CopyResource(readback.Get(), compositor.texture());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check_hresult(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "test pixels");
            std::array<int, 4> result{320, 240, 0, 0};
            for (int y = 0; y < 240; ++y) {
                const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
                for (int x = 0; x < 320; ++x) {
                    if (row[x * 4] < 200 || row[x * 4 + 1] < 200 || row[x * 4 + 2] < 200) { continue; }
                    result = {std::min(result[0], x), std::min(result[1], y),
                        std::max(result[2], x + 1), std::max(result[3], y + 1)};
                }
            }
            context->Unmap(readback.Get(), 0);
            return result;
        };
        for (const UINT dimension : {32U, 48U, 96U}) {
            CursorCompositor compositor(device.Get(), context.Get(), 320, 240,
                DXGI_FORMAT_B8G8R8A8_UNORM, 1.0F, 1.0F);
            DesktopPointer pointer;
            pointer.visible = pointer.system_cursor_shape = true;
            pointer.width = pointer.height = dimension;
            pointer.pitch = dimension * 4;
            pointer.type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
            pointer.x = 100;
            pointer.y = 80;
            pointer.hotspot_x = 3;
            pointer.hotspot_y = 4;
            pointer.bytes.resize(dimension * dimension * 4);
            for (UINT y = 3; y < 10; ++y) {
                for (UINT x = 2; x < 7; ++x) {
                    std::memset(pointer.bytes.data() + (y * dimension + x) * 4, 255, 4);
                }
            }
            compositor.compose(desktop.Get(), pointer);
            const auto actual = bounds(compositor);
            if (actual != std::array<int, 4>{99, 79, 104, 86}) {
                std::cerr << "cursor padding changed size/anchor: source=" << dimension
                    << " bounds=" << actual[0] << ',' << actual[1] << ',' << actual[2] << ',' << actual[3] << '\n';
                return 1;
            }
            // A smaller shape must also clear all pixels from the previous shape.
            pointer.width = pointer.height = 32;
            pointer.pitch = 128;
            pointer.hotspot_x = pointer.hotspot_y = 0;
            pointer.bytes.assign(32 * 32 * 4, 0);
            for (UINT y = 0; y < 6; ++y) {
                for (UINT x = 0; x < 4; ++x) {
                    std::memset(pointer.bytes.data() + (y * 32 + x) * 4, 255, 4);
                }
            }
            ++pointer.shape_version;
            compositor.compose(desktop.Get(), pointer);
            if (bounds(compositor) != std::array<int, 4>{100, 80, 104, 86}) {
                throw std::runtime_error("smaller cursor retains pixels or changes 100% size");
            }
            CursorCompositor doubled(device.Get(), context.Get(), 320, 240,
                DXGI_FORMAT_B8G8R8A8_UNORM, 1.0F, 2.0F);
            doubled.compose(desktop.Get(), pointer);
            if (bounds(doubled) != std::array<int, 4>{100, 80, 108, 92}) {
                throw std::runtime_error("custom size must still double the cursor at its hotspot");
            }
        }
        std::cout << "cursor GPU pixels: source padding, shape change, anchor and custom size passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
