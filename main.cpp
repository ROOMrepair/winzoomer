#include <cassert>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <exception>
#include <handleapi.h>
#include <libloaderapi.h>
#include <minwindef.h>
#include <synchapi.h>
#include <tchar.h>
#include <strsafe.h>
#include <windef.h>
#include <windows.h>
#include <windowsx.h>
#include <math.h>
#include <ShellScalingApi.h>

#include "monitorSelector.h"
#include "simpleMath.h"
#include "systemCursor.h"
#include "cursorRecovery.h"
#include "screenCapture.h"
#include "useD3d/renderer.h"
#include "useD3d/wgcDevice.h"
#include "useD3d/wgcSession.h"
#include <winnt.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
// Added to the Windows SDK in the Windows 10 2004 timeframe. Keep the
// numeric value here so the project also builds with older MinGW headers.
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#define QUIT_KEY_ID 1
#define TOGGLE_INPUT_HOTKEY_ID 2
#define QUIT_KEY_ID2 3
#define EXCHANGE_SCREEN 4
#define PAN_KEY_FIRST 5  // 左、上、右、下，连续四个 ID

#define BUF_SIZE 1024
#define REFRESH_TIMER_ID 1

#define VELOCITY_THRESHOLD 15.0
#define INITIAL_FL_DELTA_RADIUS 250.0
#define OverlayViewPortMoveSpeed 0.8
#define wheelScale 0.005
#define scaleFriction 3.0
#define velocity_movement 60.0
#define FPS 60.0
#define miniScale 0.01
#define radiusDeceleration 10.0
#define dragFriction 6.0

const wchar_t WIN_CLASS_NAME[] = _T("winzoomer_wclass0");
const wchar_t MUTEX_NAME[] = _T("winzoomer_mutex0");

typedef struct FlashLight {
  bool isEnabled;
  float shadow;
  float radius;
  float deltaRadius;
  void update(float dt) {
    if (abs(deltaRadius) > 1.0) {
      radius = fmax(0, radius + deltaRadius * dt);
      deltaRadius -= deltaRadius * dt * radiusDeceleration;
    }
    if (isEnabled) {
      shadow = fmin(shadow + 6.0 * dt, 0.8);
    } else {
      shadow = fmax(shadow - 6.0 * dt, 0.0);
    }
  }
} FlashLight;

typedef struct Camera {
  Vec2f position;
  Vec2f velocity;
  Vec2f scalePivot;
  float scale;
  float deltaScale;
  void update(Vec2f winSize, float dt, bool isDragging) {
    if (abs(deltaScale) > 0.1) {
      Vec2f p0 = (scalePivot - winSize * 0.5) / scale;
      scale = fmax(scale + deltaScale * dt, miniScale);
      Vec2f p1 = (scalePivot - winSize * 0.5) / scale;
      position += p0 - p1;

      deltaScale -= deltaScale * dt * scaleFriction;
    }
    if (!isDragging && velocity.length() > VELOCITY_THRESHOLD) {
      position += velocity * dt;
      velocity -= velocity * dt * dragFriction;
    }
  }
} Camera;

HWND overlay;
HWND previousForegroundWindow = NULL;

POINT mouse_pos;
POINT mouse_pos_client;
POINT last_pos_client;
POINT mouse_pos_target;
POINT overlayViewPortCenter;
POINT overlayViewPortDelta;

RECT overlayViewPort{};

float frameInterval;
int virtualLeft, virtualTop, virtualWidth, virtualHeight;

FlashLight flashLight;
Camera camera;

bool isDragging;
bool useRealTime = false;
bool useInputThroughWindow = false;
bool keyboardPanning = false;
POINT keyboardPanMouse{};
Vec2f keyboardPanRemaining;

D3dRenderer renderer;
WgcDevice wgcDevice;
WgcSession realtimeCapture;

unsigned char pixel[4];

ScreenCaptureBuffer singleScreenCapture;  // for frozen image
MonitorSelector monitorSeletor;
ScreenRect displayedScreenRect{};

SystemCursorVisibility systemCursor;
CursorRecoveryWatchdog cursorRecovery;

//& functions
LRESULT CALLBACK WindowProcRouter(HWND hwnd, UINT uMsg, WPARAM wParam,
                                  LPARAM lParam);
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

void RGBtoHSV(int r, int g, int b, float& h, float& s, float& v);
bool ReadScreenPixel(const POINT& desktopPos, unsigned char* rgba);
bool ReadFrozenImagePixel(const POINT& desktopPos, unsigned char* rgba);
bool ReadDesktopPixel(const POINT& desktopPos, unsigned char* rgba);

POINT CaptureToOverlayPoint(const POINT& capturePos);
POINT OverlayToCapturePoint(const POINT& mouseClient);

void SetInputPassthrough(bool enabledInputThrough);
bool IsMouseInputMessage(UINT message);
bool IsUserInputMessage(UINT message);

void ShutdownCursorControl();

void SetRealtimeMode(bool nextModeEnable, MonitorSelector* seletor);
bool ReLoadTextureSize(const ScreenRect& rect, const void* pixels);

bool IsInsideRect(const RECT& rect, const POINT& src);

void RenderScreen_raw();

void UnregisterPanHotkeys() {
  for (int i = 0; i < 4; ++i) UnregisterHotKey(NULL, PAN_KEY_FIRST + i);
}

bool RegisterPanHotkeys() {
  const UINT keys[] = {VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN};
  for (int i = 0; i < 4; ++i) {
    if (!RegisterHotKey(NULL, PAN_KEY_FIRST + i, MOD_CONTROL | MOD_ALT,
                        keys[i])) {
      const DWORD error = GetLastError();
      UnregisterPanHotkeys();
      char message[160]{};
      std::snprintf(message, sizeof(message),
                    "Could not register Ctrl+Alt+arrow shortcuts (error %lu).",
                    error);
      MessageBoxA(overlay, message, "Passthrough shortcut error",
                  MB_OK | MB_ICONERROR);
      return false;
    }
  }
  return true;
}

void ApplyPassthroughPan(WPARAM hotkey) {
  if (!useInputThroughWindow || hotkey < PAN_KEY_FIRST ||
      hotkey >= PAN_KEY_FIRST + 4)
    return;
  const Vec2f directions[] = {{-1, 0}, {0, -1}, {1, 0}, {0, 1}};
  // 每次移动约 48 个显示像素，缩放后仍保持相同的操作步长。
  if (!keyboardPanning) keyboardPanRemaining = Vec2f(0, 0);
  keyboardPanRemaining += directions[hotkey - PAN_KEY_FIRST] *
                          (48.0f / std::max(camera.scale, 0.0001f));
  camera.velocity = Vec2f(0, 0);
  // 鼠标不动时保留手动平移的位置，避免边缘跟随立即把镜头拉回。
  keyboardPanning = GetCursorPos(&keyboardPanMouse) != FALSE;
}

bool RebuildRenderScreen(MonitorSelector* seletor, size_t to_select) {
  if (!seletor) return false;

  const size_t previous = seletor->selected_monitor;
  seletor->SelectMonitor(to_select);

  const ScreenRect rect = seletor->selected_screen_rect;
  ScreenCaptureBuffer next;
  WgcSession nextCapture;
  if (!next.setup(rect.Width, rect.Height) ||
      !next.capture(rect.Left, rect.Top)) {
    next.shutdown();
    seletor->SelectMonitor(previous);
    return false;
  }
  if (useRealTime && !nextCapture.Start(wgcDevice, overlay, *seletor)) {
    OutputDebugStringA(nextCapture.LastError().c_str());
    next.shutdown();
    seletor->SelectMonitor(previous);
    return false;
  }
  if (!ReLoadTextureSize(rect, next.pixels)) {
    next.shutdown();
    seletor->SelectMonitor(previous);
    return false;
  }
  singleScreenCapture.shutdown();
  singleScreenCapture = next;
  displayedScreenRect = rect;
  if (useRealTime) realtimeCapture.Swap(nextCapture);
  return true;
}

void RenderBegin() { renderer.BeginFrame(); }
void RenderEnd() {
  if (FAILED(renderer.EndFrame())) {
    OutputDebugStringA("winzoomer: D3D Present failed\n");
    PostQuitMessage(1);
  }
}

void RenderFrame(float dt, float /*elapsed*/) {
  GetCursorPos(&mouse_pos);

  // mousepos 是实际的桌面坐标
  // mousepostarget 是在overlay上的坐标

  if (useInputThroughWindow) {
    if (mouse_pos.x != keyboardPanMouse.x ||
        mouse_pos.y != keyboardPanMouse.y) {
      keyboardPanning = false;
      keyboardPanRemaining = Vec2f(0, 0);
    }
    // 穿透模式下反过来，映射到 overlay 渲染光标
    mouse_pos_target = mouse_pos;
    ScreenToClient(overlay, &mouse_pos_target);
    mouse_pos_client = CaptureToOverlayPoint(mouse_pos_target);
    overlayViewPortDelta = {mouse_pos_target.x - overlayViewPortCenter.x,
                            mouse_pos_target.y - overlayViewPortCenter.y};
    if (!keyboardPanning && !IsInsideRect(overlayViewPort, mouse_pos_target)) {
      camera.velocity =
          Vec2f(overlayViewPortDelta.x * OverlayViewPortMoveSpeed,
                overlayViewPortDelta.y * OverlayViewPortMoveSpeed);
    }
  } else {
    // 一般情况下将 overlay 映射到实际的桌面
    mouse_pos_client = mouse_pos;
    ScreenToClient(overlay,
                   &mouse_pos_client);  //! overlay is always fullscreen
    mouse_pos_target = OverlayToCapturePoint(mouse_pos_client);

    if (last_pos_client.x != mouse_pos_client.x ||
        last_pos_client.y != mouse_pos_client.y) {
      if (isDragging) {
        //? 放大后偏移移动量减小
        float dx = (last_pos_client.x - mouse_pos_client.x) / camera.scale;
        float dy = (last_pos_client.y - mouse_pos_client.y) / camera.scale;

        camera.position += Vec2f(dx, dy);
        camera.velocity = Vec2f(dx * velocity_movement, dy * velocity_movement);
      }
      last_pos_client.x = mouse_pos_client.x;
      last_pos_client.y = mouse_pos_client.y;
    }
  }
  camera.update(Vec2f(virtualWidth, virtualHeight), dt, isDragging);
  if (useInputThroughWindow && keyboardPanning) {
    // 按实际 dt 缓动，约 0.17 秒完成 95%；剩余量独立于缩放的支点调整。
    const float blend = keyboardPanRemaining.length() * camera.scale < 0.1f
                            ? 1.0f
                            : 1.0f - std::exp(-18.0f * dt);
    const Vec2f step = keyboardPanRemaining * blend;
    camera.position += step;
    keyboardPanRemaining -= step;
  }
  // 使用本帧更新后的相机，与桌面纹理保持同一变换，避免缩放时光标滞后一帧。
  if (useInputThroughWindow)
    mouse_pos_client = CaptureToOverlayPoint(mouse_pos_target);

  POINT viewPortLT = OverlayToCapturePoint(POINT{0, 0});
  POINT viewPortRB =
      OverlayToCapturePoint(POINT{virtualWidth - 1, virtualHeight - 1});

  // mouse_pos_target 已经 ScreenToClient，视口也转换为虚拟桌面局部坐标。
  overlayViewPort.left = viewPortLT.x - virtualLeft;
  overlayViewPort.top = viewPortLT.y - virtualTop;
  overlayViewPort.right = viewPortRB.x - virtualLeft;
  overlayViewPort.bottom = viewPortRB.y - virtualTop;
  overlayViewPortCenter.x =
      (overlayViewPort.left + overlayViewPort.right) * 0.5;
  overlayViewPortCenter.y =
      (overlayViewPort.top + overlayViewPort.bottom) * 0.5;

  flashLight.update(dt);

  if (useRealTime && !realtimeCapture.Update(renderer)) {
    const std::string error = realtimeCapture.LastError();
    SetRealtimeMode(false, &monitorSeletor);
    // 无法读回时不继续使用过期的冻结取色缓存。
    if (useRealTime) {
      realtimeCapture.Stop();
      PostQuitMessage(1);
      return;
    }
    MessageBoxA(overlay, error.c_str(), "WGC capture error",
                MB_OK | MB_ICONERROR);
  }

  RenderBegin();
  RenderScreen_raw();

  if (flashLight.isEnabled) {
    const POINT samplePos =
        useInputThroughWindow ? mouse_pos : mouse_pos_target;
    const bool pixelValid = ReadScreenPixel(samplePos, pixel);
    if (!pixelValid) std::memset(pixel, 0, sizeof(pixel));
#ifdef FREETYPE
    // 沿用原版字体大小、颜色和左下角基线（20、52、84）。
    if (pixelValid) {
      const int r = pixel[0], g = pixel[1], b = pixel[2];
      const float red = r / 255.0f, green = g / 255.0f, blue = b / 255.0f;
      float h, s, v;
      RGBtoHSV(r, g, b, h, s, v);
      wchar_t text[128]{};
      std::swprintf(text, 128, L"RGB: %d %d %d", r, g, b);
      renderer.DrawText(text, 25, 20, red, green, blue);
      std::swprintf(text, 128, L"HEX: #%02X%02X%02X", r, g, b);
      renderer.DrawText(text, 25, 52, red, green, blue);
      std::swprintf(text, 128, L"HSV: (%.0f\u00B0, %.0f%%, %.0f%%)", h, s * 100,
                    v * 100);
      renderer.DrawText(text, 25, 84, red, green, blue);
    }
#endif
  }
  // 与原版一样，最后绘制光标，位置和尺寸不受相机缩放影响。
  if (!renderer.DrawCursor(float(mouse_pos_client.x),
                           float(mouse_pos_client.y))) {
    OutputDebugStringA("winzoomer: D3D cursor draw failed\n");
    PostQuitMessage(1);
  }
  RenderEnd();
}

double NowSeconds() {
  static const double frequency = [] {
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    return double(value.QuadPart);
  }();

  LARGE_INTEGER value;
  QueryPerformanceCounter(&value);

  return double(value.QuadPart) / frequency;
}

HANDLE CreateFrameTimer() {
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_MODIFY_STATE | SYNCHRONIZE);
  if (timer == nullptr) {
    timer = CreateWaitableTimerW(nullptr,
                                 FALSE,  // 自动复位
                                 nullptr);
  }

  return timer;
}

bool ArmOneShotTimer(HANDLE timer, double seconds) {
  LARGE_INTEGER due;

  // 单位为 100ns；负数表示相对等待时间。
  due.QuadPart = -std::max(1LL, (long long)ceil(seconds * 10000000.0));

  return SetWaitableTimer(timer, &due,
                          0,                 // 只触发一次
                          nullptr, nullptr,  // 不使用回调
                          FALSE) != FALSE;
}

/*?
    一般只会调用 winzoomer or winzoomer --cursor_recovery
    --cursor-watchdog 通过内部调用
*/
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR pCmdLine, int nCmdShow) {
  // Recovery must bypass the single-instance mutex and all overlay setup.
  const int recoveryResult = TryRunCursorRecoveryCommand(pCmdLine);
  if (recoveryResult >= 0) return recoveryResult;

  SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
  HANDLE hMutex = ::CreateMutex(NULL, TRUE, MUTEX_NAME);
  if (hMutex != NULL) {
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      HWND handle = FindWindow(WIN_CLASS_NAME, NULL);
      if (handle != NULL) {
        ShowWindow(handle, SW_SHOWNORMAL);
        SetForegroundWindow(handle);
        return 0;
      }
    }
  } else {
    MessageBoxA(NULL, "failed to execuate program", "Error",
                MB_OK | MB_ICONERROR);
    return 1;
  }

  virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
  virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
  virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

  if (!monitorSeletor.InitMonitorSelector(virtualLeft, virtualTop, virtualWidth,
                                          virtualHeight)) {
    MessageBoxA(NULL, "failed to init MonitorSelector ", "Error",
                MB_OK | MB_ICONERROR);
    return 1;
  }
  displayedScreenRect = monitorSeletor.selected_screen_rect;

  overlayViewPort.left = 0;
  overlayViewPort.top = 0;
  overlayViewPort.right = virtualWidth;
  overlayViewPort.bottom = virtualHeight;
  overlayViewPortCenter.x =
      (overlayViewPort.left + overlayViewPort.right) * 0.5;
  overlayViewPortCenter.y =
      (overlayViewPort.top + overlayViewPort.bottom) * 0.5;

  WNDCLASSEX wcex;
  wcex.cbSize = sizeof(wcex);
  wcex.style = CS_HREDRAW | CS_VREDRAW;
  wcex.lpfnWndProc = WindowProcRouter;
  wcex.cbClsExtra = 0;
  wcex.cbWndExtra = 0;
  wcex.hInstance = hInstance;
  wcex.hIcon = LoadIcon(NULL, IDI_APPLICATION);

  // 客户区使用 D3D 自绘光标，WM_SETCURSOR 负责隐藏系统箭头。
  wcex.hCursor = NULL;
  wcex.hbrBackground = (HBRUSH)CreateSolidBrush(RGB(0, 0, 0));
  wcex.lpszMenuName = NULL;
  wcex.lpszClassName = WIN_CLASS_NAME;
  wcex.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

  RegisterClassEx(&wcex);

  RegisterHotKey(NULL, QUIT_KEY_ID, MOD_CONTROL | MOD_SHIFT, VK_F12);
  RegisterHotKey(NULL, TOGGLE_INPUT_HOTKEY_ID, MOD_CONTROL | MOD_SHIFT, VK_F11);
  RegisterHotKey(NULL, EXCHANGE_SCREEN, MOD_CONTROL | MOD_SHIFT, VK_F10);
  RegisterHotKey(NULL, QUIT_KEY_ID2, 0, VK_ESCAPE);

  overlay = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WIN_CLASS_NAME,
                           L"overlay", WS_POPUP, virtualLeft, virtualTop,
                           virtualWidth, virtualHeight, NULL, NULL,
                           GetModuleHandle(NULL), NULL);

  if (overlay == NULL) {
    MessageBoxA(NULL, "fail to create window failed", "Error",
                MB_OK | MB_ICONERROR);
    return 1;
  }

  // Receive mouse data without taking focus or blocking the underlying app.
  // Normal mode continues to use WM_MOUSEWHEEL; only passthrough uses WM_INPUT.
  RAWINPUTDEVICE mouseDevice{0x01, 0x02, RIDEV_INPUTSINK, overlay};
  if (!RegisterRawInputDevices(&mouseDevice, 1, sizeof(mouseDevice))) {
    MessageBoxA(overlay, "Could not register mouse Raw Input.", "Error",
                MB_OK | MB_ICONERROR);
    DestroyWindow(overlay);
    return 1;
  }

  //! 创建overlay 前保存当前窗口状态，但是似乎只有一层？
  previousForegroundWindow = GetForegroundWindow();
  ShowWindow(overlay, nCmdShow);
  SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  SetFocus(overlay);

  // The screenshot source is the composed desktop (GetDC(NULL)). Exclude the
  // overlay from OS-level capture so a future live-capture update cannot feed
  // the rendered zoomed image back into its own texture.
  if (!SetWindowDisplayAffinity(overlay, WDA_EXCLUDEFROMCAPTURE)) {
    OutputDebugStringA(
        "winzoomer: SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed\n");
  }

  camera.scale = 1.0f;
  camera.deltaScale = 0.0f;
  flashLight.radius = 100.0f;
  flashLight.deltaRadius = 0.0f;
  flashLight.isEnabled = false;
  frameInterval = (float)1 / FPS;

  if (!renderer.Initialize(overlay, virtualWidth, virtualHeight)) {
    MessageBoxA(overlay, renderer.LastError().c_str(),
                "D3D initialization error", MB_OK | MB_ICONERROR);
    DestroyWindow(overlay);
    return 1;
  }

  std::string startupError;
  if (!singleScreenCapture.setup(displayedScreenRect.Width,
                                 displayedScreenRect.Height)) {
    startupError = "Failed to create the GDI screenshot buffer.";
  } else if (!singleScreenCapture.capture(displayedScreenRect.Left,
                                          displayedScreenRect.Top)) {
    startupError = "Failed to capture the desktop using GDI.";
  } else if (!ReLoadTextureSize(displayedScreenRect,
                                singleScreenCapture.pixels)) {
    startupError = "Failed to prepare or upload the D3D screenshot.\n";
    startupError += renderer.LastError();
  }
  if (!startupError.empty()) {
    MessageBoxA(overlay, startupError.c_str(), "Error", MB_OK | MB_ICONERROR);
    singleScreenCapture.shutdown();
    renderer.Shutdown();
    DestroyWindow(overlay);
    return 1;
  }

#ifdef FREETYPE
  if (!renderer.InitializeText()) {
    MessageBoxA(overlay, renderer.LastError().c_str(),
                "D3D text initialization error", MB_OK | MB_ICONERROR);
    singleScreenCapture.shutdown();
    renderer.Shutdown();
    DestroyWindow(overlay);
    return 1;
  }
#endif

  GetCursorPos(&mouse_pos);
  mouse_pos_client = mouse_pos;
  ScreenToClient(overlay, &mouse_pos_client);

  last_pos_client = mouse_pos_client;
  mouse_pos_target = OverlayToCapturePoint(mouse_pos_client);

  MSG msg = {};
  bool running = true;
  HANDLE frameTimer = CreateFrameTimer();
  const int waitEventCount = 128;

  double start = NowSeconds();
  double previousFrame = start - frameInterval;
  double nextFrame = start;

  auto quit = [&frameTimer, &running]() {
    ShutdownCursorControl();
    UnregisterHotKey(NULL, QUIT_KEY_ID2);
    UnregisterHotKey(NULL, QUIT_KEY_ID);
    UnregisterHotKey(NULL, EXCHANGE_SCREEN);
    UnregisterHotKey(NULL, TOGGLE_INPUT_HOTKEY_ID);

    singleScreenCapture.shutdown();
    realtimeCapture.Stop();
    wgcDevice.Shutdown();
    renderer.Shutdown();
    CloseHandle(frameTimer);
    DestroyWindow(overlay);

    running = false;
  };

  while (running) {
    for (int i = 0; i < waitEventCount; ++i) {
      if (!PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        break;
      }

      if (msg.message == WM_QUIT) {
        // Restore immediately, before graphics cleanup.
        running = false;
        break;
      }
      if (msg.message == WM_HOTKEY && msg.wParam == QUIT_KEY_ID) {
        PostQuitMessage(0);
        continue;
      }
      if (msg.message == WM_HOTKEY && msg.wParam == QUIT_KEY_ID2) {
        PostQuitMessage(0);
        continue;
      }
      if (msg.message == WM_HOTKEY && msg.wParam == TOGGLE_INPUT_HOTKEY_ID) {
        SetInputPassthrough(!useInputThroughWindow);
        continue;
      }
      if (msg.message == WM_HOTKEY && msg.wParam >= PAN_KEY_FIRST &&
          msg.wParam < PAN_KEY_FIRST + 4) {
        ApplyPassthroughPan(msg.wParam);
        continue;
      }
      if (msg.message == WM_HOTKEY && msg.wParam == EXCHANGE_SCREEN) {
        int sz = monitorSeletor.monitors.size() + 1;
        std::vector<int> nextList(sz, 0);
        for (int i = 0; i < sz; ++i) nextList[i] = i;
        size_t next = (monitorSeletor.selected_monitor + 1) % sz;
        if (!RebuildRenderScreen(&monitorSeletor, next)) {
          MessageBoxA(overlay, "Failed to switch the captured screen.", "Error",
                      MB_OK | MB_ICONERROR);
        }
        continue;
      }
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }

    if (!running) break;

    double now = NowSeconds();
    if (now >= nextFrame) {
      // for the first time  now > start  elapsed = now - previousFrame >
      // frameinterval
      double elapsed = now - previousFrame;
      previousFrame = now;
      float dt = float(std::min(elapsed, 0.1));
      RenderFrame(dt, elapsed);
      nextFrame = now + frameInterval;
      continue;
    }

    double remaining = nextFrame - NowSeconds();
    if (remaining <= 0) continue;

    if (!ArmOneShotTimer(frameTimer, remaining)) {
      running = false;
      break;
    }

    DWORD result = MsgWaitForMultipleObjectsEx(
        1, &frameTimer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

    if (result == WAIT_FAILED) {
      // 记录失败，退出循环，执行正常清理。
      running = false;
      break;
    }
  }

  quit();
  return 0;
}

LRESULT CALLBACK WindowProcRouter(HWND hwnd, UINT uMsg, WPARAM wParam,
                                  LPARAM lParam) {
  if (!useInputThroughWindow || !IsUserInputMessage(uMsg)) {
    return WindowProc(hwnd, uMsg, wParam, lParam);
  }

  if (uMsg == WM_NCHITTEST) {
    return HTTRANSPARENT;
  }
  if (uMsg == WM_MOUSEACTIVATE) {
    return MA_NOACTIVATE;
  }
  if (uMsg == WM_SETCURSOR) {
    SetCursor(NULL);
    return TRUE;
  }
  return 0;
}

// 过滤掉事件, 不触发focus
bool IsMouseInputMessage(UINT message) {
  switch (message) {
    case WM_NCHITTEST:
    case WM_MOUSEACTIVATE:
    case WM_SETCURSOR:
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
      return true;
    default:
      return false;
  }
}

bool IsUserInputMessage(UINT message) {
  if (IsMouseInputMessage(message)) {
    return true;
  }

  switch (message) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
      return true;
    default:
      return false;
  }
}

void SetInputPassthrough(bool enabledInputThrough) {
  if (enabledInputThrough == useInputThroughWindow) return;
  // 捕获失败时仍保持普通窗口和可见系统光标，不进入半启用状态。
  if (enabledInputThrough) {
    SetRealtimeMode(true, &monitorSeletor);
    if (!useRealTime) return;
    if (!RegisterPanHotkeys()) return;
  }
  //! arm 启动子进程
  if (enabledInputThrough && !cursorRecovery.arm()) {
    UnregisterPanHotkeys();
    MessageBoxA(overlay,
                "Could not start the cursor recovery process. Passthrough "
                "mode was not enabled, so the system cursor remains visible.",
                "Cursor recovery", MB_OK | MB_ICONERROR);
    return;
  }
  if (!systemCursor.setHidden(enabledInputThrough)) {
    MessageBoxA(overlay,
                enabledInputThrough
                    ? "Could not hide the system cursor. Passthrough mode "
                      "was not enabled."
                    : "Could not restore the system cursor. The application "
                      "will retry during shutdown.",
                "Magnification API", MB_OK | MB_ICONERROR);
    if (enabledInputThrough) {
      UnregisterPanHotkeys();
      return;
    }
  } else if (!enabledInputThrough) {
    //? 如果 setHidden 成功进入这里,此时不需要重复restore
    cursorRecovery.disarm();
  }

  isDragging = false;
  ReleaseCapture();

  if (enabledInputThrough) {
    HWND foreground = GetForegroundWindow();
    if (foreground != NULL && foreground != overlay) {
      previousForegroundWindow = foreground;
    }
  }

  LONG_PTR style = GetWindowLongPtr(overlay, GWL_EXSTYLE);
  const LONG_PTR previousStyle = style;

  if (enabledInputThrough) {
    // WS_EX_TRANSPARENT alone affects paint ordering on a normal window. The
    // layered-window combination makes the hit-test pass through across
    // processes as well.
    //! 还是需要layered window才能保证鼠标命中事件穿透
    style |= WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
  } else {
    style &= ~(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
  }
  SetLastError(ERROR_SUCCESS);
  const LONG_PTR oldStyle = SetWindowLongPtr(overlay, GWL_EXSTYLE, style);
  bool applied = oldStyle != 0 || GetLastError() == ERROR_SUCCESS;
  if (applied && enabledInputThrough)
    applied = SetLayeredWindowAttributes(overlay, 0, 255, LWA_ALPHA) != FALSE;
  if (applied)
    applied = SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                               SWP_FRAMECHANGED) != FALSE;
  if (!applied) {
    const DWORD error = GetLastError();
    SetWindowLongPtr(overlay, GWL_EXSTYLE, previousStyle);
    SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ShutdownCursorControl();
    char message[128]{};
    std::snprintf(message, sizeof(message),
                  "Could not change passthrough window style (error %lu).",
                  error);
    PostQuitMessage(1);
    MessageBoxA(overlay, message, "Passthrough error", MB_OK | MB_ICONERROR);
    return;
  }

  useInputThroughWindow = enabledInputThrough;
  keyboardPanning = false;
  keyboardPanRemaining = Vec2f(0, 0);
  if (!enabledInputThrough) UnregisterPanHotkeys();
  if (enabledInputThrough) flashLight.isEnabled = false;

  if (enabledInputThrough) {
    if (GetForegroundWindow() == overlay &&
        IsWindow(previousForegroundWindow)) {
      SetForegroundWindow(previousForegroundWindow);
    }
  } else {
    SetForegroundWindow(overlay);
    SetFocus(overlay);

    last_pos_client = mouse_pos_client;
  }
}

void ApplyPassthroughRawWheel(const RAWMOUSE& mouse, bool shiftDown,
                              const POINT& overlayCursor) {
  if (!useInputThroughWindow || !shiftDown ||
      !(mouse.usButtonFlags & RI_MOUSE_WHEEL))
    return;
  const int delta = static_cast<SHORT>(mouse.usButtonData);
  if (delta == 0) return;
  camera.deltaScale += delta * wheelScale;
  // Use the cursor drawn on the transformed overlay, not its unmagnified
  // desktop position, so zoom remains centered on the visible pointer.
  camera.scalePivot = Vec2f(static_cast<float>(overlayCursor.x),
                            static_cast<float>(overlayCursor.y));
}

POINT OverlayToCapturePoint(const POINT& mouseClient) {
  const float s = std::max(camera.scale, 0.0001f);
  Vec2f center(virtualWidth * 0.5f, virtualHeight * 0.5f);
  Vec2f screen(static_cast<float>(mouseClient.x),
               static_cast<float>(mouseClient.y));
  Vec2f capturePos = center + camera.position + (screen - center) / s;

  POINT result{};
  result.x = virtualLeft + static_cast<LONG>(std::lround(capturePos.x));
  result.y = virtualTop + static_cast<LONG>(std::lround(capturePos.y));
  return result;
}

POINT CaptureToOverlayPoint(const POINT& capturePos) {
  const float s = std::max(camera.scale, 0.0001f);

  Vec2f capture(static_cast<float>(capturePos.x),
                static_cast<float>(capturePos.y));
  Vec2f center(virtualWidth * 0.5f, virtualHeight * 0.5f);
  Vec2f screen = center + (capture - center - camera.position) * s;

  POINT result{};
  result.x = static_cast<LONG>(std::lround(screen.x));
  result.y = static_cast<LONG>(std::lround(screen.y));
  return result;
}

bool IsInsideRect(const RECT& rect, const POINT& src) {
  return src.x > rect.left && src.x < rect.right && src.y > rect.top &&
         src.y < rect.bottom;
}
bool CopyStringToClipBoard(const std::string& str) {
  if (!overlay) return false;
  HGLOBAL clipMem = GlobalAlloc(GMEM_MOVEABLE, str.length());
  if (!clipMem) return false;

  void* buffer = GlobalLock(clipMem);
  if (!buffer) {
    GlobalFree(clipMem);
    return false;
  }
  std::memcpy(buffer, str.data(), str.length());
  GlobalUnlock(clipMem);

  if (!OpenClipboard(overlay)) {
    GlobalFree(clipMem);
    return false;
  }
  bool success = false;
  if (EmptyClipboard()) {
    success = SetClipboardData(CF_TEXT, clipMem) != nullptr;
  }

  CloseClipboard();
  if (!success) {
    GlobalFree(clipMem);
    return false;
  }

  return true;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam,
                            LPARAM lParam) {
  switch (uMsg) {
    case WM_INPUT: {
      RAWINPUT input{};
      UINT size = sizeof(input);
      // Only mouse input is registered, so a fixed-size stack buffer suffices.
      if (useInputThroughWindow &&
          GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
                          &input, &size,
                          sizeof(RAWINPUTHEADER)) == sizeof(input) &&
          input.header.dwType == RIM_TYPEMOUSE &&
          (input.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)) {
        POINT cursor{};
        if (GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor)) {
          ApplyPassthroughRawWheel(input.data.mouse,
                                   (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0,
                                   CaptureToOverlayPoint(cursor));
        }
      }
      // Foreground WM_INPUT requires DefWindowProc for OS-side cleanup.
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    case WM_SETCURSOR: {
      if (LOWORD(lParam) == HTCLIENT) {
        SetCursor(nullptr);
        return TRUE;
      }
      break;
    }
    case WM_DESTROY: {
      ShutdownCursorControl();
      PostQuitMessage(0);
      return 0;
    }
    case WM_KEYUP: {
      switch (wParam) {
        case 'F': {
          flashLight.isEnabled = !flashLight.isEnabled;
          break;
        }
        case 'X': {
          if (!flashLight.isEnabled) {
            break;
          }
          int r = pixel[0];
          int g = pixel[1];
          int b = pixel[2];

          int length = std::snprintf(nullptr, 0, "#%02X%02X%02X", r, g, b);
          std::string colorStr(length + 1, '\0');
          std::snprintf(colorStr.data(), colorStr.size(), "#%02X%02X%02X", r, g,
                        b);
          CopyStringToClipBoard(colorStr);
          break;
        }
        case 'C': {
          if (!flashLight.isEnabled) {
            break;
          }
          int r = pixel[0];
          int g = pixel[1];
          int b = pixel[2];

          int length = std::snprintf(nullptr, 0, "%d %d %d", r, g, b);
          std::string colorStr(length + 1, '\0');
          std::snprintf(colorStr.data(), colorStr.size(), "%d %d %d", r, g, b);
          CopyStringToClipBoard(colorStr);
          break;
        }
        case 'R': {
          camera.scale = 1.0f;
          camera.deltaScale = 0.0f;
          camera.position = Vec2f(0.0f, 0.0f);
          camera.velocity = Vec2f(0.0f, 0.0f);

          flashLight.radius = 100.0f;
          flashLight.deltaRadius = 0.0f;
          flashLight.isEnabled = false;
          SetRealtimeMode(false, &monitorSeletor);
          SetFocus(overlay);
          break;
        }
        case 'U': {
          SetRealtimeMode(!useRealTime, &monitorSeletor);
          break;
        }
        default:
          break;
      }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      isDragging = true;
      return 0;
    }
    case WM_LBUTTONUP: {
      isDragging = false;
      return 0;
    }
    case WM_MOUSEWHEEL: {
      auto wheelSpeed = GET_WHEEL_DELTA_WPARAM(wParam);
      auto fwKeys = GET_KEYSTATE_WPARAM(wParam);
      float delta = wheelSpeed * wheelScale;
      if (flashLight.isEnabled && fwKeys & MK_SHIFT) {
        flashLight.deltaRadius +=
            ((wheelSpeed > 0) ? 1 : -1) * INITIAL_FL_DELTA_RADIUS;
        return 0;
      }
      if (flashLight.isEnabled && fwKeys & MK_CONTROL) {
        flashLight.deltaRadius +=
            ((wheelSpeed > 0) ? 1 : -1) * INITIAL_FL_DELTA_RADIUS;
      }
      camera.deltaScale += delta;
      camera.scalePivot = Vec2f((float)mouse_pos.x, (float)mouse_pos.y);
      return 0;
    }
    case WM_TIMER: {
      return 0;
    }
    case WM_ERASEBKGND: {
      return 1;
    }
  }
  return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void ShutdownCursorControl() {
  UnregisterPanHotkeys();
  SetCursor(LoadCursorW(NULL, IDC_ARROW));
  //? shutdown 成功 disarm 清除 arm 标记,不会调用重清理
  if (systemCursor.shutdown()) cursorRecovery.disarm();
}

bool ReadDesktopPixel(const POINT& desktopPos, unsigned char* rgba) {
  HDC screenDC = GetDC(NULL);
  if (screenDC == NULL) {
    return false;
  }

  COLORREF color = GetPixel(screenDC, desktopPos.x, desktopPos.y);
  ReleaseDC(NULL, screenDC);

  if (color == CLR_INVALID) {
    return false;
  }

  rgba[0] = GetRValue(color);
  rgba[1] = GetGValue(color);
  rgba[2] = GetBValue(color);
  rgba[3] = 255;
  return true;
}

bool ReadFrozenImagePixel(const POINT& desktopPos, unsigned char* rgba) {
  if (singleScreenCapture.pixels == nullptr) return false;
  // OverlayToCapturePoint returns absolute desktop coordinates. Convert them
  // to image-local coordinates, including negative multi-monitor origins.
  const long long x =
      static_cast<long long>(desktopPos.x) - displayedScreenRect.Left;
  const long long y =
      static_cast<long long>(desktopPos.y) - displayedScreenRect.Top;
  if (x < 0 || y < 0 || x >= singleScreenCapture.width ||
      y >= singleScreenCapture.height) {
    return false;
  }

  const size_t offset =
      (static_cast<size_t>(singleScreenCapture.height - 1 - y) *
           singleScreenCapture.width +
       static_cast<size_t>(x)) *
      4;

  rgba[0] = singleScreenCapture.pixels[offset + 2];
  rgba[1] = singleScreenCapture.pixels[offset + 1];
  rgba[2] = singleScreenCapture.pixels[offset];
  rgba[3] = 255;  // The fourth byte of a GDI BI_RGB capture is not valid alpha.
  return true;
}

// desktop 指实际的桌面坐标
bool ReadScreenPixel(const POINT& desktopPos, unsigned char* rgba) {
  return useRealTime ? ReadDesktopPixel(desktopPos, rgba)
                     : ReadFrozenImagePixel(desktopPos, rgba);
}

void RGBtoHSV(int r, int g, int b, float& h, float& s, float& v) {
  float rd = r / 255.0;
  float gd = g / 255.0;
  float bd = b / 255.0;

  float maxv = fmax(rd, fmax(gd, bd));
  float minv = fmin(rd, fmin(gd, bd));
  float delta = maxv - minv;

  if (delta < 1e-6)
    h = 0;
  else if (maxv == rd)
    h = fmod(((gd - bd) / delta), 6.0);
  else if (maxv == gd)
    h = ((bd - rd) / delta) + 2.0;
  else
    h = ((rd - gd) / delta) + 4.0;

  h *= 60;
  if (h < 0) h += 360;

  s = (maxv == 0) ? 0 : (delta / maxv);
  v = maxv;
}

// 保留原来的 GDI bottom-up 缓存和取色方式，只在上传时翻转行。
// 启动和切屏共用此函数；临时数组不会常驻，也不会每帧创建。
bool ReLoadTextureSize(const ScreenRect& rect, const void* pixels) {
  if (!pixels || rect.Width <= 0 || rect.Height <= 0 ||
      rect.Width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
      rect.Height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
    return false;
  const size_t rowBytes = static_cast<size_t>(rect.Width) * 4;
  try {
    std::vector<unsigned char> topDown(rowBytes * rect.Height);
    const auto* bottomUp = static_cast<const unsigned char*>(pixels);
    for (int y = 0; y < rect.Height; ++y) {
      std::memcpy(
          topDown.data() + static_cast<size_t>(y) * rowBytes,
          bottomUp + static_cast<size_t>(rect.Height - 1 - y) * rowBytes,
          rowBytes);
    }
    return renderer.SetScreenImage(rect.Width, rect.Height, topDown.data());
  } catch (const std::exception&) {
    OutputDebugStringA(
        "winzoomer: failed to allocate screenshot upload buffer\n");
    return false;
  }
}

void SetRealtimeMode(bool nextModeEnable, MonitorSelector* selector) {
  if (nextModeEnable == useRealTime || !selector) return;
  if (nextModeEnable) {
    if (!wgcDevice.Initialize(renderer.Device())) {
      MessageBoxA(overlay, wgcDevice.LastError().c_str(),
                  "WGC initialization error", MB_OK | MB_ICONERROR);
      return;
    }
    if (!realtimeCapture.Start(wgcDevice, overlay, *selector)) {
      MessageBoxA(overlay, realtimeCapture.LastError().c_str(),
                  "WGC start error", MB_OK | MB_ICONERROR);
      return;
    }
  } else {
    // 冻结或捕获失败时先恢复输入和系统光标，再进行读回或显示错误。
    if (useInputThroughWindow) SetInputPassthrough(false);
    // 冻结的是当前显示的桌面纹理，而不是重新截取桌面；只在此处读回。
    if (!renderer.ReadScreenImage(singleScreenCapture.pixels,
                                  singleScreenCapture.width,
                                  singleScreenCapture.height)) {
      MessageBoxA(overlay, renderer.LastError().c_str(),
                  "Freeze screenshot error", MB_OK | MB_ICONERROR);
      return;
    }
    realtimeCapture.Stop();
  }
  useRealTime = nextModeEnable;
}

void RenderScreen_raw() {
  if (!renderer.DrawScreen(float(displayedScreenRect.Left - virtualLeft),
                           float(displayedScreenRect.Top - virtualTop),
                           camera.position.x, camera.position.y, camera.scale,
                           float(mouse_pos_client.x), float(mouse_pos_client.y),
                           flashLight.radius, flashLight.shadow)) {
    OutputDebugStringA("winzoomer: D3D screen draw failed\n");
    PostQuitMessage(1);
  }
}
