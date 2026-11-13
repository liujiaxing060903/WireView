#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstring>
#include <stdexcept>
#include <string>

namespace wireview::host {
// DXGI supplies unrotated surfaces even for portrait desktops. Normalize the
// desktop BEFORE cursor composition/scaling so all subsequent coordinates,
// including touch, remain in the display's logical orientation.
class DesktopRotation {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    static void check(HRESULT result, const char* operation) {
        if (FAILED(result)) { throw std::runtime_error(std::string(operation) + ": " + std::to_string(result)); }
    }
public:
    DesktopRotation(ID3D11Device* device, ID3D11DeviceContext* context,
        UINT width, UINT height, DXGI_FORMAT format, DXGI_MODE_ROTATION rotation)
        : context_(context), width_(width), height_(height) {
        const bool portrait = rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270;
        if (portrait) { width_ = height; height_ = width; }
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width; description.Height = height;
        description.MipLevels = 1U; description.ArraySize = 1U;
        description.Format = format; description.SampleDesc.Count = 1U;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&description, nullptr, &source_), "rotation source texture");
        check(device->CreateShaderResourceView(source_.Get(), nullptr, &source_view_), "rotation source view");
        description.Width = width_; description.Height = height_;
        description.BindFlags = D3D11_BIND_RENDER_TARGET;
        check(device->CreateTexture2D(&description, nullptr, &output_), "rotation output texture");
        check(device->CreateRenderTargetView(output_.Get(), nullptr, &output_view_), "rotation output view");
        static constexpr char vertex[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
})";
        // Mapping matches the DXGI Desktop Duplication rotation convention:
        // ROTATE90 maps a raw source (x,y) to logical (rawHeight-1-y,x).
        const std::string pixel = std::string(R"(
Texture2D<float4> source : register(t0);
float4 main(float4 position : SV_Position) : SV_Target {
    uint2 p = uint2(position.xy);
    uint w, h; source.GetDimensions(w, h);
)") + (rotation == DXGI_MODE_ROTATION_ROTATE90 ? "p = uint2(p.y, h - 1 - p.x);" :
            rotation == DXGI_MODE_ROTATION_ROTATE180 ? "p = uint2(w - 1 - p.x, h - 1 - p.y);" :
            rotation == DXGI_MODE_ROTATION_ROTATE270 ? "p = uint2(w - 1 - p.y, p.x);" : "") +
            "return source.Load(int3(p, 0)); }";
        Ptr<ID3DBlob> vs, ps, errors;
        check(D3DCompile(vertex, std::strlen(vertex), nullptr, nullptr, nullptr, "main", "vs_5_0", 0U, 0U, &vs, &errors), "rotation vertex shader");
        check(D3DCompile(pixel.data(), pixel.size(), nullptr, nullptr, nullptr, "main", "ps_5_0", 0U, 0U, &ps, &errors), "rotation pixel shader");
        check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_), "rotation vertex shader creation");
        check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_), "rotation pixel shader creation");
    }
    void update(ID3D11Texture2D* desktop) {
        context_->CopyResource(source_.Get(), desktop);
        const D3D11_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_), 0.0F, 1.0F};
        context_->OMSetRenderTargets(1U, output_view_.GetAddressOf(), nullptr);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffffU);
        context_->RSSetState(nullptr);
        context_->RSSetViewports(1U, &viewport);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_.Get(), nullptr, 0U);
        context_->PSSetShader(pixel_.Get(), nullptr, 0U);
        context_->PSSetShaderResources(0U, 1U, source_view_.GetAddressOf());
        context_->Draw(3U, 0U);
        ID3D11ShaderResourceView* empty{};
        context_->PSSetShaderResources(0U, 1U, &empty);
        context_->OMSetRenderTargets(0U, nullptr, nullptr);
    }
    ID3D11Texture2D* texture() const noexcept { return output_.Get(); }
    UINT width() const noexcept { return width_; }
    UINT height() const noexcept { return height_; }
private:
    Ptr<ID3D11DeviceContext> context_;
    Ptr<ID3D11Texture2D> source_, output_;
    Ptr<ID3D11ShaderResourceView> source_view_;
    Ptr<ID3D11RenderTargetView> output_view_;
    Ptr<ID3D11VertexShader> vertex_;
    Ptr<ID3D11PixelShader> pixel_;
    UINT width_, height_;
};
} // namespace wireview::host
