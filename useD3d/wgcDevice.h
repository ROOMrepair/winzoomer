#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <roapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>

// 仅包装渲染器的设备，不创建第二个 D3D device、context 或捕获线程。
// Initialize/Shutdown（包括析构）必须在同一个主线程调用。
class WgcDevice {
 public:
  WgcDevice() = default;
  ~WgcDevice() { Shutdown(); }
  WgcDevice(const WgcDevice&) = delete;
  WgcDevice& operator=(const WgcDevice&) = delete;

  bool Initialize(ID3D11Device* renderDevice) {
    lastError.clear();
    if (device.Get() == renderDevice && runtimeDevice) return true;
    Shutdown();
    if (!renderDevice) return Check(E_INVALIDARG, "Missing renderer device");

    if (!Check(RoInitialize(RO_INIT_MULTITHREADED), "RoInitialize")) return false;
    roInitialized = true; // S_OK 和 S_FALSE 都需要配对 RoUninitialize。

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    if (!Check(renderDevice->QueryInterface(
            __uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi.GetAddressOf())),
            "Query IDXGIDevice")) {
      Shutdown();
      return false;
    }

    // 沿用原 wgcCapture.cpp 的包装方式，兼容当前 MinGW SDK 缺少的声明。
    const FARPROC address = GetProcAddress(GetModuleHandleW(L"d3d11.dll"),
                                          "CreateDirect3D11DeviceFromDXGIDevice");
    using Wrap = HRESULT(WINAPI*)(IDXGIDevice*, IInspectable**);
    Wrap wrap = nullptr;
    static_assert(sizeof(wrap) == sizeof(address), "Function pointer size");
    std::memcpy(&wrap, &address, sizeof(wrap));
    if (!wrap) {
      Check(E_NOINTERFACE, "CreateDirect3D11DeviceFromDXGIDevice unavailable");
      Shutdown();
      return false;
    }
    if (!Check(wrap(dxgi.Get(), runtimeDevice.GetAddressOf()), "Wrap renderer device")) {
      Shutdown();
      return false;
    }
    device = renderDevice; // AddRef：明确持有渲染器的同一个设备。
    return true;
  }

  void Shutdown() {
    runtimeDevice.Reset();
    device.Reset();
    if (roInitialized) {
      RoUninitialize();
      roInitialized = false;
    }
  }

  // 都是借用指针。后续把 RuntimeDevice() 传给 WGC 的 CreateFreeThreaded。
  ID3D11Device* Device() const { return device.Get(); }
  IInspectable* RuntimeDevice() const { return runtimeDevice.Get(); }
  const std::string& LastError() const { return lastError; }

 private:
  bool Check(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return true;
    char message[256]{};
    std::snprintf(message, sizeof(message), "WGC: %s (0x%08lX)\n",
                  operation, static_cast<unsigned long>(result));
    lastError = message;
    OutputDebugStringA(message);
    return false;
  }

  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<IInspectable> runtimeDevice;
  bool roInitialized = false;
  std::string lastError;
};
