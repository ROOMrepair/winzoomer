#pragma once

#include <windef.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <vector>
#include <cstdint>
#include <mutex>

#include "monitorSelector.h"

#include <windows.graphics.capture.h>

using Microsoft::WRL::ComPtr;
using Closable = ABI::Windows::Foundation::IClosable;
using Session = ABI::Windows::Graphics::Capture::IGraphicsCaptureSession;

using WindowId = ABI::Windows::UI::WindowId;
using WindowIterable =
    ABI::Windows::Foundation::Collections::IIterable<WindowId>;
using WindowIterator =
    ABI::Windows::Foundation::Collections::IIterator<WindowId>;

// 修补  #include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.h>
namespace ABI::Windows::UI::Composition {
using ABI::Windows::Graphics::DirectX::DirectXAlphaMode;
using ABI::Windows::Graphics::DirectX::DirectXPixelFormat;
}  // namespace ABI::Windows::UI::Composition

#include <windows.graphics.capture.interop.h>

struct CaptureSizeInt32 {
  INT32 width, height;
};

struct Item : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE Name(HSTRING*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetSize(CaptureSizeInt32*) = 0;
};

struct Frame : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE Surface(IInspectable**) = 0;
  virtual HRESULT STDMETHODCALLTYPE Time(INT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE ContentSize(CaptureSizeInt32*) = 0;
};
struct Frame3 : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE ConfigurationIteration(UINT64*) = 0;
};
struct Pool : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE Recreate(IInspectable*, INT32, INT32,
                                             CaptureSizeInt32) = 0;
  virtual HRESULT STDMETHODCALLTYPE Next(Frame**) = 0;
  virtual HRESULT STDMETHODCALLTYPE AddArrived(IUnknown*, INT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE RemoveArrived(INT64) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateSession(Item*, Session** session) = 0;
};
struct PoolFactory : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE CreateFreeThreaded(IInspectable*, INT32,
                                                       INT32, CaptureSizeInt32,
                                                       Pool**) = 0;
};

struct CursorSession : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetCursorCaptureEnabled(boolean*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetCursorCaptureEnabled(boolean) = 0;
};

struct DisplaySession : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE Exclude(IInspectable*, UINT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetExclusions(IInspectable**) = 0;
};

struct SurfaceAccess : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID, void**) = 0;
};

struct MonitorWGCInfo {
  CaptureSizeInt32 size{};
  ComPtr<Item> item{};
  ComPtr<Pool> pool{};
  ComPtr<Session> session{};
  ComPtr<DisplaySession> display{};
  ComPtr<CursorSession> cursorSession{};
  ComPtr<ID3D11Texture2D> staging;  // cpu 可读的中转纹理

  std::vector<unsigned char> pixels;
  UINT64 configuration = 0;
  bool ready = false;
};

struct WGCCapturedFrame {
  ScreenRect rect{};
  std::vector<unsigned char> pixels;
  uint64_t generation = 0;
};

//? 主线程最新一次希望 WGC 执行的设置
struct Request {
  bool enabled = false;     // 是否开启捕获
  size_t selected = 0;      // 显示器索引
  uint64_t generation = 0;  // 请求版本,每次setcapture + 1
};

class WGC_Capture {
 public:
  ~WGC_Capture();

  bool Start(HWND handle, MonitorSelector* s);
  void SetCapture(bool enabled, size_t selected);
  void Stop();
  const WGCCapturedFrame* AcquireLatest();
  void ReleaseFrame(bool retry = false);
  bool TakeFailure();

 private:
  static DWORD WINAPI Run(void* context);
  bool Init();
  void Clean();
  void CloseMonitors();
  bool Reload(size_t selected);
  bool _initSources();
  bool _loadMonitor(size_t i, MonitorWGCInfo& wgcInfo);

  HRESULT ReadMonitor(MonitorWGCInfo& info);
  HRESULT Capture(size_t selected, bool firstFrame, WGCCapturedFrame& frame);
  void ReportFailure(uint64_t generation);

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IInspectable> runtimeDevice;

  ComPtr<IGraphicsCaptureItemInterop> interop;
  ComPtr<WindowIterable> exclusions;
  ComPtr<PoolFactory> poolFactory;

  // same size with selector monitor
  std::vector<MonitorWGCInfo> monitorInfo;
  MonitorSelector* selector = nullptr;
  HWND main_handle = nullptr;

  bool roInitialized = false;
  HANDLE worker = NULL;

  HANDLE stopEvent = NULL;
  HANDLE wakeEvent = NULL;

  constexpr static int frameSize = 2;
  Request requested;
  WGCCapturedFrame frames[2];
  std::mutex lock;
  int ready = -1;
  int reading = -1;
  bool failed = false;
};
