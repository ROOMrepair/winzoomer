#include "renderer.h"

#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#ifdef FREETYPE
#include <vector>
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

// 链接时需要 d3d11、dxgi、d3dcompiler；此文件不依赖 OpenGL。
bool D3dRenderer::Check(HRESULT result, const char* operation) {
  if (SUCCEEDED(result)) return true;
  char message[256]{};
  std::snprintf(message, sizeof(message), "D3dRenderer: %s (0x%08lX)\n",
                operation, static_cast<unsigned long>(result));
  OutputDebugStringA(message);
  lastError = message;
  return false;
}

namespace {
struct ScreenConstants {
  float rectangle[4]; // x, y, width, height
  float viewport[2];
  float camera[2];
  float scale;
  float shadow;
  float cursor[2];
  float radius;
  float drawMode; // 0: desktop, 1: text, 2: cursor
  float padding[2];
  float textColor[4];
};
static_assert(sizeof(ScreenConstants) == 80, "HLSL constant buffer layout");

const char screenShader[] = R"(
cbuffer ScreenParameters : register(b0) {
  float4 rectangle;
  float2 viewport;
  float2 camera;
  float scale;
  float shadow;
  float2 cursor;
  float radius;
  float drawMode;
  float2 padding;
  float4 textColor;
};
struct VertexOutput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};
VertexOutput VSMain(uint id : SV_VertexID) {
  // 四个角：左上、右上、左下、右下，组成 triangle strip。
  float2 uv = float2(id & 1, id >> 1);
  float2 center = viewport * 0.5;
  float2 screenPosition = rectangle.xy + uv * rectangle.zw;
  if (drawMode == 2) {
    // 原 RenderCursor 的两个三角形；GL 向上为正，D3D 向下为正。
    static const float2 cursorVertices[6] = {
      float2(0, 0), float2(0, -1), float2(0.27, -0.65184),
      float2(0, 0), float2(0.27, -0.65184), float2(0.707106, -0.707106)
    };
    screenPosition = rectangle.xy + cursorVertices[id] * float2(30, -30);
  } else {
    screenPosition = center + (screenPosition - center - camera) * scale;
  }
  VertexOutput result;
  result.position = float4(screenPosition.x / viewport.x * 2 - 1,
                          1 - screenPosition.y / viewport.y * 2, 0, 1);
  result.uv = uv;
  return result;
}
Texture2D screenImage : register(t0);
SamplerState screenSampler : register(s0);
float4 PSMain(VertexOutput input) : SV_TARGET {
  // 光标不采样纹理，也不参与手电筒暗化。
  if (drawMode == 2) return float4(1, 1, 1, 1);
  float4 sampled = screenImage.Sample(screenSampler, input.uv);
  if (drawMode == 1) return float4(textColor.rgb, sampled.r);
  // SV_POSITION 和鼠标客户区坐标都以左上角为原点，不需要翻转 Y。
  float darkness = length(input.position.xy - cursor) < radius * scale
                    ? 0 : shadow;
  float3 color = sampled.rgb;
  // GDI BI_RGB 的第四字节可能为 0，桌面画面始终按不透明处理。
  return float4(color * (1 - darkness), 1);
}
)";
} // namespace

D3dRenderer::~D3dRenderer() { Shutdown(); }

bool D3dRenderer::Initialize(HWND window, int width, int height) {
  Shutdown();
  lastError.clear();
  if (!window || width <= 0 || height <= 0)
    return Check(E_INVALIDARG, "Invalid window or dimensions");

  // device	资源工厂	创建纹理、Shader、缓冲区等
  // context	绘图指令入口	绑定资源、清屏、复制、执行 Draw
  // swapchain	窗口画面的呈现管理器	获取后缓冲区、执行 Present
  DXGI_SWAP_CHAIN_DESC desc{};
  desc.BufferDesc.Width = width;
  desc.BufferDesc.Height = height;
  desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1; // 	每像素一个采样，不启用 MSAA
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2; // 后缓冲区
  desc.OutputWindow = window;
  desc.Windowed = TRUE;
  // 先使用传统 HWND 路径，保持与 layered 穿透窗口的呈现方式一致。
  desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  //! 创建！
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  if (!Check(D3D11CreateDeviceAndSwapChain(
          nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
          D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1, D3D11_SDK_VERSION,
          &desc, swapchain.GetAddressOf(), device.GetAddressOf(), nullptr,
          context.GetAddressOf()), "Create device and swapchain")) {
    Shutdown();
    return false;
  }

  ComPtr<ID3D11Texture2D> backbuffer;
  // ID3D11Texture2D 接口的形式 获取第0个缓冲区,
  // backbuffer 此时和swapchain 引用同一份 GPU
  // 后缓冲本质上就是一张存放最终画面像素的二维 GPU 图像
  if (!Check(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
          reinterpret_cast<void**>(backbuffer.GetAddressOf())), "Get backbuffer") ||
      // 为这张图像创建“绘制输出入口”,允许渲染管线把颜色写入它
      !Check(device->CreateRenderTargetView(backbuffer.Get(), nullptr,
          renderTarget.GetAddressOf()), "Create backbuffer RTV")) {
    Shutdown();
    return false;
  }

  // DXGI 中负责创建交换链、枚举显卡等工作的管理对象
  ComPtr<IDXGIFactory> factory;
  if (SUCCEEDED(swapchain->GetParent(__uuidof(IDXGIFactory),
          reinterpret_cast<void**>(factory.GetAddressOf())))) {
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
  }
  windowWidth = width;
  windowHeight = height;
  return true;
}

void D3dRenderer::BeginFrame() {
  if (!context || !renderTarget) return;
  ID3D11RenderTargetView* target = renderTarget.Get();
  context->OMSetRenderTargets(1, &target, nullptr);
  D3D11_VIEWPORT viewport{};
  viewport.Width = static_cast<float>(windowWidth);
  viewport.Height = static_cast<float>(windowHeight);
  viewport.MaxDepth = 1;
  context->RSSetViewports(1, &viewport);
  const float background[] = {0.1f, 0.1f, 0.1f, 1};
  context->ClearRenderTargetView(target, background);
}

HRESULT D3dRenderer::EndFrame() {
  return swapchain ? swapchain->Present(1, 0) : E_UNEXPECTED;
}

ID3D11Device* D3dRenderer::Device() const { return device.Get(); }

bool D3dRenderer::CreateScreenPipeline() {
  if (sampler) return true;
  ComPtr<ID3DBlob> vs, ps, errors;
  auto compile = [&](const char* entry, const char* profile,
                     ComPtr<ID3DBlob>& code) {
    HRESULT hr = D3DCompile(screenShader, sizeof(screenShader) - 1,
        "screenShader", nullptr, nullptr, entry, profile,
        D3DCOMPILE_ENABLE_STRICTNESS, 0, code.GetAddressOf(),
        errors.ReleaseAndGetAddressOf());
    if (Check(hr, entry)) return true;
    if (errors) {
      const char* details = static_cast<const char*>(errors->GetBufferPointer());
      OutputDebugStringA(details);
      lastError += details;
    }
    return false;
  };
  if (!compile("VSMain", "vs_5_0", vs) ||
      !compile("PSMain", "ps_5_0", ps)) return false;
  if (!Check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(),
          nullptr, vertexShader.ReleaseAndGetAddressOf()), "Create vertex shader") ||
      !Check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(),
          nullptr, pixelShader.ReleaseAndGetAddressOf()), "Create pixel shader"))
    return false;

  D3D11_BUFFER_DESC buffer{};
  buffer.ByteWidth = sizeof(ScreenConstants);
  buffer.Usage = D3D11_USAGE_DEFAULT;
  buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  if (!Check(device->CreateBuffer(&buffer, nullptr,
          constants.ReleaseAndGetAddressOf()), "Create constants")) return false;

  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  if (!Check(device->CreateRasterizerState(&raster,
          rasterizer.ReleaseAndGetAddressOf()), "Create rasterizer")) return false;

  D3D11_SAMPLER_DESC sampling{};
  sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampling.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampling.MaxAnisotropy = 1;
  sampling.MaxLOD = 0; // 前两个阶段只使用一层图片。
  return Check(device->CreateSamplerState(&sampling,
      sampler.ReleaseAndGetAddressOf()), "Create sampler");
}

bool D3dRenderer::SetScreenImage(int width, int height,
                               const unsigned char* topDownBGRA) {
  lastError.clear();
  if (!device || !topDownBGRA || width <= 0 || height <= 0 ||
      width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
      height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
    return Check(E_INVALIDARG, "Invalid device, pixels or image dimensions");
  if (!CreateScreenPipeline()) return false;

  if (width != screenWidth || height != screenHeight || !screenTexture) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = topDownBGRA;
    data.SysMemPitch = static_cast<UINT>(width) * 4;
    ComPtr<ID3D11Texture2D> nextTexture;
    ComPtr<ID3D11ShaderResourceView> nextView;
    if (!Check(device->CreateTexture2D(&desc, &data, nextTexture.GetAddressOf()),
               "Create screen texture") ||
        !Check(device->CreateShaderResourceView(nextTexture.Get(), nullptr,
               nextView.GetAddressOf()), "Create screen SRV")) return false;
    screenTexture = nextTexture;
    screenView = nextView;
    screenWidth = width;
    screenHeight = height;
  } else {
    context->UpdateSubresource(screenTexture.Get(), 0, nullptr,
                              topDownBGRA, static_cast<UINT>(width) * 4, 0);
  }
  return true;
}

bool D3dRenderer::CopyScreenRegion(ID3D11Texture2D* source, int x, int y,
                                  int width, int height) {
  if (!screenTexture || !source || x < 0 || y < 0 || width <= 0 || height <= 0 ||
      width > screenWidth || height > screenHeight ||
      x > screenWidth - width || y > screenHeight - height)
    return Check(E_INVALIDARG, "Invalid capture region");
  D3D11_TEXTURE2D_DESC desc{};
  source->GetDesc(&desc);
  ComPtr<ID3D11Device> owner;
  source->GetDevice(owner.GetAddressOf());
  if (owner.Get() != device.Get() || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
      desc.SampleDesc.Count != 1 || desc.ArraySize != 1 ||
      desc.Width < static_cast<UINT>(width) || desc.Height < static_cast<UINT>(height))
    return Check(E_INVALIDARG, "Capture texture/device mismatch");
  ID3D11ShaderResourceView* empty = nullptr;
  context->PSSetShaderResources(0, 1, &empty);
  const D3D11_BOX box{0, 0, 0, static_cast<UINT>(width), static_cast<UINT>(height), 1};
  context->CopySubresourceRegion(screenTexture.Get(), 0, x, y, 0, source, 0, &box);
  return Check(device->GetDeviceRemovedReason(), "Copy captured texture");
}

bool D3dRenderer::ReadScreenImage(unsigned char* bottomUpBGRA, int width, int height) {
  if (!screenTexture || !bottomUpBGRA || width != screenWidth || height != screenHeight)
    return Check(E_INVALIDARG, "Frozen screenshot dimensions mismatch");
  D3D11_TEXTURE2D_DESC desc{};
  screenTexture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  if (!Check(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()),
             "Create frozen readback texture")) return false;
  context->CopyResource(staging.Get(), screenTexture.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (!Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
             "Read frozen screenshot")) return false;
  const size_t rowBytes = static_cast<size_t>(width) * 4;
  for (int y = 0; y < height; ++y) {
    std::memcpy(bottomUpBGRA + static_cast<size_t>(height - 1 - y) * rowBytes,
                static_cast<const unsigned char*>(mapped.pData) +
                    static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
  }
  context->Unmap(staging.Get(), 0);
  return true;
}

bool D3dRenderer::DrawScreen(float x, float y, float cameraX,
                            float cameraY, float scale, float mouseX,
                            float mouseY, float radius, float shadow) {
  if (!screenView || !std::isfinite(x) || !std::isfinite(y) ||
      !std::isfinite(cameraX) || !std::isfinite(cameraY) ||
      !std::isfinite(scale) || scale <= 0) return false;
  ScreenConstants data{{x, y, static_cast<float>(screenWidth),
                             static_cast<float>(screenHeight)},
                       {static_cast<float>(windowWidth), static_cast<float>(windowHeight)},
                       {cameraX, cameraY}, scale, shadow, {mouseX, mouseY},
                       radius, 0, {0, 0}, {0, 0, 0, 0}};
  context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
  BindScreenPipeline();
  ID3D11ShaderResourceView* image = screenView.Get();
  context->PSSetShaderResources(0, 1, &image);
  context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  context->Draw(4, 0);
  // 解除读绑定，便于下一次更新纹理，也为后续 GPU 复制留出接口。
  image = nullptr;
  context->PSSetShaderResources(0, 1, &image);
  return true;
}

bool D3dRenderer::DrawCursor(float x, float y) {
  if (!constants || !std::isfinite(x) || !std::isfinite(y)) return false;
  ScreenConstants data{};
  data.rectangle[0] = x;
  data.rectangle[1] = y;
  data.viewport[0] = static_cast<float>(windowWidth);
  data.viewport[1] = static_cast<float>(windowHeight);
  data.drawMode = 2;
  context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
  BindScreenPipeline();
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  context->Draw(6, 0);
  return true;
}

// 桌面、字形和光标共用 shader、常量缓冲及管线绑定。
void D3dRenderer::BindScreenPipeline() {
  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  context->VSSetShader(vertexShader.Get(), nullptr, 0);
  context->PSSetShader(pixelShader.Get(), nullptr, 0);
  ID3D11Buffer* buffer = constants.Get();
  context->VSSetConstantBuffers(0, 1, &buffer);
  context->PSSetConstantBuffers(0, 1, &buffer);
  ID3D11SamplerState* sampling = sampler.Get();
  context->PSSetSamplers(0, 1, &sampling);
  context->RSSetState(rasterizer.Get());
}

#ifdef FREETYPE
bool D3dRenderer::InitializeText() {
  lastError.clear();
  if (textBlend) return true;
  if (!device) return Check(E_UNEXPECTED, "Initialize renderer before text");
  if (!CreateScreenPipeline()) return false;

  // 沿用 simpleText.cpp 的字体路径：exe 的上一级目录 / fonts。
  wchar_t pathBuffer[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(nullptr, pathBuffer, MAX_PATH);
  if (!length || length >= MAX_PATH) return Check(E_FAIL, "Get font directory");
  std::wstring path(pathBuffer, length);
  path = path.substr(0, path.find_last_of(L"/\\"));
  path = path.substr(0, path.find_last_of(L"/\\")) + L"\\fonts\\Px437_Acer_VGA_8x8.ttf";
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
                                       nullptr, 0, nullptr, nullptr);
  if (!bytes) return Check(E_FAIL, "Convert font path");
  std::string fontPath(bytes, '\0');
  WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
                      fontPath.data(), bytes, nullptr, nullptr);

  struct Font {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    ~Font() {
      if (face) FT_Done_Face(face);
      if (library) FT_Done_FreeType(library);
    }
  } font;
  if (FT_Init_FreeType(&font.library)) return Check(E_FAIL, "Initialize FreeType");
  if (FT_New_Face(font.library, fontPath.c_str(), 0, &font.face))
    return Check(E_FAIL, "Load fonts/Px437_Acer_VGA_8x8.ttf");
  if (FT_Set_Pixel_Sizes(font.face, 0, 16)) return Check(E_FAIL, "Set font size");

  // 保留原来逐字形加载的方式，补充 HSV 需要的度数符号。
  for (unsigned code = 0; code < glyphs.size(); ++code) {
    if (code >= 128 && code != 0xB0) continue;
    if (FT_Load_Char(font.face, code, FT_LOAD_RENDER))
      return Check(E_FAIL, "Rasterize font glyph");
    const auto& bitmap = font.face->glyph->bitmap;
    Glyph glyph;
    glyph.width = bitmap.width;
    glyph.height = bitmap.rows;
    glyph.bearingX = font.face->glyph->bitmap_left;
    glyph.bearingY = font.face->glyph->bitmap_top;
    glyph.advance = static_cast<float>(font.face->glyph->advance.x >> 6);
    if (glyph.width && glyph.height) {
      if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
        return Check(E_FAIL, "Unsupported glyph bitmap format");
      std::vector<unsigned char> pixels(static_cast<size_t>(glyph.width) * glyph.height);
      for (int y = 0; y < glyph.height; ++y) {
        const int sourceRow = bitmap.pitch >= 0 ? y : glyph.height - 1 - y;
        std::memcpy(pixels.data() + static_cast<size_t>(y) * glyph.width,
                    bitmap.buffer + static_cast<size_t>(sourceRow) * std::abs(bitmap.pitch),
                    glyph.width);
      }
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = glyph.width;
      desc.Height = glyph.height;
      desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
      desc.Format = DXGI_FORMAT_R8_UNORM;
      desc.Usage = D3D11_USAGE_IMMUTABLE;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA data{};
      data.pSysMem = pixels.data();
      data.SysMemPitch = glyph.width;
      ComPtr<ID3D11Texture2D> texture;
      if (!Check(device->CreateTexture2D(&desc, &data, texture.GetAddressOf()), "Create glyph texture") ||
          !Check(device->CreateShaderResourceView(texture.Get(), nullptr,
                 glyph.view.GetAddressOf()), "Create glyph SRV")) return false;
    }
    glyphs[code] = glyph;
  }

  D3D11_BLEND_DESC blend{};
  auto& target = blend.RenderTarget[0];
  target.BlendEnable = TRUE;
  target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
  target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  target.SrcBlendAlpha = D3D11_BLEND_ONE;
  target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  return Check(device->CreateBlendState(&blend, textBlend.GetAddressOf()), "Create text blend");
}

bool D3dRenderer::DrawText(const wchar_t* text, float x, float y,
                           float r, float g, float b) {
  if (!text || !textBlend) return false;
  BindScreenPipeline();
  context->OMSetBlendState(textBlend.Get(), nullptr, 0xffffffff);
  ScreenConstants data{};
  data.viewport[0] = static_cast<float>(windowWidth);
  data.viewport[1] = static_cast<float>(windowHeight);
  data.scale = data.drawMode = 1;
  data.textColor[0] = r; data.textColor[1] = g; data.textColor[2] = b;
  for (; *text; ++text) {
    const unsigned code = static_cast<unsigned>(*text);
    const Glyph& glyph = glyphs[code < 128 || code == 0xB0 ? code : '?'];
    if (glyph.view) {
      data.rectangle[0] = x + glyph.bearingX;
      data.rectangle[1] = windowHeight - y - glyph.bearingY;
      data.rectangle[2] = static_cast<float>(glyph.width);
      data.rectangle[3] = static_cast<float>(glyph.height);
      context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
      ID3D11ShaderResourceView* image = glyph.view.Get();
      context->PSSetShaderResources(0, 1, &image);
      context->Draw(4, 0);
    }
    x += glyph.advance;
  }
  ID3D11ShaderResourceView* empty = nullptr;
  context->PSSetShaderResources(0, 1, &empty);
  context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  return true;
}
#endif

void D3dRenderer::Shutdown() {
  if (context) context->ClearState();
#ifdef FREETYPE
  for (auto& glyph : glyphs) glyph = {};
  textBlend.Reset();
#endif
  screenView.Reset();
  screenTexture.Reset();
  sampler.Reset();
  rasterizer.Reset();
  constants.Reset();
  pixelShader.Reset();
  vertexShader.Reset();
  renderTarget.Reset();
  if (context) context->Flush();
  swapchain.Reset();
  context.Reset();
  device.Reset();
  windowWidth = windowHeight = screenWidth = screenHeight = 0;
}
