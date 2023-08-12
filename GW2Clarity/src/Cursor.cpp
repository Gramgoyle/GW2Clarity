#include "Cursor.h"

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <range/v3/all.hpp>

#include "Core.h"
#include "ImGuiExtensions.h"

namespace GW2Clarity
{

Cursor::Cursor(ComPtr<ID3D11Device>& dev) : activateCursor_("activate_cursor", "Toggle Cursor", "General") {
    Load();

    activateCursor_.callback([&](Activated a) {
        if(a == Activated::Yes)
            visible_ = !visible_;
        return PassToGame::Allow;
    });

    auto& sm = ShaderManager::i();

    const auto& testImage = Core::i().testImage();
    previewImage_ = MakeRenderTarget(dev, testImage.width, testImage.height, DXGI_FORMAT_R8G8B8A8_UNORM);

    copyVS_ = sm.GetShader(L"Copy.hlsl", D3D11_SHVER_VERTEX_SHADER, "ScreenQuad_VS");
    copyPS_ = sm.GetShader(L"Copy.hlsl", D3D11_SHVER_PIXEL_SHADER, "Copy_PS");

    cursorCB_ = sm.MakeConstantBuffer<CursorData>();
    cursorVS_ = sm.GetShader(L"Cursor.hlsl", D3D11_SHVER_VERTEX_SHADER, "ScreenQuad_VS");
    auto makePS = [&](const char* ep) {
        return sm.GetShader(L"Cursor.hlsl", D3D11_SHVER_PIXEL_SHADER, ep);
    };
    const auto circlePS = makePS("Circle_PS");
    const auto squarePS = makePS("Square_PS");
    const auto crossPS = makePS("Cross_PS");
    cursorPS_[get_index<Layer::Circle, decltype(Layer::type)>()] = circlePS;
    cursorPS_[get_index<Layer::Square, decltype(Layer::type)>()] = squarePS;
    cursorPS_[get_index<Layer::Cross, decltype(Layer::type)>()] = crossPS;

    CD3D11_BLEND_DESC blendDesc(D3D11_DEFAULT);
    blendDesc.RenderTarget[0].BlendEnable = true;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    GW2_CHECKED_HRESULT(dev->CreateBlendState(&blendDesc, defaultBlend_.GetAddressOf()));

    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_SUBTRACT;
    GW2_CHECKED_HRESULT(dev->CreateBlendState(&blendDesc, invertBlend_.GetAddressOf()));

    SettingsMenu::i().AddImplementer(this);
}

Cursor::~Cursor() = default;

void Cursor::Draw(ComPtr<ID3D11DeviceContext>& ctx) {
    if(!SettingsMenu::i().isVisible() && editor_.Editing())
        editor_.StopEditing();

    if(!visible_ && !editor_.Editing())
        return;

    if(editor_.Editing()) {
        constexpr float clear[] = { 0.f };
        ctx->ClearRenderTargetView(previewImage_.rtv.Get(), clear);

        UINT numVPs = 1;
        D3D11_VIEWPORT oldVP;
        ctx->RSGetViewports(&numVPs, &oldVP);

        // Setup viewport
        D3D11_VIEWPORT vp;
        memset(&vp, 0, sizeof(D3D11_VIEWPORT));
        vp.Width = previewImage_.width;
        vp.Height = previewImage_.height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        vp.TopLeftX = vp.TopLeftY = 0;
        ctx->RSSetViewports(1, &vp);

        ShaderManager::i().SetShaders(ctx.Get(), copyVS_, copyPS_);
        ID3D11ShaderResourceView* srvs[] = { Core::i().testImage().srv.Get() };
        ctx->PSSetShaderResources(0, 1, srvs);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ID3D11RenderTargetView* rtvs[] = { previewImage_.rtv.Get() };
        ctx->OMSetRenderTargets(1, rtvs, nullptr);

        DrawScreenQuad(ctx.Get());

        const f32 t = TimeInMilliseconds() * 1.e-3f;

        DrawLayer(ctx.Get(), *editor_.selectedItem(), vec2(0.5f + sin(t) * 0.15f, 0.5f + cos(t) * 0.15f), vec2(80.f));

        rtvs[0] = Core::i().backBufferRTV().Get();
        ctx->OMSetRenderTargets(1, rtvs, nullptr);
        ctx->RSSetViewports(1, &oldVP);
    }

    const auto& io = ImGui::GetIO();
    auto mp = FromImGui(io.MousePos) / Core::i().screenDims();

    if(glm::any(glm::lessThan(mp, vec2(0.f))) || glm::any(glm::greaterThan(mp, vec2(1.f))))
        return;

    for(auto& l : layers_)
        DrawLayer(ctx.Get(), l, mp, Core::i().screenDims());
}

void Cursor::DrawMenu(Keybind** currentEditedKeybind) {
    editor_.Draw([&](auto& editLayer, auto& save) {
        if(!editLayer.name.empty())
            ImGuiTitle(std::format("Editing Cursor Layer '{}'", editLayer.name).c_str(), 0.75f);
        else
            ImGuiTitle("New Cursor Layer", 0.75f);

        save(ImGui::InputText("Name##NewLayer", &editLayer.name));

        ImGui::Image(previewImage_.srv.Get(), ImVec2(80.f, 80.f));

        auto currTypeName = std::visit(Overloaded { [](const Layer::Circle&) { return "Circle"; },
                                                    [](const Layer::Square&) { return "Square"; },
                                                    [](const Layer::Cross&) {
                                                        return "Cross";
                                                    } },
                                       editLayer.type);

        SCOPE(Combo("Shape", currTypeName)) {
            const auto currIndex = editLayer.type.index();
            const auto isCircle = get_index<Layer::Circle, decltype(editLayer.type)>() == currIndex;
            const auto isSquare = get_index<Layer::Square, decltype(editLayer.type)>() == currIndex;
            const auto isCross = get_index<Layer::Cross, decltype(editLayer.type)>() == currIndex;
            if(save(ImGui::Selectable("Circle", isCircle) && !isCircle)) {
                editLayer.type = Layer::Circle {};
            }

            if(save(ImGui::Selectable("Square", isSquare) && !isSquare)) {
                editLayer.type = Layer::Square {};
            }

            if(save(ImGui::Selectable("Cross", isCross) && !isCross)) {
                editLayer.type = Layer::Cross {};
            }
        }

        save(ImGui::ColorEdit4("Border Color & Transparency",
                               glm::value_ptr(editLayer.colorBorder),
                               ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreview));

        save(ImGui::ColorEdit4("Fill Color & Transparency",
                               glm::value_ptr(editLayer.colorFill),
                               ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreview));

        save(ImGui::DragFloat("Border Thickness", &editLayer.edgeThickness, 0.05f, 0.f, 100.f));

        std::visit(
            PartialOverloaded { [&save](Layer::Square& s) { save(ImGui::DragFloat("Angle", &s.angle, 0.1f, 0.f, 360.f)); },
                                [&save, &editLayer](Layer::Cross& c) {
                                    save(ImGui::DragFloat("Angle", &c.angle, 0.1f, 0.f, 360.f));
                                    save(ImGui::DragFloat(
                                        "Cross Thickness", &c.crossThickness, 0.05f, 1.f, std::min(editLayer.dims.x, editLayer.dims.y)));
                                    if(save(ImGui::Checkbox("Full screen", &c.fullscreen)))
                                        if(!c.fullscreen)
                                            editLayer.dims = vec2(32.f);
                                } },
            editLayer.type);

        if(!std::holds_alternative<Layer::Cross>(editLayer.type) || !std::get<Layer::Cross>(editLayer.type).fullscreen)
            save(ImGui::DragFloat2("Cursor Size", glm::value_ptr(editLayer.dims), 0.2f, 1.f, ImGui::GetIO().DisplaySize.x * 2.f));
    });

    ImGui::Separator();

    ImGuiKeybindInput(activateCursor_, currentEditedKeybind, "Toggles cursor layers visibility.");

    editor_.MaybeSave();
}

void Cursor::DrawLayer(ID3D11DeviceContext* ctx, const Layer& l, const vec2& mousePos, const vec2& targetDims) {
    auto& cb = *cursorCB_;

    // if(l.invert)
    //     ctx->OMSetBlendState(invertBlend_.Get(), nullptr, 0xffffffff);
    // else
    //     ctx->OMSetBlendState(defaultBlend_.Get(), nullptr, 0xffffffff);

    vec2 dims = l.dims;
    if(const auto* c = std::get_if<Layer::Cross>(&l.type); c && c->fullscreen)
        dims = vec2(static_cast<f32>(std::max(targetDims.x, targetDims.y)) * 2.f);

    cb->colorFill = l.colorFill;
    cb->colorBorder = l.colorBorder;
    f32 div = std::min(dims.x, dims.y);
    cb->parameters = vec4(l.edgeThickness * 1.f / div, 0.f, 0.f, 0.f);
    std::visit(PartialOverloaded { [&](const Layer::Cross& c) {
                                      cb->parameters.y = c.crossThickness / div;
                                      cb->parameters.z = c.angle / 180.f * std::numbers::pi_v<f32>;
                                  },
                                   [&](const Layer::Square& s) {
                                       cb->parameters.z = s.angle / 180.f * std::numbers::pi_v<f32>;
                                   } },
               l.type);
    cb->dimensions = vec4(mousePos, dims / targetDims);
    cb.Update(ctx);

    ShaderManager::i().SetConstantBuffers(ctx, cb);
    ShaderManager::i().SetShaders(ctx, cursorVS_, cursorPS_[l.type.index()]);

    DrawScreenQuad(ctx);
}

void Cursor::Load() {
    using namespace nlohmann;
    layers_.clear();

    auto& cfg = JSONConfigurationFile::i();
    cfg.Reload();

    const auto& layers = cfg.json()["cursor_layers"];
    for(const auto& l : layers)
        layers_.push_back(l.get<Layer>());

    editor_.Loaded();
}

void Cursor::Save() {
    using namespace nlohmann;

    auto& cfg = JSONConfigurationFile::i();

    auto& layers = cfg.json()["cursor_layers"];
    layers = json::array();
    for(const auto& l : layers_)
        layers.push_back(l);

    cfg.Save();
}
} // namespace GW2Clarity
