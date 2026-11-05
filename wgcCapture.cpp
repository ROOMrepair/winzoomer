#include "wgcCapture.h"

#include <corecrt.h>
#include <handleapi.h>
#include <roapi.h>
#include <synchapi.h>
#include <windef.h>
#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <new>
#include <winerror.h>
#include <winstring.h>

#include <windows.ui.h>
#include <windows.foundation.collections.h>

class WindowIter final : public WindowIterator {
 public:
  explicit WindowIter(HWND hwnd)
      : id{static_cast<UINT64>(reinterpret_cast<UINT_PTR>(hwnd))} {}

  // 这里表示可以通过三种接口访问
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;

    if (iid == __uuidof(IUnknown)) {
      *out = static_cast<IUnknown*>(this);
    } else if (iid == __uuidof(IInspectable)) {
      *out = static_cast<IInspectable*>(this);
    } else if (iid == __uuidof(WindowIterator)) {
      *out = static_cast<WindowIterator*>(this);
    } else {
      return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&refs);
  }

  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = InterlockedDecrement(&refs);
    if (!count) delete this;
    return count;
  }

  HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) override {
    if (!count || !iids) return E_POINTER;
    *count = 0;
    *iids = nullptr;

    auto* result = static_cast<IID*>(CoTaskMemAlloc(sizeof(IID)));
    if (!result) return E_OUTOFMEMORY;

    result[0] = __uuidof(WindowIterator);
    *iids = result;
    *count = 1;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* s) override {
    *s = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* t) override {
    *t = BaseTrust;
    return S_OK;
  }

  virtual /* propget */ HRESULT STDMETHODCALLTYPE
  get_Current(WindowId* value) override {
    if (!value) return E_POINTER;
    if (!current) return static_cast<HRESULT>(0x8000000B);
    *value = id;
    return S_OK;
  }

  virtual /* propget */ HRESULT STDMETHODCALLTYPE
  get_HasCurrent(_Out_ boolean* hasCurrent) override {
    *hasCurrent = current;
    return S_OK;
  }

  // 当前只有一个窗口
  virtual HRESULT STDMETHODCALLTYPE
  MoveNext(_Out_ boolean* hasCurrent) override {
    current = false;
    *hasCurrent = false;
    return S_OK;
  }

  // 从迭代器当前位置开始，最多取出 capacity 个元素
  // actual 告诉调用方实际取出了几个
  virtual HRESULT STDMETHODCALLTYPE GetMany(_In_ unsigned capacity,
                                            _Out_writes_to_(capacity, *actual)
                                                WindowId* value,
                                            _Out_ unsigned* actual) override {
    *actual = capacity && current ? 1 : 0;
    if (*actual) {
      value[0] = id;
      current = false;
    }
    return S_OK;
  };

 private:
  // 当前只有一个窗口
  WindowId id;
  LONG refs = 1;
  bool current = true;  // 表示当前迭代器是否指向一个有效元素
};

class WindowList final : public WindowIterable {
 public:
  explicit WindowList(HWND hwnd)
      : id{static_cast<UINT64>(reinterpret_cast<UINT_PTR>(hwnd))} {}

  HRESULT STDMETHODCALLTYPE First(WindowIterator** result) override {
    if (!result) return E_POINTER;

    const auto hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(id.Value));
    *result = new (std::nothrow) WindowIter(hwnd);
    return *result ? S_OK : E_OUTOFMEMORY;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refs));
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = static_cast<ULONG>(InterlockedDecrement(&refs));
    if (remaining == 0) {
      delete this;
    }
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) override {
    if (!count || !iids) return E_POINTER;
    *count = 0;
    *iids = nullptr;

    auto* result = static_cast<IID*>(CoTaskMemAlloc(sizeof(IID)));
    if (!result) return E_OUTOFMEMORY;

    result[0] = __uuidof(WindowIterable);
    *iids = result;
    *count = 1;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;

    if (iid == __uuidof(IUnknown)) {
      *out = static_cast<IUnknown*>(this);
    } else if (iid == __uuidof(IInspectable)) {
      *out = static_cast<IInspectable*>(this);
    } else if (iid == __uuidof(WindowIterable)) {
      *out = static_cast<WindowIterable*>(this);
    } else {
      return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) override {
    if (!name) return E_POINTER;
    *name = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* level) override {
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
  }

 private:
  LONG refs = 1;
  WindowId id;
};

inline GUID iid(const wchar_t* text) {
  GUID result{};
  CLSIDFromString(text, &result);
  return result;
}

inline bool factory(const wchar_t* name, const wchar_t* id, void** out) {
  HSTRING text = nullptr;
  if (FAILED(WindowsCreateString(name, static_cast<UINT32>(wcslen(name)),
                                 &text))) {
    return false;
  }
  HRESULT result = RoGetActivationFactory(text, iid(id), out);
  WindowsDeleteString(text);
  if (FAILED(result)) {
    return false;
  }
  return true;
}

static void closeObject(IInspectable* object) {
  if (!object) return;

  ComPtr<Closable> closable;
  HRESULT hr = object->QueryInterface(
      __uuidof(Closable), reinterpret_cast<void**>(closable.GetAddressOf()));

  if (SUCCEEDED(hr)) {
    hr = closable->Close();
  }
  if (FAILED(hr)) {
    OutputDebugStringA("WGC: Close failed\n");
  }
}

static void CloseMonitor(MonitorWGCInfo& info) {
  closeObject(info.session.Get());
  info.cursorSession.Reset();
  info.display.Reset();
  info.session.Reset();

  closeObject(info.pool.Get());
  info.pool.Reset();
  info.item.Reset();

  info.staging.Reset();
  std::vector<unsigned char>{}.swap(info.pixels);
  info.size = {};
  info.configuration = 0;
  info.ready = false;
}

static bool CheckWGC(HRESULT hr, const char* operation) {
  if (SUCCEEDED(hr)) return true;
  char message[192]{};
  std::snprintf(message, sizeof(message), "winzoomer: WGC %s (0x%08lX)\n",
                operation, static_cast<unsigned long>(hr));
  OutputDebugStringA(message);
  std::fputs(message, stderr);
  return false;
}

#define FAILED_WRAPPER(call, operation)               \
  do {                                                \
    if (!CheckWGC((call), (operation))) return false; \
  } while (false)

WGC_Capture::~WGC_Capture() {
  Stop();
  Clean();
}

bool WGC_Capture::Init() {
  FAILED_WRAPPER(RoInitialize(RO_INIT_MULTITHREADED), "RoInitialize failed");
  roInitialized = true;

  if (!_initSources()) return false;
  exclusions.Attach(new (std::nothrow) WindowList(main_handle));
  if (!exclusions.Get())
    return CheckWGC(E_OUTOFMEMORY, "create exclusion list");
  monitorInfo.resize(selector->monitors.size());

  return true;
}

bool WGC_Capture::_initSources() {
  FAILED_WRAPPER(
      D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                        D3D11_SDK_VERSION, device.ReleaseAndGetAddressOf(),
                        nullptr, context.ReleaseAndGetAddressOf()),
      "D3D11CreateDevice failed");

  ComPtr<IDXGIDevice> dxgi;
  FAILED_WRAPPER(device->QueryInterface(
                     __uuidof(IDXGIDevice),
                     reinterpret_cast<void**>(dxgi.ReleaseAndGetAddressOf())),
                 "query idxgidevice failed");

  FARPROC address = GetProcAddress(GetModuleHandleW(L"d3d11.dll"),
                                   "CreateDirect3D11DeviceFromDXGIDevice");

  // 这个版本没有 windows.graphics.directx.direct3d11.interop.h
  // 和 CreateDirect3D11DeviceFromDXGIDevice 声明
  using Wrap = HRESULT(WINAPI*)(IDXGIDevice*, IInspectable**);
  Wrap wrap = nullptr;
  std::memcpy(&wrap, &address, sizeof(wrap));
  if (!wrap) {
    return CheckWGC(E_NOINTERFACE, "device wrapping function unavailable");
  }

  FAILED_WRAPPER(wrap(dxgi.Get(), runtimeDevice.ReleaseAndGetAddressOf()),
                 "call wrap failed");

  //? IGraphicsCaptureItemInterop 为 Windows.Graphics.Capture 的操作接口
  //? Windows.Graphics.Capture.GraphicsCaptureItem 是一个runtimeclass
  //? 需要通过 RoGetActivationFactory 构建
  //? 头文件没有这个类型，这里只能手写uuid
  if (!factory(L"Windows.Graphics.Capture.GraphicsCaptureItem",
               L"{3628e81b-3cac-4c60-b7f4-23ce0e0c3356}",
               reinterpret_cast<void**>(interop.ReleaseAndGetAddressOf()))) {
    return CheckWGC(E_NOINTERFACE, "GraphicsCaptureItem factory unavailable");
  };

  if (!factory(
          L"Windows.Graphics.Capture.Direct3D11CaptureFramePool",
          L"{589b103f-6bbc-5df5-a991-02e28b3b66d5}",
          reinterpret_cast<void**>(poolFactory.ReleaseAndGetAddressOf()))) {
    return CheckWGC(E_NOINTERFACE, "capture frame pool factory unavailable");
  };

  return true;
}

// 事实上Reload可以直接作为对外接口,像之前的RealtimeCapture直接在rebuild内部
// 但也可以加 Request requested, 保存主线程最新一次希望的 WGC, 读取可以之后做,
// reload只在内部做一次
//
// 比如截图线程正在读取屏幕 1，版本是 5。这时你切到屏幕 2，主线程把 requested
// 更新为版本 6 并唤醒线程。屏幕 1 的读回可能还会完成，但发布前发现 5 !=
// 6，就会丢掉这张旧帧。下一轮再按版本 6 加载屏幕 2。
bool WGC_Capture::Reload(size_t selected) {
  if (selector->monitors.size() != monitorInfo.size() ||
      selector->monitors.size() != selector->monitor_infos.size()) {
    return false;
  }

  const size_t count = selector->monitors.size();

  const bool full = selected >= count;
  std::vector<MonitorWGCInfo> pending(count);

  for (size_t i = 0; i < count; ++i) {
    const bool wanted = full || i == selected;
    if (wanted && !monitorInfo[i].session.Get()) {
      if (!_loadMonitor(i, pending[i])) {
        for (auto& info : pending) CloseMonitor(info);
        return false;
      }
    }
  }

  for (size_t i = 0; i < count; ++i) {
    const bool wanted = full || i == selected;
    if (!wanted) {
      CloseMonitor(monitorInfo[i]);
    } else if (pending[i].session.Get()) {
      monitorInfo[i] = std::move(pending[i]);
    }
  }

  return true;
}

// 重新开启 session 捕获
bool WGC_Capture::_loadMonitor(size_t i, MonitorWGCInfo& wgcInfo) {
  FAILED_WRAPPER(
      interop->CreateForMonitor(
          selector->monitors[i], iid(L"{79c3f95b-31f7-4ec2-a464-632ef5d30760}"),
          reinterpret_cast<void**>(wgcInfo.item.ReleaseAndGetAddressOf())),
      "CreateForMonitor failed");
  //? rcMonitor size is same with GetSize()
  FAILED_WRAPPER(wgcInfo.item->GetSize(&wgcInfo.size), "item get size");
  const RECT& rect = selector->monitor_infos[i].rcMonitor;
  const ScreenRect& desktop = selector->virtual_screen_rect;
  if (wgcInfo.size.width <= 0 || wgcInfo.size.height <= 0 ||
      wgcInfo.size.width != rect.right - rect.left ||
      wgcInfo.size.height != rect.bottom - rect.top ||
      rect.left < desktop.Left || rect.top < desktop.Top ||
      rect.right > desktop.Left + desktop.Width ||
      rect.bottom > desktop.Top + desktop.Height) {
    return CheckWGC(E_FAIL, "monitor bounds mismatch");
  }
  try {
    //! 一处缓存
    wgcInfo.pixels.resize(static_cast<size_t>(wgcInfo.size.width) *
                          wgcInfo.size.height * 4);
  } catch (const std::exception&) {
    return CheckWGC(E_OUTOFMEMORY, "allocate monitor pixels");
  }

  FAILED_WRAPPER(poolFactory->CreateFreeThreaded(
                     runtimeDevice.Get(), 87 /* B8G8R8A8 */, 2, wgcInfo.size,
                     wgcInfo.pool.ReleaseAndGetAddressOf()),
                 "CreateFreeThreaded");
  FAILED_WRAPPER(
      wgcInfo.pool->CreateSession(wgcInfo.item.Get(),
                                  wgcInfo.session.ReleaseAndGetAddressOf()),
      "CreateCaptureSession");

  FAILED_WRAPPER(
      wgcInfo.session->QueryInterface(
          iid(L"{bb91f61b-218a-587d-8580-2701a74c0525}"),
          reinterpret_cast<void**>(wgcInfo.display.ReleaseAndGetAddressOf())),
      "QueryInterface display failed");

  FAILED_WRAPPER(
      wgcInfo.display->Exclude(exclusions.Get(), &wgcInfo.configuration),
      "SetWindowExclusionList failed");

  FAILED_WRAPPER(wgcInfo.session->QueryInterface(
                     iid(L"{2c39ae40-7d2e-5044-804e-8b6799d4cf9e}"),
                     reinterpret_cast<void**>(
                         wgcInfo.cursorSession.ReleaseAndGetAddressOf())),
                 "QueryInterface cursorSession failed");

  FAILED_WRAPPER(wgcInfo.cursorSession->SetCursorCaptureEnabled(false),

                 "Disable captured cursor failed");
  FAILED_WRAPPER(wgcInfo.session->StartCapture(), "StartCapture failed");

  return true;
}

#undef FAILED_WRAPPER

struct FrameLease {
  ComPtr<Frame> frame;
  ~FrameLease() { closeObject(frame.Get()); }
};

HRESULT WGC_Capture::ReadMonitor(MonitorWGCInfo& info) {
  static const GUID frame3Id = iid(L"{71616dc8-fea5-5741-a3d8-591acc39a9ee}");
  static const GUID surfaceId = iid(L"{a9b3d012-3df2-4ee3-b8d1-8695f457d3c1}");

  FrameLease latest;

  // session 启动捕获，windows 生成图像并交给 Pool, pool 可以用pool next
  // 取样下一帧 next 对应 TryGetNextFrame 2 对应 CreateFreeThreaded 的容量
  // 表示每块显示器的帧池有两份缓冲空间
  // 这里的2只是尝试取两次，实际可以任意，但是不一定成功
  for (int queued = 0; queued < 2; ++queued) {
    FrameLease next;
    HRESULT hr = info.pool->Next(next.frame.GetAddressOf());
    if (FAILED(hr)) return hr;
    if (!next.frame.Get()) break;  // 取帧可能失败，如果为空返回

    // frame3 获取帧配置
    ComPtr<Frame3> version;
    hr = next.frame->QueryInterface(
        frame3Id, reinterpret_cast<void**>(version.GetAddressOf()));
    if (FAILED(hr)) return hr;
    UINT64 iteration = 0;
    hr = version->ConfigurationIteration(
        &iteration);  // 读取生成这张帧时采用的配置版本
    if (FAILED(hr)) return hr;

    // `info.configuration` 是之前调用 Exclude(..., &info.configuration)
    // 时得到的版本。同一配置下生成的多张帧可以有相同的版本号
    // 每次 exclude 会返回一个配置版本
    if (iteration >= info.configuration) latest.frame.Swap(next.frame);
  }
  if (!latest.frame.Get()) return S_FALSE;

  CaptureSizeInt32 size{};
  HRESULT hr = latest.frame->ContentSize(&size);
  if (FAILED(hr)) return hr;
  if (size.width != info.size.width || size.height != info.size.height) {
    return E_FAIL;
  }

  // 得到这张帧的 WinRT 图像surface。正式返回类型是 IDirect3DSurface
  ComPtr<IInspectable> surface;
  hr = latest.frame->Surface(surface.GetAddressOf());
  if (FAILED(hr)) return hr;

  // 通过 WinRT surface 获取访问底层资源的互操作接口 access
  ComPtr<SurfaceAccess> access;
  hr = surface->QueryInterface(surfaceId,
                               reinterpret_cast<void**>(access.GetAddressOf()));
  if (FAILED(hr)) return hr;

  // 最终为了拿到 texture
  ComPtr<ID3D11Texture2D> texture;
  hr = access->GetInterface(__uuidof(ID3D11Texture2D),
                            reinterpret_cast<void**>(texture.GetAddressOf()));
  if (FAILED(hr)) return hr;
  // 这里 texture 是 WGC 帧里的源纹理

  // 创建中转纹理
  if (!info.staging.Get()) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    hr = device->CreateTexture2D(&desc, nullptr, info.staging.GetAddressOf());
    if (FAILED(hr)) return hr;
  }

  context->CopyResource(info.staging.Get(), texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  hr = context->Map(info.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) return hr;

  // OpenGL's existing screenshot quad expects the bottom row first.
  const size_t rowBytes = static_cast<size_t>(size.width) * 4;
  for (int y = 0; y < size.height; ++y) {
    std::memcpy(info.pixels.data() + (size.height - 1 - y) * rowBytes,
                static_cast<const unsigned char*>(mapped.pData) +
                    static_cast<size_t>(y) * mapped.RowPitch,
                rowBytes);
  }
  context->Unmap(info.staging.Get(), 0);
  info.ready = true;
  return S_OK;
}

HRESULT WGC_Capture::Capture(size_t selected, bool firstFrame,
                             WGCCapturedFrame& output) {
  const bool full = selected >= monitorInfo.size();
  bool changed = firstFrame;
  bool allReady = true;
  for (size_t i = 0; i < monitorInfo.size(); ++i) {
    if (!full && i != selected) continue;
    auto& info = monitorInfo[i];
    // 通过 pool 读到 pixel
    const HRESULT hr = ReadMonitor(info);
    if (FAILED(hr)) return hr;
    changed |= hr == S_OK;
    allReady &= info.ready;
  }
  if (!allReady || !changed) return S_FALSE;

  if (!full) {
    const auto& info = monitorInfo[selected];
    const RECT& rect = selector->monitor_infos[selected].rcMonitor;
    output.rect = {static_cast<int>(rect.left), static_cast<int>(rect.top),
                   info.size.width, info.size.height};
    // 进行一次cpu 复制
    output.pixels = info.pixels;
    return S_OK;
  }

  output.rect = selector->virtual_screen_rect;
  const size_t rowBytes = static_cast<size_t>(output.rect.Width) * 4;
  output.pixels.assign(rowBytes * output.rect.Height, 0);
  for (size_t i = 0; i < monitorInfo.size(); ++i) {
    const auto& info = monitorInfo[i];
    const RECT& rect = selector->monitor_infos[i].rcMonitor;
    const size_t x = static_cast<size_t>(rect.left - output.rect.Left) * 4;
    const size_t y = output.rect.Height - (rect.bottom - output.rect.Top);
    const size_t sourceRowBytes = static_cast<size_t>(info.size.width) * 4;
    // 进行 cpu 复制,复制两块屏
    for (int row = 0; row < info.size.height; ++row) {
      std::memcpy(output.pixels.data() + (y + row) * rowBytes + x,
                  info.pixels.data() + row * sourceRowBytes, sourceRowBytes);
    }
  }
  return S_OK;
}

bool WGC_Capture::Start(HWND handle, MonitorSelector* s) {
  if (worker) return true;
  if (!handle || !s || s->monitors.empty() ||
      s->monitors.size() != s->monitor_infos.size()) {
    return false;
  }
  main_handle = handle;
  selector = s;

  stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  wakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
  if (stopEvent && wakeEvent) {
    worker = CreateThread(NULL, 0, Run, this, 0, NULL);
  }
  if (!worker) {
    Stop();
    return false;
  }
  return true;
}

void WGC_Capture::SetCapture(bool enabled, size_t selected) {
  if (!worker) return;
  {
    std::lock_guard<std::mutex> guard(lock);
    requested = {enabled, selected, requested.generation + 1};
    ready = -1;
    failed = false;
  }
  SetEvent(wakeEvent);
}

// 取 ready 设置为 reading
const WGCCapturedFrame* WGC_Capture::AcquireLatest() {
  std::lock_guard<std::mutex> guard(lock);
  if (ready < 0 || reading >= 0) return nullptr;
  reading = ready;
  ready = -1;
  return &frames[reading];
}

void WGC_Capture::ReleaseFrame(bool retry) {
  std::lock_guard<std::mutex> guard(lock);
  if (reading < 0) return;
  if (frames[reading].generation != requested.generation) {
    frames[reading] = {};
  } else if (retry && ready < 0 && requested.enabled && !failed) {
    // 重试将之前的保留，截图线程也能更新最新的ready = writing
    ready = reading;
  }
  reading = -1;
}

bool WGC_Capture::TakeFailure() {
  std::lock_guard<std::mutex> guard(lock);
  const bool result = failed;
  failed = false;
  return result;
}

// 只有失败的版本仍是当前请求，才设置 failed 并清掉待显示的帧。这样，屏幕 1
// 的旧请求即使迟到报错，也不会把刚切到屏幕 2 的实时模式关掉
void WGC_Capture::ReportFailure(uint64_t generation) {
  std::lock_guard<std::mutex> guard(lock);
  if (generation == requested.generation) {
    ready = -1;
    failed = true;
  }
}

DWORD WINAPI WGC_Capture::Run(void* context) {
  auto& self = *static_cast<WGC_Capture*>(context);
  const HANDLE events[] = {self.stopEvent, self.wakeEvent};
  Request applied;
  bool active = false;
  bool firstFrame = true;

  for (;;) {
    const DWORD wait =
        WaitForMultipleObjects(2, events, FALSE, active ? 16 : INFINITE);
    if (wait == WAIT_OBJECT_0) break;
    if (wait == WAIT_FAILED) {
      std::lock_guard<std::mutex> guard(self.lock);
      self.ready = -1;
      self.failed = true;
      break;
    }

    Request next;
    {
      std::lock_guard<std::mutex> guard(self.lock);
      next = self.requested;
    }

    try {
      //  按一下切换，requested generation ++
      if (next.generation != applied.generation) {
        applied = next;
        {
          std::lock_guard<std::mutex> guard(self.lock);
          for (int i = 0; i < frameSize; ++i) {
            if (i != self.reading) self.frames[i] = {};
          }
        }
        if (!applied.enabled) {
          self.CloseMonitors();
          active = false;
          continue;
        }
        if (!self.roInitialized && !self.Init()) {
          self.Clean();
          self.ReportFailure(applied.generation);
          active = false;
          continue;
        }
        if (!self.Reload(applied.selected)) {
          self.CloseMonitors();
          self.ReportFailure(applied.generation);
          active = false;
          continue;
        }
        active = true;
        firstFrame = true;
      }

      //! ready 缓冲槽已经写完，等主线程来拿
      if (!active) continue;
      int writing = 0;
      {
        std::lock_guard<std::mutex> guard(self.lock);
        while (writing == self.ready || writing == self.reading) ++writing;
      }
      // 内部进行 gpu pool 到 cpu info pixel
      // 同时cpu info pixel 到 cpu frame[writing]
      const HRESULT hr =
          self.Capture(applied.selected, firstFrame, self.frames[writing]);
      if (hr == S_OK) {
        std::lock_guard<std::mutex> guard(self.lock);
        if (applied.generation == self.requested.generation) {
          self.frames[writing].generation = applied.generation;
          self.ready = writing;
          firstFrame = false;
        }
      } else if (FAILED(hr)) {
        CheckWGC(hr, "read captured frame");
        self.CloseMonitors();
        self.ReportFailure(applied.generation);
        active = false;
      }
    } catch (const std::exception& error) {
      OutputDebugStringA("winzoomer: WGC worker failed: ");
      OutputDebugStringA(error.what());
      OutputDebugStringA("\n");
      self.Clean();
      self.ReportFailure(applied.generation);
      active = false;
    }
  }

  self.Clean();
  return 0;
}

// 针对于线程的运行
void WGC_Capture::Stop() {
  if (worker) {
    SetEvent(stopEvent);
    WaitForSingleObject(worker, INFINITE);
    CloseHandle(worker);
    worker = NULL;
  }
  if (stopEvent) CloseHandle(stopEvent);
  if (wakeEvent) CloseHandle(wakeEvent);
  stopEvent = wakeEvent = NULL;
  main_handle = nullptr;
  selector = nullptr;
  std::lock_guard<std::mutex> guard(lock);
  for (auto& frame : frames) frame = {};
  requested = {};
  ready = reading = -1;
  failed = false;
}

// 针对 reload 可以清理 monitorinfo
void WGC_Capture::CloseMonitors() {
  for (auto& monitor : monitorInfo) {
    CloseMonitor(monitor);
  }
}

// 全部资源
void WGC_Capture::Clean() {
  CloseMonitors();
  monitorInfo.clear();

  exclusions.Reset();
  poolFactory.Reset();
  interop.Reset();

  runtimeDevice.Reset();
  context.Reset();
  device.Reset();

  if (roInitialized) {
    RoUninitialize();
    roInitialized = false;
  }
}
