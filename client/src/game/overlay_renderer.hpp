#pragma once

#include <cstdint>

struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11SamplerState;
struct ID3D11BlendState;
struct ID3D11RasterizerState;
struct ID3D11DepthStencilState;

namespace oxymp::client::game {

/// Вывод страницы интерфейса в кадр игры.
///
/// Точки страницы приходят обычным буфером и кладутся в текстуру, которая
/// растягивается на весь кадр поверх всего остального. Прозрачность берётся не
/// из четвёртого канала, а из самой картинки: страница рисует на чёрном, и
/// почти чёрные точки считаются пустотой. Так надёжнее — прозрачность оконного
/// снимка Windows задаёт по-разному в зависимости от версии и от того, как
/// заведено окно, а чернота есть чернота везде.
///
/// Работает только из перехвата показа кадра: контекст устройства Direct3D 11 не
/// выдерживает работы из двух потоков, и трогать его откуда-либо ещё нельзя.
class OverlayRenderer {
public:
    OverlayRenderer() = default;
    ~OverlayRenderer();

    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    /// Рисует страницу поверх кадра.
    ///
    /// changed говорит, обновлять ли текстуру: точки меняются реже кадров, и
    /// перезаливать их каждый раз незачем.
    void draw(IDXGISwapChain* swapchain, const std::uint8_t* pixels, int width, int height,
              bool changed);

    /// Заливает кадр целиком непрозрачным чёрным.
    ///
    /// Крышка на время начальной загрузки: пока страница не нарисовала свой
    /// первый кадр, показывать поверх игры нечего, а под нами идёт её
    /// вступительный ролик — логотипы и полицейская заставка. Заливка закрывает
    /// его с первого же кадра, на котором стоит перехват показа, и до входа в
    /// мир. Рисуется в самом низу, до страницы и меню, а те ложатся поверх.
    void fillOpaque(IDXGISwapChain* swapchain);

    /// Отпускает всё, что связано с буферами кадра.
    ///
    /// Обязательно перед пересозданием буферов: пока хоть кто-то держит ссылку
    /// на прежний буфер кадра, смена разрешения не удастся.
    void releaseFrameResources();

private:
    /// Заводит то, что не зависит от размера: шейдеры и состояния конвейера.
    [[nodiscard]] bool ensurePipeline(IDXGISwapChain* swapchain);

    /// Заводит текстуру под точки страницы. Пересоздаёт при смене размера.
    [[nodiscard]] bool ensureTexture(int width, int height);

    /// Заводит цель вывода из текущего буфера кадра.
    [[nodiscard]] bool ensureTarget(IDXGISwapChain* swapchain);

    void releaseAll();

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    ID3D11VertexShader* vertexShader_ = nullptr;
    ID3D11PixelShader* pixelShader_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    ID3D11RasterizerState* rasterizer_ = nullptr;
    ID3D11DepthStencilState* depthStencil_ = nullptr;

    ID3D11Texture2D* texture_ = nullptr;
    ID3D11ShaderResourceView* view_ = nullptr;
    int textureWidth_ = 0;
    int textureHeight_ = 0;

    ID3D11RenderTargetView* target_ = nullptr;

    /// Жаловались ли уже, что завести конвейер не вышло. Молчание после первой
    /// жалобы намеренное: кадров шестьдесят в секунду.
    bool failed_ = false;
};

} // namespace oxymp::client::game
