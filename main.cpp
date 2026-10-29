#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
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

#include "simpleMath.h"
#include "simpleText.h"
#include "simpleShader.h"
#include "system_cursor.h"
#include "cursor_recovery.h"

#include <glad/glad.h>
#include <winnt.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
// Added to the Windows SDK in the Windows 10 2004 timeframe. Keep the
// numeric value here so the project also builds with older MinGW headers.
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#define QUIT_KEY_ID 1
#define TOGGLE_INPUT_HOTKEY_ID 2
#define QUIT_KEY_ID2 3

#define BUF_SIZE 1024
#define REFRESH_TIMER_ID 1

#define VELOCITY_THRESHOLD 15.0
#define INITIAL_FL_DELTA_RADIUS 250.0
#define OverlayViewPortMoveSpeed 1.2
#define wheelScale 0.005
#define scaleFriction 3.0
#define velocity_movement 60.0
#define FPS 60.0
#define miniScale 0.01
#define radiusDeceleration 10.0
#define dragFriction 6.0

const wchar_t WIN_CLASS_NAME[] = _T("winzoomer_wcls0");
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

struct ScreenCaptureBuffer {
  int width = 0;
  int height = 0;
  HDC memoryDC = NULL;
  HDC screenDC = NULL;
  HBITMAP bitmap = NULL;
  HGDIOBJ previousBitmap = NULL;
  unsigned char* pixels = nullptr;

  bool initialize(int captureWidth, int captureHeight) {
    width = captureWidth;
    height = captureHeight;

    memoryDC = CreateCompatibleDC(NULL);
    if (memoryDC == NULL) {
      return false;
    }

    screenDC = GetDC(NULL);
    if (screenDC == NULL) {
      shutdown();
      return false;
    }

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    // Keep a bottom-up DIB so the existing OpenGL texture coordinates remain
    // unchanged (the first row in memory is the bottom row of the desktop).
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    //? like GetDIBits(HDC,HBITMAP,...)
    //? 建一张 GDI 能使用的 HBITMAP，同时返回其指针
    bitmap = CreateDIBSection(NULL, &bitmapInfo, DIB_RGB_COLORS,
                              reinterpret_cast<void**>(&pixels), NULL, 0);
    if (bitmap == NULL || pixels == nullptr) {
      shutdown();
      return false;
    }

    //? 用于恢复原状态
    previousBitmap = SelectObject(memoryDC, bitmap);
    if (previousBitmap == NULL || previousBitmap == HGDI_ERROR) {
      previousBitmap = NULL;
      shutdown();
      return false;
    }

    return true;
  }
  /*? how to capture screen
      1. getdc -> create compatiblebitmap of this dc
      2. bitblt from compatiblebitmap
      3. 取 pixels
  */
  bool capture(int sourceX, int sourceY) {
    if (memoryDC == NULL || bitmap == NULL || pixels == nullptr) {
      return false;
    }

    if (screenDC == NULL) {
      return false;
    }

    BOOL copied = BitBlt(memoryDC, 0, 0, width, height, screenDC, sourceX,
                         sourceY, SRCCOPY);
    // Complete GDI writes before publishing or reading the DIB's pixels.
    BOOL flushed = GdiFlush();
    return copied != FALSE && flushed != FALSE;
  }

  void shutdown() {
    if (memoryDC != NULL && previousBitmap != NULL) {
      SelectObject(memoryDC, previousBitmap);
    }
    if (bitmap != NULL) {
      DeleteObject(bitmap);
    }
    if (memoryDC != NULL) {
      DeleteDC(memoryDC);
    }
    if (screenDC != NULL) {
      ReleaseDC(NULL, screenDC);
    }

    width = 0;
    height = 0;
    memoryDC = NULL;
    screenDC = NULL;
    bitmap = NULL;
    previousBitmap = NULL;
    pixels = nullptr;
  }
};

// The worker owns all GDI resources. The UI/OpenGL thread only leases the
// latest completed pixel buffer, so neither BitBlt nor the upload can race it.
struct RealtimeCapture {
  ScreenCaptureBuffer buffers[3];
  CRITICAL_SECTION lock;
  HANDLE worker = NULL;
  HANDLE stopEvent = NULL;
  HANDLE wakeEvent = NULL;
  HANDLE initializedEvent = NULL;
  std::atomic<bool> enabled{false};
  bool initialized = false;
  int ready = -1;
  int reading = -1;
  int width = 0, height = 0, left = 0, top = 0;

  RealtimeCapture() { InitializeCriticalSection(&lock); }
  ~RealtimeCapture() {
    stop();
    DeleteCriticalSection(&lock);
  }

  static DWORD WINAPI run(void* context) {
    auto& self = *static_cast<RealtimeCapture*>(context);
    self.initialized = true;
    for (auto& buffer : self.buffers) {
      if (!buffer.initialize(self.width, self.height)) {
        self.initialized = false;
        break;
      }
    }
    SetEvent(self.initializedEvent);

    HANDLE events[] = {self.stopEvent, self.wakeEvent};
    while (self.initialized &&
           WaitForSingleObject(self.stopEvent, 0) == WAIT_TIMEOUT) {
      if (!self.enabled.load()) {
        if (WaitForMultipleObjects(2, events, FALSE, INFINITE) !=
            WAIT_OBJECT_0 + 1) {
          break;
        }
        continue;
      }

      const DWORD started = GetTickCount();
      int writing = -1;
      EnterCriticalSection(&self.lock);
      //? reading 渲染线程读取中， ready 等待渲染线程读取
      for (int i = 0; i < 3; ++i) {
        if (i != self.reading && i != self.ready) {
          writing = i;
          break;
        }
      }
      LeaveCriticalSection(&self.lock);

      if (writing >= 0 && self.buffers[writing].capture(self.left, self.top)) {
        EnterCriticalSection(&self.lock);
        if (self.enabled.load()) {
          // Drop the previous ready frame, never queue stale frames.
          self.ready = writing;
        }
        LeaveCriticalSection(&self.lock);
      }

      //? 控制截图 interval
      const DWORD elapsed = GetTickCount() - started;
      const DWORD interval = static_cast<DWORD>(1000.0 / FPS);
      const DWORD delay = elapsed < interval ? interval - elapsed : 1;
      if (WaitForSingleObject(self.stopEvent, delay) != WAIT_TIMEOUT) {
        break;
      }
    }

    for (auto& buffer : self.buffers) {
      buffer.shutdown();
    }
    return 0;
  }

  bool start(int captureWidth, int captureHeight, int sourceX, int sourceY) {
    if (worker != NULL) {
      return initialized;
    }
    width = captureWidth;
    height = captureHeight;
    left = sourceX;
    top = sourceY;
    stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    wakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    initializedEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (stopEvent && wakeEvent && initializedEvent) {
      worker = CreateThread(NULL, 0, run, this, 0, NULL);
    }
    if (worker == NULL) {
      stop();
      return false;
    }
    WaitForSingleObject(initializedEvent, INFINITE);
    if (!initialized) {
      stop();
      return false;
    }
    return true;
  }

  void setEnabled(bool value) {
    EnterCriticalSection(&lock);
    enabled.store(value);
    ready = -1;
    LeaveCriticalSection(&lock);
    if (value && wakeEvent) {
      SetEvent(wakeEvent);
    }
  }

  int acquireLatest() {
    EnterCriticalSection(&lock);
    const int result = ready;
    if (result >= 0) {
      reading = result;
      ready = -1;
    }
    LeaveCriticalSection(&lock);
    return result;
  }

  void release() {
    EnterCriticalSection(&lock);
    reading = -1;
    LeaveCriticalSection(&lock);
  }

  void stop() {
    if (worker) {
      SetEvent(stopEvent);
      WaitForSingleObject(worker, INFINITE);
      CloseHandle(worker);
      worker = NULL;
    }
    if (stopEvent) CloseHandle(stopEvent);
    if (wakeEvent) CloseHandle(wakeEvent);
    if (initializedEvent) CloseHandle(initializedEvent);
    stopEvent = wakeEvent = initializedEvent = NULL;
    enabled.store(false);
    initialized = false;
    ready = reading = -1;
  }
};

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

HDC g_hdc = NULL;
HGLRC g_glrc = NULL;
GLuint shader_img;
GLuint screen_texture;
GLuint screenVBO, screenVAO, screenEBO;

GLuint screen_upload_pbo[2] = {};
GLsync screen_upload_fence[2] = {};
int screen_upload_pbo_index = 0;
size_t screen_upload_bytes = 0;
bool screen_mipmaps_dirty = false;

GLuint shader_cursor;
GLuint cursor_vbo, cursor_vao;

unsigned char pixel[4];

ScreenCaptureBuffer screenCapture;

RealtimeCapture realtimeCapture;
SystemCursorVisibility systemCursor;
CursorRecoveryWatchdog cursorRecovery;

LRESULT CALLBACK WindowProcRouter(HWND hwnd, UINT uMsg, WPARAM wParam,
                                  LPARAM lParam);
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

void RGBtoHSV(int r, int g, int b, float& h, float& s, float& v);
bool ReadScreenPixel(const POINT& desktopPos, unsigned char* rgba);
bool ReadFrozenImagePixel(const POINT& desktopPos, unsigned char* rgba);
bool ReadDesktopPixel(const POINT& desktopPos, unsigned char* rgba);
bool CacheFrozenScreenTexture();

POINT CaptureToOverlayPoint(const POINT& capturePos);
POINT OverlayToCapturePoint(const POINT& mouseClient);

void SetInputPassthrough(bool enabledInputThrough);
bool IsMouseInputMessage(UINT message);
bool IsUserInputMessage(UINT message);

bool UpdateScreenTexture();
void ConfigureScreenTextureFilter(bool realtime);

void SetRealtimeEnabled(bool enabled);
void ShutdownCursorControl();

void InitializeScreenUploadPbos();
void ShutdownScreenUploadPbos();

bool IsInsideRect(const RECT& rect, const POINT& src);

void RenderScreen_raw();
void RenderCursor(POINT renderPos);

void RenderBegin() {
  glViewport(0, 0, virtualWidth, virtualHeight);
  glClearColor(0.1, 0.1, 0.1, 1);
  glClear(GL_COLOR_BUFFER_BIT);
}
void RenderEnd() { SwapBuffers(g_hdc); }

void RenderFrame(float dt, float elapsed) {
  GetCursorPos(&mouse_pos);

  if (useInputThroughWindow) {
    // 穿透模式下反过来，映射到 overlay 渲染光标
    mouse_pos_target = mouse_pos;
    ScreenToClient(overlay, &mouse_pos_target);
    mouse_pos_client = CaptureToOverlayPoint(mouse_pos_target);
    overlayViewPortDelta = {mouse_pos_target.x - overlayViewPortCenter.x,
                            mouse_pos_target.y - overlayViewPortCenter.y};
    if (!IsInsideRect(overlayViewPort, mouse_pos_target)) {
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

  if (!useInputThroughWindow) {
    // Match the camera used to draw this frame, including pan/zoom inertia.
    mouse_pos_target = OverlayToCapturePoint(mouse_pos_client);
  }
  POINT viewPortLT = OverlayToCapturePoint(POINT{virtualLeft, virtualTop});
  POINT viewPortRB = OverlayToCapturePoint(
      POINT{virtualLeft + virtualWidth - 1, virtualTop + virtualHeight - 1});

  overlayViewPort.left = viewPortLT.x;
  overlayViewPort.top = viewPortLT.y;
  overlayViewPort.right = viewPortRB.x;
  overlayViewPort.bottom = viewPortRB.y;
  overlayViewPortCenter.x =
      (overlayViewPort.left + overlayViewPort.right) * 0.5;
  overlayViewPortCenter.y =
      (overlayViewPort.top + overlayViewPort.bottom) * 0.5;

  flashLight.update(dt);

  if (useRealTime) {
    UpdateScreenTexture();
  }

  RenderBegin();
  RenderScreen_raw();

  if (flashLight.isEnabled) {
#ifdef FREETYPE
    const POINT samplePos =
        useInputThroughWindow ? mouse_pos : mouse_pos_target;
    const bool pixelValid = ReadScreenPixel(samplePos, pixel);
    if (!pixelValid) std::memset(pixel, 0, sizeof(pixel));

    int r = pixel[0];
    int g = pixel[1];
    int b = pixel[2];

    float h, s, v;
    RGBtoHSV(r, g, b, h, s, v);
    float tx = 25.0f;
    float ty = 20.0f;
    float scale = 1.0f;
    float padding = PIXEL_HEIGHT * scale * 2;

#ifdef DEBUG
    Vec3f color = Vec3f(255.0f, 255.0f, 255.0f);
#else
    Vec3f color = Vec3f(r / 255.0f, g / 255.0f, b / 255.0f);
#endif
    if (pixelValid) {
      SimpleRenderOneLine(tx, ty, scale, color, virtualWidth, virtualHeight,
                          "RGB: %d %d %d", r, g, b);
      ty += padding;

      SimpleRenderOneLine(tx, ty, scale, color, virtualWidth, virtualHeight,
                          "HEX: #%02X%02X%02X", r, g, b);
      ty += padding;

      SimpleRenderOneLine(tx, ty, scale, color, virtualWidth, virtualHeight,
                          "HSV: (%.0f°, %.0f%%, %.0f%%)", h, s * 100, v * 100);
    } else {
      SimpleRenderOneLine(tx, ty, scale, Vec3f(1, 1, 1), virtualWidth,
                          virtualHeight, "Color: N/A");
    }

#ifdef DEBUG
    ty += padding;
    SimpleRenderOneLine(tx, ty, scale, color, virtualWidth, virtualHeight,
                        "<<< camerapos.x=%d camerapos.y=%d scale=%f >>>",
                        (int)camera.position.x, (int)camera.position.y,
                        camera.scale);
    ty += padding;

    SimpleRenderOneLine(tx, ty, scale, color, virtualWidth, virtualHeight,
                        "<<< mousepos.x=%d mousepos.y=%d>>>", (int)mouse_pos.x,
                        (int)mouse_pos.y);
    ty += padding;

    SimpleRenderOneLine(tx, ty, scale, color, virtualWidth, virtualHeight,
                        "<<< real time mode: %s >>>",
                        useRealTime ? "on" : "off");
    ty += padding;

    SimpleRenderOneLine(tx, ty, scale, color, virtualWidth, virtualHeight,
                        "<<< targetPos.x=%d targetPos.y=%d>>>",
                        mouse_pos_target.x, mouse_pos_target.y);
    ty += padding;

    SimpleRenderOneLine(tx, ty, scale, color, virtualWidth, virtualHeight,
                        "<<< mouse_pos_client.x=%d mouse_pos_client.y=%d>>>",
                        (int)mouse_pos_client.x + virtualLeft,
                        (int)mouse_pos_client.y + virtualTop);
    ty += padding;

    SimpleRenderOneLine(
        tx, ty, scale, color, virtualWidth, virtualHeight,
        "<<< overlayViewPort left=%ld top=%ld right=%ld bottom=%ld >>>",
        overlayViewPort.left, overlayViewPort.top, overlayViewPort.right,
        overlayViewPort.bottom);

    ty += padding;
    SimpleRenderOneLine(tx, ty, scale, color, virtualWidth, virtualHeight,
                        "<<< overlayViewPortDelta dx=%ld dy=%ld >>>",
                        overlayViewPortDelta.x, overlayViewPortDelta.y);
#endif
#endif
  }
  RenderCursor(mouse_pos_client);
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

  overlayViewPort.left = virtualLeft;
  overlayViewPort.top = virtualTop;
  overlayViewPort.right = virtualLeft + virtualWidth;
  overlayViewPort.bottom = virtualTop + virtualHeight;
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
  // The custom cursor is drawn in OpenGL; WM_SETCURSOR handles normal mode.
  wcex.hCursor = NULL;
  wcex.hbrBackground = (HBRUSH)CreateSolidBrush(RGB(0, 0, 0));
  wcex.lpszMenuName = NULL;
  wcex.lpszClassName = WIN_CLASS_NAME;
  wcex.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

  RegisterClassEx(&wcex);

  RegisterHotKey(NULL, QUIT_KEY_ID, MOD_CONTROL | MOD_SHIFT, VK_F12);
  RegisterHotKey(NULL, TOGGLE_INPUT_HOTKEY_ID, MOD_CONTROL | MOD_SHIFT, VK_F11);
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
    MessageBoxW(overlay, L"Could not register mouse Raw Input.", L"Error",
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
  ShowCursor(false);

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


  g_hdc = GetDC(overlay);
  PIXELFORMATDESCRIPTOR pfd = {sizeof(PIXELFORMATDESCRIPTOR)};
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  pfd.cAlphaBits = 8;
  pfd.cDepthBits = 24;
  int pf = ChoosePixelFormat(g_hdc, &pfd);
  SetPixelFormat(g_hdc, pf, &pfd);

  g_glrc = wglCreateContext(g_hdc);
  if (g_glrc == NULL || g_hdc == NULL) {
    MessageBoxA(NULL, "failed to create glContext", "Error",
                MB_OK | MB_ICONERROR);
    return 1;
  }

  wglMakeCurrent(g_hdc, g_glrc);

  if (!gladLoadGL()) {
    MessageBoxA(NULL, "failed to run gladLoadGL", "Error",
                MB_OK | MB_ICONERROR);
    return 1;
  }

  // shader
  shader_img = createShader(vertexShader, fragmentShader);
  if (shader_img == 0) {
    return 1;
  }

  shader_cursor = createShader(cursorVertShader, cursorFragShader);
  if (shader_cursor == 0) {
    return 1;
  }

  if (!screenCapture.initialize(virtualWidth, virtualHeight) ||
      !screenCapture.capture(virtualLeft, virtualTop)) {
    MessageBoxA(NULL, "failed to capture the virtual screen", "Error",
                MB_OK | MB_ICONERROR);
    screenCapture.shutdown();
    return 1;
  }

#ifdef FREETYPE
  if (!InitFreetype()) return 1;
#endif  // FREETYPE

  // clang-format off
  float vertices[] = {
      0, 0, 0, 0, 0,  // top left
      (float)virtualWidth, 0, 0, 1, 0,  // top right
      0, (float)virtualHeight, 0, 0, 1,  // bottom left
      (float)virtualWidth, (float)virtualHeight, 0, 1, 1  // bottom right
  };
  unsigned int indices[] = {0, 1, 2, 1, 2, 3};
  // clang-format on

  // screen vao
  glGenTextures(1, &screen_texture);
  glGenBuffers(1, &screenVBO);
  glGenVertexArrays(1, &screenVAO);
  glGenBuffers(1, &screenEBO);

  glBindVertexArray(screenVAO);
  glBindBuffer(GL_ARRAY_BUFFER, screenVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, screenEBO);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
               GL_STATIC_DRAW);

  glVertexAttribPointer(0, 3, GL_FLOAT, false, 5 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(0);

  glVertexAttribPointer(1, 2, GL_FLOAT, false, 5 * sizeof(float),
                        (void*)(3 * sizeof(float)));
  glEnableVertexAttribArray(1);

  // texture
  glBindTexture(GL_TEXTURE_2D, screen_texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, virtualWidth, virtualHeight, 0,
               GL_BGRA, GL_UNSIGNED_BYTE, screenCapture.pixels);
  glGenerateMipmap(GL_TEXTURE_2D);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);

  glBindTexture(GL_TEXTURE_2D, 0);
  glBindVertexArray(0);

  float ratio[2] = {(float)virtualWidth, (float)virtualHeight};
  glUseProgram(shader_img);
  glUniform2fv(glGetUniformLocation(shader_img, "uResolution"), 1, ratio);
  glUniform2fv(glGetUniformLocation(shader_img, "windowSize"), 1, ratio);

  // cursor vao
  glGenVertexArrays(1, &cursor_vao);
  glGenBuffers(1, &cursor_vbo);

  glBindVertexArray(cursor_vao);
  glBindBuffer(GL_ARRAY_BUFFER, cursor_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 12, NULL, GL_DYNAMIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), 0);
  glEnableVertexAttribArray(0);
  glBindVertexArray(0);

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
    UnregisterHotKey(NULL, TOGGLE_INPUT_HOTKEY_ID);
    realtimeCapture.stop();
    screenCapture.shutdown();

    glDeleteBuffers(1, &screenVBO);
    glDeleteBuffers(1, &screenEBO);
    glDeleteVertexArrays(1, &screenVAO);
    ShutdownScreenUploadPbos();
    CloseHandle(frameTimer);
#ifdef FREETYPE
    ClearFreetype();
#endif  // FREETYPE
    glDeleteTextures(1, &screen_texture);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(g_glrc);
    ReleaseDC(overlay, g_hdc);
    running = false;
  };

  while (running) {
    for (int i = 0; i < waitEventCount; ++i) {
      if (!PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        break;
      }

      if (msg.message == WM_QUIT) {
        // Restore immediately, before joining the capture worker or GL cleanup.
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
  //! arm 启动子进程
  if (enabledInputThrough && !cursorRecovery.arm()) {
    MessageBoxW(overlay,
                L"Could not start the cursor recovery process. Passthrough "
                L"mode was not enabled, so the system cursor remains visible.",
                L"Cursor recovery", MB_OK | MB_ICONERROR);
    return;
  }
  if (!systemCursor.setHidden(enabledInputThrough)) {
    MessageBoxW(overlay,
                enabledInputThrough
                    ? L"Could not hide the system cursor. Passthrough mode "
                      L"was not enabled."
                    : L"Could not restore the system cursor. The application "
                      L"will retry during shutdown.",
                L"Magnification API", MB_OK | MB_ICONERROR);
    if (enabledInputThrough) {
      return;
    }
  } else if (!enabledInputThrough) {
    //? 如果 setHidden 成功进入这里,此时不需要重复restore
    cursorRecovery.disarm();
  }
  useInputThroughWindow = enabledInputThrough;
  isDragging = false;
  ReleaseCapture();
  if (enabledInputThrough) {
    SetRealtimeEnabled(true);
    flashLight.isEnabled = false;
  }

  if (enabledInputThrough) {
    HWND foreground = GetForegroundWindow();
    if (foreground != NULL && foreground != overlay) {
      previousForegroundWindow = foreground;
    }
  }

  LONG_PTR style = GetWindowLongPtr(overlay, GWL_EXSTYLE);

  if (enabledInputThrough) {
    // WS_EX_TRANSPARENT alone affects paint ordering on a normal window. The
    // layered-window combination makes the hit-test pass through across
    // processes as well.
    //! 还是需要layered window才能保证鼠标命中事件穿透
    style |= WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
  } else {
    style &= ~(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
  }
  SetWindowLongPtr(overlay, GWL_EXSTYLE, style);

  SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

  if (enabledInputThrough) {
    //? 不调用这个也可以，先保留
    SetLayeredWindowAttributes(overlay, 0, 255, LWA_ALPHA);
  }

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
      SetCursor(LoadCursorW(NULL, IDC_ARROW));
      return TRUE;
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
          SetRealtimeEnabled(false);
          SetFocus(overlay);
          break;
        }
        case 'U': {
          SetRealtimeEnabled(!useRealTime);
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
    case WM_ERASEBKGND: {
      return 1;
    }
  }
  return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void ShutdownCursorControl() {
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
  if (screenCapture.pixels == nullptr) return false;
  // OverlayToCapturePoint returns absolute desktop coordinates. Convert them
  // to image-local coordinates, including negative multi-monitor origins.
  const long long x = static_cast<long long>(desktopPos.x) - virtualLeft;
  const long long y = static_cast<long long>(desktopPos.y) - virtualTop;
  if (x < 0 || y < 0 || x >= screenCapture.width || y >= screenCapture.height) {
    return false;
  }

  const size_t offset =
      (static_cast<size_t>(screenCapture.height - 1 - y) * screenCapture.width +
       static_cast<size_t>(x)) *
      4;

  rgba[0] = screenCapture.pixels[offset + 2];
  rgba[1] = screenCapture.pixels[offset + 1];
  rgba[2] = screenCapture.pixels[offset];
  rgba[3] = 255;  // The fourth byte of a GDI BI_RGB capture is not valid alpha.
  return true;
}

bool ReadScreenPixel(const POINT& desktopPos, unsigned char* rgba) {
  return useRealTime ? ReadDesktopPixel(desktopPos, rgba)
                     : ReadFrozenImagePixel(desktopPos, rgba);
}

bool CacheFrozenScreenTexture() {
  // Read once when leaving realtime mode. Capture buffers may already contain
  // a newer, not-yet-uploaded frame, so read the actual displayed texture.
  if (screenCapture.pixels == nullptr || screenCapture.width != virtualWidth ||
      screenCapture.height != virtualHeight) {
    screenCapture.shutdown();
    if (!screenCapture.initialize(virtualWidth, virtualHeight)) return false;
  }
  GLint previousTexture = 0, previousPackBuffer = 0;

  //? 查询当前活动单元glActiveTexture使用纹理
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);  // 名称
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,
                &previousPackBuffer);  // 查询当前绑定buffer
  const GLenum packing[] = {GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH,
                            GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS};
  GLint previousPacking[4]{};
  for (int i = 0; i < 4; ++i) {
    glGetIntegerv(packing[i], &previousPacking[i]);
    glPixelStorei(packing[i], i == 0 ? 1 : 0);
  }

  /*
    只有当 glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); 设置不经过读回 pbo
    才可以通过glGetTexImage(..., screenCapture.pixels);直接都会内存
    所以要保存previous GL_PIXEL_PACK_BUFFER_BINDING
  */
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, screen_texture);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                screenCapture.pixels);
  const GLenum error = glGetError();
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
  glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPackBuffer));
  for (int i = 0; i < 4; ++i) glPixelStorei(packing[i], previousPacking[i]);
  if (error != GL_NO_ERROR) {
    screenCapture.shutdown();
    OutputDebugStringA(
        "winzoomer: failed to cache frozen image for color picking\n");
    return false;
  }
  return true;
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

void ConfigureScreenTextureFilter(bool realtime) {
  glBindTexture(GL_TEXTURE_2D, screen_texture);
  // Every entry into realtime mode must disable all old mip levels, including
  // the entry via passthrough. Rebuild the chain once when freezing the image.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
  //? 实时模式使用原图 第 0 层, 使用linear
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, realtime ? 0 : 1000);
  if (!realtime && screen_mipmaps_dirty) {
    glGenerateMipmap(GL_TEXTURE_2D);
    screen_mipmaps_dirty = false;
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  realtime ? GL_LINEAR : GL_LINEAR_MIPMAP_LINEAR);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void SetRealtimeEnabled(bool enabled) {
  if (enabled && !realtimeCapture.start(virtualWidth, virtualHeight,
                                        virtualLeft, virtualTop)) {
    OutputDebugStringA("winzoomer: failed to start realtime capture\n");
    return;
  }
  if (!enabled && useRealTime) {
    realtimeCapture.setEnabled(false);
    CacheFrozenScreenTexture();
  }
  useRealTime = enabled;
  if (enabled && !screen_upload_pbo[0]) {
    InitializeScreenUploadPbos();
  }
  //? wakeEvent
  realtimeCapture.setEnabled(enabled);
  ConfigureScreenTextureFilter(enabled);
}

void ShutdownScreenUploadPbos() {
  for (auto& fence : screen_upload_fence) {
    if (fence) {
      glDeleteSync(fence);
      fence = nullptr;
    }
  }
  glDeleteBuffers(2, screen_upload_pbo);
  screen_upload_pbo[0] = screen_upload_pbo[1] = 0;
  screen_upload_pbo_index = 0;
}

void InitializeScreenUploadPbos() {
  screen_upload_bytes = static_cast<size_t>(virtualWidth) *
                        static_cast<size_t>(virtualHeight) * 4;
  // Old drivers can still use the direct upload path.
  if (!glMapBufferRange || !glFenceSync || !glClientWaitSync || !glDeleteSync ||
      !glGetBufferParameteri64v) {
    return;
  }
  glGenBuffers(2, screen_upload_pbo);
  bool allocated = screen_upload_pbo[0] != 0 && screen_upload_pbo[1] != 0;
  for (GLuint pbo : screen_upload_pbo) {
    if (!allocated) break;
    //? 上传数据到gpu使用 unpack 描述pbo
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_UNPACK_BUFFER,
                 static_cast<GLsizeiptr>(screen_upload_bytes), nullptr,
                 GL_STREAM_DRAW);
    GLint64 allocatedBytes = 0;
    glGetBufferParameteri64v(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE,
                             &allocatedBytes);
    allocated = allocatedBytes == static_cast<GLint64>(screen_upload_bytes);
  }
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  if (!allocated) {
    OutputDebugStringA(
        "winzoomer: PBO allocation failed, using direct upload\n");
    ShutdownScreenUploadPbos();
  }
}

bool UpdateScreenTexture() {
  int pboIndex = -1;
  if (screen_upload_pbo[0]) {
    for (int offset = 0; offset < 2; ++offset) {
      const int i = (screen_upload_pbo_index + offset) % 2;
      if (screen_upload_fence[i]) {
        // Never wait on the UI thread. Keep displaying the current texture if
        // both staging buffers are still in use by the GPU.
        const GLenum status = glClientWaitSync(screen_upload_fence[i], 0, 0);
        if (status == GL_TIMEOUT_EXPIRED) continue;  // 未完成
        if (status == GL_WAIT_FAILED) {
          ShutdownScreenUploadPbos();
          break;
        }
        glDeleteSync(screen_upload_fence[i]);
        screen_upload_fence[i] = nullptr;
      }
      pboIndex = i;
      break;
    }
    if (screen_upload_pbo[0] && pboIndex < 0) return false;
  }

  const int frame = realtimeCapture.acquireLatest();
  if (frame < 0) return false;

  const unsigned char* pixels = realtimeCapture.buffers[frame].pixels;
  bool staged = false;

  if (pboIndex >= 0) {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, screen_upload_pbo[pboIndex]);
    // The fence guarantees that the GPU has finished using this allocation.
    // Mapping it unsynchronized is safe and avoids a driver-imposed wait.
    //? 映射内存
    void* destination = glMapBufferRange(
        GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(screen_upload_bytes),
        GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    //? 同步复制到pbo
    if (destination) {
      std::memcpy(destination, pixels, screen_upload_bytes);
      //? 结束映射,检查隐射结果
      staged = glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER) == GL_TRUE;
    }
    if (!staged) {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
      ShutdownScreenUploadPbos();
    }
  }

  //? 提交 pbo -> 纹理的更新请求,glTexSubImage2D 异步
  //? 当前 pbo 会被占用
  glBindTexture(GL_TEXTURE_2D, screen_texture);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, virtualWidth, virtualHeight, GL_BGRA,
                  GL_UNSIGNED_BYTE, staged ? nullptr : pixels);
  if (staged) {
    //? glFenceSync 在gpu 命令流里插入标记
    //? GL_SYNC_GPU_COMMANDS_COMPLETE
    // 在此之前的命令全部完成后触发,等gltexsubimage2D
    screen_upload_fence[pboIndex] =
        glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (!screen_upload_fence[pboIndex]) {
      ShutdownScreenUploadPbos();
    } else {
      screen_upload_pbo_index = (pboIndex + 1) % 2;
    }
  }
  realtimeCapture.release();

  screen_mipmaps_dirty = true;
  glBindTexture(GL_TEXTURE_2D, 0);
  return true;
}

void RenderScreen_raw() {
  glUseProgram(shader_img);

  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(screenVAO);
  glBindTexture(GL_TEXTURE_2D, screen_texture);

  float cameraPos[2] = {camera.position.x, camera.position.y};
  float mousePos[2] = {(float)mouse_pos_client.x, (float)mouse_pos_client.y};

  glUniform1f(glGetUniformLocation(shader_img, "cameraScale"), camera.scale);
  glUniform1f(glGetUniformLocation(shader_img, "flShadow"), flashLight.shadow);
  glUniform1f(glGetUniformLocation(shader_img, "flRadius"), flashLight.radius);

  glUniform2fv(glGetUniformLocation(shader_img, "cameraPos"), 1, cameraPos);
  glUniform2fv(glGetUniformLocation(shader_img, "mousePos"), 1, mousePos);
  glUniform1i(glGetUniformLocation(shader_img, "uTexture"), 0);

  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT,
                 (void*)(0 * sizeof(unsigned int)));

  glBindVertexArray(0);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void RenderCursor(POINT renderPos) {
  static GLfloat cursorVerticesOrigin[12] = {
      0, 0, 0,     -1.0f,     0.27f,     -0.65184f,
      0, 0, 0.27f, -0.65184f, 0.707106f, -0.707106f};

  GLfloat cursorVertices[12] = {0};
  float cursorScale = 30.0f;
  Vec3f color = {1, 1, 1};

  for (int i = 0; i < 12; i += 2) {
    cursorVertices[i] = cursorVerticesOrigin[i] * cursorScale + renderPos.x;
    cursorVertices[i + 1] = cursorVerticesOrigin[i + 1] * cursorScale +
                            (virtualHeight - renderPos.y);
  }

  glUseProgram(shader_cursor);
  glBindVertexArray(cursor_vao);

  float c[3] = {color.x, color.y, color.z};
  Mat4 projection = ortho(0, virtualWidth, 0, virtualHeight);
  glUniform3fv(glGetUniformLocation(shader_cursor, "fillColor"), 1, c);
  glUniformMatrix4fv(glGetUniformLocation(shader_cursor, "projection"), 1,
                     GL_FALSE, projection.m);
  glBindBuffer(GL_ARRAY_BUFFER, cursor_vbo);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(cursorVertices), cursorVertices);

  glDrawArrays(GL_TRIANGLES, 0, 6);
}
