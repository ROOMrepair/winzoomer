#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#ifdef FREETYPE
#include <array>
#endif

using Microsoft::WRL::ComPtr;

class D3dRenderer {
 public:
  D3dRenderer() = default;
  ~D3dRenderer();
  D3dRenderer(const D3dRenderer&) = delete;
  D3dRenderer& operator=(const D3dRenderer&) = delete;

  // 所有方法在同一个渲染线程调用。窗口尺寸暂时固定，不处理 resize。
  bool Initialize(HWND window, int width, int height);
  void BeginFrame();
  HRESULT EndFrame();
  void Shutdown();

  // 紧密排列的 BGRA8，第一行是图片顶部；GDI bottom-up 数据需先翻转行。
  bool SetScreenImage(int width, int height, const unsigned char* topDownBGRA);
  // 实时路径：同一 device 上的 GPU 局部复制，坐标以图片左上角为原点。
  bool CopyScreenRegion(ID3D11Texture2D* source, int x, int y, int width, int height);
  // 仅在冻结时读回，写入现有 GDI bottom-up 缓存；不包含文字或光标。
  bool ReadScreenImage(unsigned char* bottomUpBGRA, int width, int height);

  // x/y 是图片左上角相对虚拟桌面窗口的坐标。
  // cameraX/Y 与主版本 camera.position 一致；scale=1 表示原始尺寸。
  // mouseX/Y 是窗口客户区坐标；手电筒显示半径为 radius * scale。
  // shadow 直接使用 flashLight.shadow，保留关闭时的渐隐。
  bool DrawScreen(float x, float y, float cameraX = 0, float cameraY = 0,
                  float scale = 1, float mouseX = 0, float mouseY = 0,
                  float radius = 0, float shadow = 0);

  // 复用原版两个三角形，窗口客户区坐标，固定 30 像素白色光标。
  bool DrawCursor(float x, float y);

#ifdef FREETYPE
  bool InitializeText();
  // 和原 RenderText 一样，y 是距窗口底部的文字基线，颜色范围 0..1。
  bool DrawText(const wchar_t* text, float x, float y,
                float r, float g, float b);
#endif

  // 借用指针：后续 WGC 使用同一个 device，调用者不能直接 Release。
  ID3D11Device* Device() const;
  const std::string& LastError() const { return lastError; }

 private:
  bool Check(HRESULT result, const char* operation);
  bool CreateScreenPipeline();
  void BindScreenPipeline();
  std::string lastError;

  // 创建资源的入口。用于创建纹理、shader、缓冲区等
  ComPtr<ID3D11Device> device;

  // 提交绘图操作的入口。设置当前使用哪些资源，执行清屏、更新数据、复制纹理和绘制命令
  ComPtr<ID3D11DeviceContext> context;

  ComPtr<IDXGISwapChain> swapchain;

  // 交换链后缓冲区的写入视图。绑定后，清屏和绘制结果写到这张窗口画面中
  ComPtr<ID3D11RenderTargetView> renderTarget;

  ComPtr<ID3D11Texture2D> screenTexture;

  // 截图纹理的读取视图。pixel shader 通过它读取 screenTexture 的颜色
  ComPtr<ID3D11ShaderResourceView> screenView;
  ComPtr<ID3D11VertexShader> vertexShader;
  ComPtr<ID3D11PixelShader> pixelShader;

  // 给 shader 传递参数的常量缓冲区。保存图片矩形、窗口大小、相机位置和缩放比例
  ComPtr<ID3D11Buffer> constants;

  // 规定如何采样纹理
  ComPtr<ID3D11SamplerState> sampler;

  // 规定三角形如何生成像素片段
  ComPtr<ID3D11RasterizerState> rasterizer;

#ifdef FREETYPE
  struct Glyph {
    ComPtr<ID3D11ShaderResourceView> view;
    int width = 0, height = 0, bearingX = 0, bearingY = 0;
    float advance = 0;
  };
  std::array<Glyph, 256> glyphs;
  ComPtr<ID3D11BlendState> textBlend;
#endif

  int windowWidth = 0, windowHeight = 0;
  int screenWidth = 0, screenHeight = 0;
};
