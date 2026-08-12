#include "overlay_renderer.hpp"

#include <spdlog/spdlog.h>

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstring>

namespace oxymp::client::game {
namespace {

/// Вершинный шейдер без вершин.
///
/// Треугольник, накрывающий экран, строится из номера вершины: три точки, и
/// никакого буфера вершин заводить не нужно. Заодно это избавляет от заботы о
/// разметке ввода, которую пришлось бы сохранять и возвращать игре.
constexpr const char* kVertexShader = R"(
struct Output {
    float4 position : SV_POSITION;
    float2 texture0 : TEXCOORD0;
};

Output main(uint id : SV_VertexID) {
    Output output;

    const float2 corner = float2((id << 1) & 2, id & 2);

    output.texture0 = corner;
    output.position = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);

    return output;
}
)";

/// Пиксельный шейдер: цвет страницы как есть.
///
/// Прозрачность берётся из четвёртого канала, и это стало возможно вместе с
/// переходом на CEF. Прежний движок умел отдавать страницу только в окно, а
/// снимок окна приходил без внятной прозрачности — приходилось выводить её из
/// яркости, считая почти чёрные точки пустотой. Порог этот требовал от страницы
/// не рисовать ничего тёмного и всё равно оставлял каёмку по краям панелей.
///
/// CEF отдаёт настоящую прозрачность, уже помноженную на цвет: складывать её с
/// кадром нужно как «единица на обратную прозрачность», без повторного
/// умножения. Отсюда и вид шейдера — точка отдаётся как пришла.
constexpr const char* kPixelShader = R"(
Texture2D page : register(t0);
SamplerState pageSampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 texture0 : TEXCOORD0) : SV_TARGET {
    return page.Sample(pageSampler, texture0);
}
)";

/// Всё, что мы меняем в конвейере и обязаны вернуть игре.
///
/// Возвращать необходимо: игра не ждёт, что кто-то трогал её устройство между
/// кадрами, и оставленное нами состояние она примет за своё. Проявляется это не
/// пропавшим интерфейсом, а испорченной картинкой игры — что искать несравнимо
/// труднее.
struct SavedState {
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* depthStencilView = nullptr;

    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

    ID3D11RasterizerState* rasterizer = nullptr;

    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4]{};
    UINT sampleMask = 0;

    ID3D11DepthStencilState* depthStencil = nullptr;
    UINT stencilReference = 0;

    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11InputLayout* layout = nullptr;

    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    ID3D11GeometryShader* geometryShader = nullptr;

    ID3D11ShaderResourceView* resource = nullptr;
    ID3D11SamplerState* sampler = nullptr;
};

void save(ID3D11DeviceContext* context, SavedState& state) {
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, state.targets,
                                &state.depthStencilView);
    context->RSGetViewports(&state.viewportCount, state.viewports);
    context->RSGetState(&state.rasterizer);
    context->OMGetBlendState(&state.blend, state.blendFactor, &state.sampleMask);
    context->OMGetDepthStencilState(&state.depthStencil, &state.stencilReference);
    context->IAGetPrimitiveTopology(&state.topology);
    context->IAGetInputLayout(&state.layout);
    context->VSGetShader(&state.vertexShader, nullptr, nullptr);
    context->PSGetShader(&state.pixelShader, nullptr, nullptr);
    context->GSGetShader(&state.geometryShader, nullptr, nullptr);
    context->PSGetShaderResources(0, 1, &state.resource);
    context->PSGetSamplers(0, 1, &state.sampler);
}

void restore(ID3D11DeviceContext* context, SavedState& state) {
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, state.targets,
                                state.depthStencilView);
    context->RSSetViewports(state.viewportCount, state.viewports);
    context->RSSetState(state.rasterizer);
    context->OMSetBlendState(state.blend, state.blendFactor, state.sampleMask);
    context->OMSetDepthStencilState(state.depthStencil, state.stencilReference);
    context->IASetPrimitiveTopology(state.topology);
    context->IASetInputLayout(state.layout);
    context->VSSetShader(state.vertexShader, nullptr, 0);
    context->PSSetShader(state.pixelShader, nullptr, 0);
    context->GSSetShader(state.geometryShader, nullptr, 0);
    context->PSSetShaderResources(0, 1, &state.resource);
    context->PSSetSamplers(0, 1, &state.sampler);

    // Всё, что отдали методы Get, приходит с увеличенным счётчиком ссылок, и
    // отпустить это обязаны мы.
    for (ID3D11RenderTargetView*& target : state.targets) {
        if (target != nullptr) {
            target->Release();
            target = nullptr;
        }
    }

    const auto release = [](auto*& object) {
        if (object != nullptr) {
            object->Release();
            object = nullptr;
        }
    };

    release(state.depthStencilView);
    release(state.rasterizer);
    release(state.blend);
    release(state.depthStencil);
    release(state.layout);
    release(state.vertexShader);
    release(state.pixelShader);
    release(state.geometryShader);
    release(state.resource);
    release(state.sampler);
}

/// Собирает шейдер из исходного текста.
///
/// Во время работы, а не при сборке: собранный заранее шейдер пришлось бы
/// держать в проекте двоичным файлом, порождённым чужим средством, — а сборка
/// одного треугольника занимает доли миллисекунды и делается один раз за запуск.
ID3DBlob* compile(const char* source, const char* profile) {
    ID3DBlob* result = nullptr;
    ID3DBlob* complaint = nullptr;

    const HRESULT compiled =
        ::D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", profile,
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &result, &complaint);

    if (complaint != nullptr) {
        if (FAILED(compiled)) {
            spdlog::error("шейдер интерфейса не собрался: {}",
                          static_cast<const char*>(complaint->GetBufferPointer()));
        }
        complaint->Release();
    }

    if (FAILED(compiled)) {
        if (result != nullptr) {
            result->Release();
        }
        return nullptr;
    }

    return result;
}

} // namespace

OverlayRenderer::~OverlayRenderer() {
    releaseAll();
}

void OverlayRenderer::releaseFrameResources() {
    if (target_ != nullptr) {
        target_->Release();
        target_ = nullptr;
    }
}

void OverlayRenderer::releaseAll() {
    releaseFrameResources();

    const auto release = [](auto*& object) {
        if (object != nullptr) {
            object->Release();
            object = nullptr;
        }
    };

    release(view_);
    release(texture_);
    release(depthStencil_);
    release(rasterizer_);
    release(blend_);
    release(sampler_);
    release(pixelShader_);
    release(vertexShader_);
    release(context_);
    release(device_);

    textureWidth_ = 0;
    textureHeight_ = 0;
}

bool OverlayRenderer::ensurePipeline(IDXGISwapChain* swapchain) {
    if (device_ != nullptr) {
        return true;
    }

    if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device_)))) {
        spdlog::error("у кадра игры не оказалось устройства Direct3D 11 — интерфейс в кадре "
                      "рисовать нечем");
        return false;
    }

    device_->GetImmediateContext(&context_);
    if (context_ == nullptr) {
        return false;
    }

    ID3DBlob* const vertex = compile(kVertexShader, "vs_4_0");
    if (vertex == nullptr) {
        return false;
    }

    const HRESULT vertexCreated = device_->CreateVertexShader(
        vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vertexShader_);
    vertex->Release();

    if (FAILED(vertexCreated)) {
        return false;
    }

    ID3DBlob* const pixel = compile(kPixelShader, "ps_4_0");
    if (pixel == nullptr) {
        return false;
    }

    const HRESULT pixelCreated = device_->CreatePixelShader(pixel->GetBufferPointer(),
                                                            pixel->GetBufferSize(), nullptr,
                                                            &pixelShader_);
    pixel->Release();

    if (FAILED(pixelCreated)) {
        return false;
    }

    D3D11_SAMPLER_DESC samplerDescription{};
    samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;

    if (FAILED(device_->CreateSamplerState(&samplerDescription, &sampler_))) {
        return false;
    }

    D3D11_BLEND_DESC blendDescription{};
    blendDescription.RenderTarget[0].BlendEnable = TRUE;
    // Единица, а не прозрачность источника: CEF отдаёт цвет уже помноженным на
    // неё, и второе умножение сделало бы полупрозрачные панели вдвое тусклее.
    blendDescription.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDescription.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDescription.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDescription.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDescription.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDescription.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDescription.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (FAILED(device_->CreateBlendState(&blendDescription, &blend_))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizerDescription{};
    rasterizerDescription.FillMode = D3D11_FILL_SOLID;
    rasterizerDescription.CullMode = D3D11_CULL_NONE;
    rasterizerDescription.DepthClipEnable = TRUE;

    if (FAILED(device_->CreateRasterizerState(&rasterizerDescription, &rasterizer_))) {
        return false;
    }

    // Глубина не нужна вовсе: интерфейс лежит поверх всего и ни с чем не спорит.
    D3D11_DEPTH_STENCIL_DESC depthDescription{};
    depthDescription.DepthEnable = FALSE;
    depthDescription.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDescription.DepthFunc = D3D11_COMPARISON_ALWAYS;

    if (FAILED(device_->CreateDepthStencilState(&depthDescription, &depthStencil_))) {
        return false;
    }

    spdlog::info("интерфейс рисуется прямо в кадр игры");
    return true;
}

bool OverlayRenderer::ensureTexture(int width, int height) {
    if (texture_ != nullptr && textureWidth_ == width && textureHeight_ == height) {
        return true;
    }

    if (view_ != nullptr) {
        view_->Release();
        view_ = nullptr;
    }
    if (texture_ != nullptr) {
        texture_->Release();
        texture_ = nullptr;
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(width);
    description.Height = static_cast<UINT>(height);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(device_->CreateTexture2D(&description, nullptr, &texture_))) {
        return false;
    }

    if (FAILED(device_->CreateShaderResourceView(texture_, nullptr, &view_))) {
        return false;
    }

    textureWidth_ = width;
    textureHeight_ = height;

    return true;
}

bool OverlayRenderer::ensureTarget(IDXGISwapChain* swapchain) {
    if (target_ != nullptr) {
        return true;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&backBuffer)))) {
        return false;
    }

    const HRESULT created = device_->CreateRenderTargetView(backBuffer, nullptr, &target_);
    backBuffer->Release();

    return SUCCEEDED(created);
}

void OverlayRenderer::draw(IDXGISwapChain* swapchain, const std::uint8_t* pixels, int width,
                           int height, bool changed) {
    if (failed_ || swapchain == nullptr || pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }

    if (!ensurePipeline(swapchain) || !ensureTexture(width, height) || !ensureTarget(swapchain)) {
        failed_ = true;
        releaseAll();
        spdlog::error("интерфейс в кадре игры отключён: подготовить вывод не удалось");
        return;
    }

    if (changed) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(texture_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;

            // Построчно: у отображённой текстуры между строками бывает
            // промежуток, и копирование целиком сдвинуло бы картинку.
            for (int row = 0; row < height; ++row) {
                std::memcpy(static_cast<std::uint8_t*>(mapped.pData) +
                                static_cast<std::size_t>(row) * mapped.RowPitch,
                            pixels + static_cast<std::size_t>(row) * rowBytes, rowBytes);
            }

            context_->Unmap(texture_, 0);
        }
    }

    SavedState saved;
    save(context_, saved);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<FLOAT>(width);
    viewport.Height = static_cast<FLOAT>(height);
    viewport.MaxDepth = 1.0F;

    // Размер берётся у самого кадра, а не у страницы: они совпадают почти
    // всегда, но в тот кадр, когда игра сменила разрешение, а снимок ещё нет,
    // растянуть нужно по кадру.
    DXGI_SWAP_CHAIN_DESC chain{};
    if (SUCCEEDED(swapchain->GetDesc(&chain))) {
        viewport.Width = static_cast<FLOAT>(chain.BufferDesc.Width);
        viewport.Height = static_cast<FLOAT>(chain.BufferDesc.Height);
    }

    const FLOAT blendFactor[4] = {0.0F, 0.0F, 0.0F, 0.0F};

    context_->OMSetRenderTargets(1, &target_, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_);
    context_->OMSetBlendState(blend_, blendFactor, 0xFFFFFFFF);
    context_->OMSetDepthStencilState(depthStencil_, 0);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_, nullptr, 0);
    context_->PSSetShader(pixelShader_, nullptr, 0);
    context_->GSSetShader(nullptr, nullptr, 0);
    context_->PSSetShaderResources(0, 1, &view_);
    context_->PSSetSamplers(0, 1, &sampler_);

    context_->Draw(3, 0);

    restore(context_, saved);
}

} // namespace oxymp::client::game
