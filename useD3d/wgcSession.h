#pragma once

// 复用原版的 WGC ABI 声明和 MonitorSelector，不实例化 CPU 捕获类。
#include "../wgcCapture.h"
#include "wgcDevice.h"
#include <memory>
#include <string>

class D3dRenderer;

// 所有方法在主线程调用；不保存 CPU 像素，也不使用独立工作线程。
class WgcSession {
 public:
  WgcSession();
  ~WgcSession();
  WgcSession(const WgcSession&) = delete;
  WgcSession& operator=(const WgcSession&) = delete;
  bool Start(WgcDevice& device, HWND overlay, const MonitorSelector& selector);
  bool Update(D3dRenderer& renderer);
  void Stop();
  void Swap(WgcSession& other);
  const std::string& LastError() const { return lastError; }

 private:
  struct Source;
  std::vector<std::unique_ptr<Source>> sources;
  ScreenRect canvas{};
  std::string lastError;
  bool Check(HRESULT result, const char* operation);
};
