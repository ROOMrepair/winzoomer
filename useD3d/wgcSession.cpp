#include "wgcSession.h"
#include "renderer.h"
#include <winstring.h>
#include <new>
#include <utility>

namespace {
// 原版窗口排除列表实现。独立放在匿名命名空间，不依赖旧捕获线程。
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


struct FrameLease {
  ComPtr<Frame> frame;
  ~FrameLease() { closeObject(frame.Get()); }
};
} // namespace

struct WgcSession::Source {
  ComPtr<Item> item;
  ComPtr<Pool> pool;
  ComPtr<Session> session;
  CaptureSizeInt32 size{};
  RECT desktop{};
  UINT64 configuration = 0;
  ~Source() {
    closeObject(session.Get());
    session.Reset();
    closeObject(pool.Get());
  }
};

WgcSession::WgcSession() = default;
WgcSession::~WgcSession() { Stop(); }
void WgcSession::Stop() { sources.clear(); canvas = {}; }
void WgcSession::Swap(WgcSession& other) {
  sources.swap(other.sources);
  std::swap(canvas, other.canvas);
  lastError.swap(other.lastError);
}

bool WgcSession::Check(HRESULT result, const char* operation) {
  if (SUCCEEDED(result)) return true;
  char text[256]{};
  std::snprintf(text, sizeof(text), "WGC: %s (0x%08lX)\n", operation,
                static_cast<unsigned long>(result));
  lastError = text;
  OutputDebugStringA(text);
  return false;
}

bool WgcSession::Start(WgcDevice& device, HWND overlay, const MonitorSelector& selector) {
  lastError.clear();
  if (!device.RuntimeDevice() || !overlay || selector.monitors.empty() ||
      selector.monitors.size() != selector.monitor_infos.size())
    return Check(E_INVALIDARG, "Invalid capture device or monitor list");
  ComPtr<IGraphicsCaptureItemInterop> interop;
  ComPtr<PoolFactory> poolFactory;
  if (!factory(L"Windows.Graphics.Capture.GraphicsCaptureItem",
               L"{3628e81b-3cac-4c60-b7f4-23ce0e0c3356}",
               reinterpret_cast<void**>(interop.GetAddressOf())) ||
      !factory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool",
               L"{589b103f-6bbc-5df5-a991-02e28b3b66d5}",
               reinterpret_cast<void**>(poolFactory.GetAddressOf())))
    return Check(E_NOINTERFACE, "Create WGC factories");
  ComPtr<WindowIterable> exclusions;
  exclusions.Attach(new (std::nothrow) WindowList(overlay));
  if (!exclusions) return Check(E_OUTOFMEMORY, "Create exclusion list");
  const ScreenRect nextCanvas = selector.selected_screen_rect;
  if (nextCanvas.Width <= 0 || nextCanvas.Height <= 0)
    return Check(E_INVALIDARG, "Invalid capture canvas");
  std::vector<std::unique_ptr<Source>> pending;
  const bool full = selector.selected_monitor >= selector.monitors.size();
  for (size_t i = 0; i < selector.monitors.size(); ++i) {
    if (!full && i != selector.selected_monitor) continue;
    auto source = std::make_unique<Source>();
    source->desktop = selector.monitor_infos[i].rcMonitor;
    if (!Check(interop->CreateForMonitor(selector.monitors[i],
               iid(L"{79c3f95b-31f7-4ec2-a464-632ef5d30760}"),
               reinterpret_cast<void**>(source->item.GetAddressOf())), "Create monitor item") ||
        !Check(source->item->GetSize(&source->size), "Get capture size")) return false;
    const RECT& r = source->desktop;
    if (source->size.width != r.right - r.left || source->size.height != r.bottom - r.top ||
        source->size.width <= 0 || source->size.height <= 0 ||
        r.left < nextCanvas.Left || r.top < nextCanvas.Top ||
        r.right > nextCanvas.Left + nextCanvas.Width ||
        r.bottom > nextCanvas.Top + nextCanvas.Height)
      return Check(E_FAIL, "Monitor layout changed; restart the application");
    if (!Check(poolFactory->CreateFreeThreaded(device.RuntimeDevice(),
               87 /* BGRA8 */, 2, source->size, source->pool.GetAddressOf()), "Create frame pool") ||
        !Check(source->pool->CreateSession(source->item.Get(), source->session.GetAddressOf()),
               "Create capture session")) return false;
    ComPtr<DisplaySession> display;
    ComPtr<CursorSession> cursor;
    if (!Check(source->session->QueryInterface(
               iid(L"{bb91f61b-218a-587d-8580-2701a74c0525}"),
               reinterpret_cast<void**>(display.GetAddressOf())), "Query window exclusion") ||
        !Check(display->Exclude(exclusions.Get(), &source->configuration), "Exclude overlay") ||
        !Check(source->session->QueryInterface(
               iid(L"{2c39ae40-7d2e-5044-804e-8b6799d4cf9e}"),
               reinterpret_cast<void**>(cursor.GetAddressOf())), "Query cursor capture") ||
        !Check(cursor->SetCursorCaptureEnabled(false), "Disable captured cursor") ||
        !Check(source->session->StartCapture(), "Start capture")) return false;
    pending.push_back(std::move(source));
  }
  // 所有新 session 成功后才替换旧状态，失败时保留原捕获。
  sources.swap(pending);
  canvas = nextCanvas;
  return true;
}

bool WgcSession::Update(D3dRenderer& renderer) {
  for (auto& source : sources) {
    FrameLease latest;
    for (int queued = 0; queued < 2; ++queued) {
      FrameLease next;
      if (!Check(source->pool->Next(next.frame.GetAddressOf()), "Get capture frame")) return false;
      if (!next.frame) break;
      ComPtr<Frame3> version;
      UINT64 iteration = 0;
      if (!Check(next.frame->QueryInterface(iid(L"{71616dc8-fea5-5741-a3d8-591acc39a9ee}"),
                 reinterpret_cast<void**>(version.GetAddressOf())), "Query frame configuration") ||
          !Check(version->ConfigurationIteration(&iteration), "Read frame configuration")) return false;
      if (iteration >= source->configuration) latest.frame.Swap(next.frame);
    }
    if (!latest.frame) continue; // 没有新帧就保留原纹理内容。
    CaptureSizeInt32 size{};
    if (!Check(latest.frame->ContentSize(&size), "Read frame size")) return false;
    if (size.width != source->size.width || size.height != source->size.height)
      return Check(E_FAIL, "Capture size changed; restart the application");
    ComPtr<IInspectable> surface;
    ComPtr<SurfaceAccess> access;
    ComPtr<ID3D11Texture2D> texture;
    if (!Check(latest.frame->Surface(surface.GetAddressOf()), "Get frame surface") ||
        !Check(surface->QueryInterface(iid(L"{a9b3d012-3df2-4ee3-b8d1-8695f457d3c1}"),
                 reinterpret_cast<void**>(access.GetAddressOf())), "Query surface access") ||
        !Check(access->GetInterface(__uuidof(ID3D11Texture2D),
                 reinterpret_cast<void**>(texture.GetAddressOf())), "Get capture texture")) return false;
    if (!renderer.CopyScreenRegion(texture.Get(), source->desktop.left - canvas.Left,
            source->desktop.top - canvas.Top, size.width, size.height)) {
      lastError = renderer.LastError();
      return false;
    }
    // 先提交 GPU 复制，再由 FrameLease 关闭帧；后续绘制读取自有纹理。
  }
  return true;
}
