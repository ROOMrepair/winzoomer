#ifndef UNICODE
#define UNICODE
#include <windows.h>
#endif

#ifndef _UNICODE
#define _UNICODE
#endif

#include <cassert>
#include <tchar.h>
#include <strsafe.h>
#include <windowsx.h>
#include <math.h>
#include <ShellScalingApi.h>
#include "shader.hpp"

#define BUF_SIZE 512
#define REFRESH_TIMER_ID 1

#define wheelScale 0.005
#define scaleFriction 3.0
#define rate 75.0
#define miniScale 0.01
#define radiusDeceleration 10.0
#define dragFriction 6.0

#define VELOCITY_THRESHOLD 15.0
#define INITIAL_FL_DELTA_RADIUS 250.0

#define UNFOLD_RECT_XYWH(rect) \
  rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top

const wchar_t WIN_CLASS_NAME[] = _T("WHAT_8MTfo7IzrQ");
const wchar_t MUTEX_NAME[] = _T("WHAT_1JzKDIayja");

typedef struct Vec2f {
  float x;
  float y;
  Vec2f() : x(0), y(0) {}
  Vec2f(float _x, float _y) : x(_x), y(_y) {}
  Vec2f operator+(const float s) { return Vec2f(x + s, y + s); }
  Vec2f operator-(const float s) { return Vec2f(x - s, y - s); }
  Vec2f operator-(const Vec2f other) { return Vec2f(x - other.x, y - other.y); }
  Vec2f operator*(const float s) { return Vec2f(x * s, y * s); }
  Vec2f operator/(const float s) { return Vec2f(x / s, y / s); }
  Vec2f& operator+=(const Vec2f v) {
    this->x += v.x;
    this->y += v.y;
    return *this;
  }
  Vec2f& operator-=(const Vec2f v) {
    this->x -= v.x;
    this->y -= v.y;
    return *this;
  }
  float length() { return sqrt(x * x + y * y); }
} Vec2f;

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

std::string vertexShader = R"(
#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

out vec2 TexCoord;

uniform vec2 uResolution;
uniform vec2 cameraPos;
uniform float cameraScale;

uniform vec2 screenshotSize;

void main()
{
    // BitmapToMem 存储时已经倒过来了
    vec2 ndc = vec2((((aPos.x - cameraPos.x) / uResolution.x)* 2.0 - 1.0) * cameraScale,
        (((aPos.y + cameraPos.y ) / uResolution.y) * 2.0 - 1.0) * cameraScale);
    gl_Position = vec4(ndc, 0, 1.0);
    TexCoord = aTexCoord;
}
)";

std::string fragmentShader = R"(
#version 330 core

in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform vec2 mousePos;
uniform float flShadow;
uniform float flRadius;
uniform float cameraScale;
uniform vec2 windowSize;

void main()
{

  vec4 cursor = vec4(mousePos.x,windowSize.y - mousePos.y,0.0,1.0);

  FragColor = mix(
      texture(uTexture,TexCoord),
      vec4(0.0,0.0,0.0,0.0),
      length(cursor - gl_FragCoord) < (flRadius * cameraScale) ? 0.0 : flShadow
      );
}
)";

//& >>>>>>>>>>>> state
HWND overlay, dialog;
COLORREF color;
COLORREF prevColor;
POINT mouse_pos;
POINT last_pos;
TCHAR buf[BUF_SIZE] = {};
char clicked;

int virtualLeft, virtualTop, virtualWidth, virtualHeight;

FlashLight flashLight;
Camera camera;

bool isDragging;

bool showDialog;

//& opengl
HDC g_hdc = NULL;
HGLRC g_glrc = NULL;
GLuint screen_texture;
GLuint screenVBO;
GLuint screenVAO;
GLuint screenEBO;

//& >>>>>>>>>>>> rect
RECT rcRGB = {10, 40, 250, 60};
RECT rcHEX = {10, 60, 250, 80};
RECT rcHSV = {10, 80, 250, 100};
RECT tip_rc = {10, 100, 300, 150};
RECT tip_rc_2 = {10, 150, 300, 200};
RECT tip_rc_3 = {10, 200, 500, 250};
RECT tip_rc_4 = {10, 250, 600, 300};
RECT rcColor = {220, 20, 300, 80};
RECT dialogWindow = {0, 0, 400, 300};
//& >>>>>>>>>>>> function
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void RGBtoHSV(int r, int g, int b, double& h, double& s, double& v);
HBITMAP CaptureScreenToBitmap(int width, int height);
unsigned char* BitmapToMem(HBITMAP hbm, int width, int height);
void DrawFrame(unsigned int vao, Shader& shader);
void DrawFrame_raw(unsigned int vao, GLuint shader);
GLuint createShader();

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR pCmdLine, int nCmdShow) {
  SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
  HANDLE hMutex = ::CreateMutex(NULL, TRUE, MUTEX_NAME);
  if (hMutex != NULL) {
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      HWND handle = FindWindow(WIN_CLASS_NAME, NULL);
      if (handle != NULL) {
        ShowWindow(handle, SW_SHOWNORMAL);
        SetForegroundWindow(handle);
        return FALSE;
      }
    }
  } else {
    StringCchPrintf(buf, BUF_SIZE, _T("程序运行错误: %lu\n"), GetLastError());
    MessageBox(NULL, buf, _T("提示"), MB_OK);
    return FALSE;
  }

  WNDCLASSEX wcex;
  wcex.cbSize = sizeof(wcex);
  wcex.style = CS_HREDRAW | CS_VREDRAW;
  wcex.lpfnWndProc = WindowProc;
  wcex.cbClsExtra = 0;
  wcex.cbWndExtra = 0;
  wcex.hInstance = hInstance;
  wcex.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
  wcex.hbrBackground = NULL;
  wcex.lpszMenuName = NULL;
  wcex.lpszClassName = WIN_CLASS_NAME;
  wcex.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

  RegisterClassEx(&wcex);

  RegisterHotKey(NULL, 1, MOD_CONTROL | MOD_SHIFT, VK_F12);

  virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
  virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
  virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

  overlay = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOOLWINDOW, WIN_CLASS_NAME,
                           L"", WS_POPUP, 0, 0, virtualWidth, virtualHeight,
                           NULL, NULL, GetModuleHandle(NULL), NULL);
  SetLayeredWindowAttributes(overlay, RGB(0, 0, 0), 0, LWA_COLORKEY);
  // SetLayeredWindowAttributes(overlay, 0, 255, LWA_ALPHA);

  dialog = CreateWindowEx(WS_EX_TOPMOST, WIN_CLASS_NAME, L"", WS_POPUP,
                          UNFOLD_RECT_XYWH(dialogWindow), NULL, NULL, hInstance,
                          NULL);

  if (overlay == NULL || dialog == NULL) {
    return 0;
  }

  ShowWindow(overlay, nCmdShow);

  showDialog = false;
  camera.scale = 1.0f;
  camera.deltaScale = 0.0f;
  flashLight.radius = 100.0f;
  flashLight.deltaRadius = 0.0f;
  flashLight.isEnabled = false;

  //& <<<<< init opengl
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

  assert(g_glrc != NULL && g_hdc != NULL);
  wglMakeCurrent(g_hdc, g_glrc);

  if (!gladLoadGL()) {
    MessageBox(NULL, L"gladLoadGL() failed", L"Error", MB_OK);
    return false;
  }
  //& >>>>> init opengl

  // std::string vpath = std::string("./vert.glsl");
  // std::string fpath = std::string("./frag.glsl");
  // Shader shader(vpath.c_str(), fpath.c_str());
  GLuint shader = createShader();

  glViewport(0, 0, virtualWidth, virtualHeight);
  glGenTextures(1, &screen_texture);
  glGenBuffers(1, &screenVBO);
  glGenVertexArrays(1, &screenVAO);
  glGenBuffers(1, &screenEBO);

  // clang-format off
  float vertices[] = {
      0,    0,  0,    0,   0,                     // top left
      (float)virtualWidth,   0, 0,   1,   0,     // top right
      0,   (float)virtualHeight, 0,   0,   1,     // bottom left
      (float)virtualWidth,   (float)virtualHeight, 0,   1,   1 // bottom   right
  };
  unsigned int indices[] = {
    0,1,2,
    1,2,3
  };
  // clang-format on

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

  auto hbitmap = CaptureScreenToBitmap(virtualWidth, virtualHeight);
  auto data = BitmapToMem(hbitmap, virtualWidth, virtualHeight);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, screen_texture);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, virtualWidth, virtualHeight, 0,
               GL_BGRA, GL_UNSIGNED_BYTE, data);
  glGenerateMipmap(GL_TEXTURE_2D);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);

  DeleteObject(hbitmap);
  delete data;

  float ratio[2] = {(float)virtualWidth, (float)virtualHeight};

  // shader.use();
  // shader.setVec2(std::string("uResolution"), ratio);
  // shader.setVec2(std::string("windowSize"), ratio);
  // shader.setInt(std::string("uTexture"), 0);
  glUseProgram(shader);
  glUniform2fv(glGetUniformLocation(shader, "uResolution"), 1, ratio);
  glUniform2fv(glGetUniformLocation(shader, "windowSize"), 1, ratio);
  glUniform1f(glGetUniformLocation(shader, "uTexture"), 0);

  float dt = 1 / rate;

  SetTimer(overlay, REFRESH_TIMER_ID, 16, NULL);

  MSG msg = {};
  while (true) {
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        KillTimer(overlay, REFRESH_TIMER_ID);
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(g_glrc);
        ReleaseDC(overlay, g_hdc);

        glDeleteBuffers(1, &screenVBO);
        glDeleteBuffers(1, &screenEBO);
        glDeleteVertexArrays(1, &screenVAO);
        return 0;
      }
      if (msg.message == WM_HOTKEY && msg.wParam == 1) {
        KillTimer(dialog, REFRESH_TIMER_ID);
        PostQuitMessage(0);
        continue;
      }
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    camera.update(Vec2f(virtualWidth, virtualHeight), dt, isDragging);
    flashLight.update(dt);
    DrawFrame_raw(screenVAO, shader);
    // DrawFrame(screenVAO, shader);
  }

  return 0;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam,
                            LPARAM lParam) {
  switch (uMsg) {
    case WM_CREATE: {
      return 0;
    }
    case WM_DESTROY: {
      PostQuitMessage(0);
      return 0;
    }
    case WM_KEYUP: {
      if (hwnd == overlay) {
        switch (wParam) {
          case 'F':
            clicked = 'F';
            flashLight.isEnabled = !flashLight.isEnabled;
            break;
          case 'R':
            clicked = 'R';
            camera.scale = 1.0f;
            camera.deltaScale = 0.0f;
            camera.position = Vec2f(0.0f, 0.0f);
            camera.velocity = Vec2f(0.0f, 0.0f);

            flashLight.radius = 100.0f;
            flashLight.deltaRadius = 0.0f;
            flashLight.isEnabled = false;
            showDialog = false;
            ShowWindow(dialog, SW_HIDE);
            SetFocus(overlay);
            break;
          case 'S':
            clicked = 'S';
            showDialog = !showDialog;
            if (showDialog) {
              ShowWindow(dialog, SW_SHOW);
            } else {
              ShowWindow(dialog, SW_HIDE);
            }
            SetFocus(overlay);
            break;
          default:
            break;
        }
      }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      if (hwnd == overlay) {
        isDragging = true;
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      if (hwnd == overlay) {
        isDragging = false;
      }
      return 0;
    }
    case WM_MOUSEWHEEL: {
      if (hwnd == overlay) {
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
      }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      if (hwnd == overlay) {
        //! 这里必须要手动绘制一下，不然peekmessage不会处理wm_paint事件
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
      }
      if (hwnd == dialog) {
        // BeginPaint(hwnd, &ps);
        // EndPaint(hwnd, &ps);
        auto x = mouse_pos.x;
        auto y = mouse_pos.y;
        auto r = GetRValue(color);
        auto g = GetGValue(color);
        auto b = GetBValue(color);

        HDC hdc = BeginPaint(hwnd, &ps);

        //?  离屏 DC
        //?  但为了让它“立即可用”，Windows
        //?  自动在里面塞了一张临时的、非常小的默认位图,oldbmp。
        //?  例如在很多系统上它是一个 ?  1×1 像素的 monochrome bitmap。
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP bmp =
            CreateCompatibleBitmap(hdc, ps.rcPaint.right, ps.rcPaint.bottom);
        //? 替换内部的bitmap,返回原句柄
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);

        FillRect(memDC, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1));

        //! draw start
        StringCchPrintf(buf, BUF_SIZE, _T("x=%ld y=%ld clicked:%c\n"), x, y,
                        clicked);
        DrawTextEx(memDC, buf, -1, &tip_rc, DT_LEFT | DT_TOP, NULL);

        StringCchPrintf(buf, BUF_SIZE, _T("winsize x=%d y=%d w=%d h=%d\n"),
                        virtualLeft, virtualTop, virtualWidth, virtualHeight);
        DrawTextEx(memDC, buf, -1, &tip_rc_2, DT_LEFT | DT_TOP, NULL);

        StringCchPrintf(buf, BUF_SIZE, _T("flashlight:%d fldelta:%.2f\n"),
                        flashLight.isEnabled, flashLight.deltaRadius);
        DrawTextEx(memDC, buf, -1, &tip_rc_3, DT_LEFT | DT_TOP, NULL);

        StringCchPrintf(buf, BUF_SIZE,
                        _T("camera scale:%.2f dscale:%.2f x:%.2f y:%.2f\n"),
                        camera.scale, camera.deltaScale, camera.position.x,
                        camera.position.y);
        DrawTextEx(memDC, buf, -1, &tip_rc_4, DT_LEFT | DT_TOP, NULL);

        StringCchPrintf(buf, BUF_SIZE, _T("RGB:(%d,%d,%d)\n"), r, g, b);
        DrawTextEx(memDC, buf, -1, &rcRGB, DT_LEFT | DT_TOP, NULL);

        StringCchPrintf(buf, 128, _T("HEX: #%02X%02X%02X"), r, g, b);
        DrawText(memDC, buf, -1, &rcHEX, DT_LEFT | DT_TOP);

        double h, s, v;
        RGBtoHSV(r, g, b, h, s, v);
        StringCchPrintf(buf, 128, _T("HSV: (%.0f°, %.0f%%, %.0f%%)"), h,
                        s * 100, v * 100);
        DrawText(memDC, buf, -1, &rcHSV, DT_LEFT | DT_TOP);

        HBRUSH hBrush = CreateSolidBrush(RGB(r, g, b));
        FillRect(memDC, &rcColor, hBrush);
        DeleteBrush(hBrush);

        BitBlt(hdc, 0, 0, ps.rcPaint.right, ps.rcPaint.bottom, memDC, 0, 0,
               SRCCOPY);

        //? 替换回去
        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
        //! draw end
        EndPaint(hwnd, &ps);
      }
      return 0;
    }
    case WM_TIMER: {
      HDC allDC = GetDC(NULL);
      GetCursorPos(&mouse_pos);
      color = GetPixel(allDC, (int)mouse_pos.x, (int)mouse_pos.y);
      DeleteDC(allDC);

      if (last_pos.x != mouse_pos.x || last_pos.y != mouse_pos.y) {
        if (isDragging) {
          float dx = (last_pos.x - mouse_pos.x) / camera.scale;
          float dy = (last_pos.y - mouse_pos.y) / camera.scale;
          camera.position += Vec2f(dx, dy);
          camera.velocity = Vec2f(dx * rate, dy * rate);
        }
        last_pos.x = mouse_pos.x;
        last_pos.y = mouse_pos.y;
      }
      if (color != prevColor) {
        prevColor = color;
      }

      RedrawWindow(dialog, &tip_rc, NULL, RDW_INVALIDATE);
      RedrawWindow(dialog, &tip_rc_2, NULL, RDW_INVALIDATE);
      RedrawWindow(dialog, &tip_rc_3, NULL, RDW_INVALIDATE);
      RedrawWindow(dialog, &tip_rc_4, NULL, RDW_INVALIDATE);
      RedrawWindow(dialog, &rcRGB, NULL, RDW_INVALIDATE);
      RedrawWindow(dialog, &rcHEX, NULL, RDW_INVALIDATE);
      RedrawWindow(dialog, &rcHSV, NULL, RDW_INVALIDATE);
      RedrawWindow(dialog, &rcColor, NULL, RDW_INVALIDATE);
      return 0;
    }
    case WM_MOUSEMOVE: {
      return 0;
    }
    case WM_ERASEBKGND: {
      return 1;
    }
    case WM_DPICHANGED: {
      return 0;
    }
  }
  return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void RGBtoHSV(int r, int g, int b, double& h, double& s, double& v) {
  double rd = r / 255.0;
  double gd = g / 255.0;
  double bd = b / 255.0;

  double maxv = fmax(rd, fmax(gd, bd));
  double minv = fmin(rd, fmin(gd, bd));
  double delta = maxv - minv;

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

HBITMAP CaptureScreenToBitmap(int width, int height) {
  HDC hScreen = GetDC(NULL);
  HDC hMemDC = CreateCompatibleDC(hScreen);

  HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, width, height);
  SelectObject(hMemDC, hBitmap);

  //? 复制屏幕到 hBitmap
  BitBlt(hMemDC, 0, 0, width, height, hScreen, 0, 0, SRCCOPY);

  DeleteDC(hMemDC);
  ReleaseDC(NULL, hScreen);

  return hBitmap;
}

unsigned char* BitmapToMem(HBITMAP hbm, int width, int height) {
  BITMAP bm;
  GetObject(hbm, sizeof(BITMAP), &bm);
  BITMAPINFOHEADER bi = {};
  bi.biSize = sizeof(BITMAPINFOHEADER);
  bi.biWidth = bm.bmWidth;
  bi.biHeight = bm.bmHeight;
  bi.biPlanes = 1;
  bi.biBitCount = 32;
  bi.biCompression = BI_RGB;
  assert(width == bm.bmWidth && height == bm.bmHeight);
  size_t size = bm.bmWidth * bm.bmHeight * 4;
  unsigned char* data = new unsigned char[size];

  HDC hdc = GetDC(NULL);
  GetDIBits(hdc, hbm, 0, height, data, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
  ReleaseDC(NULL, hdc);
  return data;
}

void DrawFrame_raw(unsigned int vao, GLuint shader) {
  glClearColor(0.1, 0.1, 0.1, 1.0);  //? colorkey background
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(shader);

  float cameraPos[2] = {camera.position.x, camera.position.y};
  float mousePos[2] = {(float)mouse_pos.x, (float)mouse_pos.y};

  glUniform1f(glGetUniformLocation(shader, "cameraScale"), camera.scale);
  glUniform1f(glGetUniformLocation(shader, "flShadow"), flashLight.shadow);
  glUniform1f(glGetUniformLocation(shader, "flRadius"), flashLight.radius);

  glUniform2fv(glGetUniformLocation(shader, "cameraPos"), 1, cameraPos);
  glUniform2fv(glGetUniformLocation(shader, "mousePos"), 1, mousePos);

  glBindVertexArray(vao);
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT,
                 (void*)(0 * sizeof(unsigned int)));

  SwapBuffers(g_hdc);
}

void DrawFrame(unsigned int vao, Shader& shader) {
  glClearColor(0.1, 0.1, 0.1, 1.0);  //? colorkey background
  glClear(GL_COLOR_BUFFER_BIT);
  shader.use();

  float cameraPos[2] = {camera.position.x, camera.position.y};
  float mousePos[2] = {(float)mouse_pos.x, (float)mouse_pos.y};
  shader.setFloat("cameraScale", camera.scale);
  shader.setVec2("cameraPos", cameraPos);
  shader.setVec2("mousePos", mousePos);

  shader.setFloat("flShadow", flashLight.shadow);
  shader.setFloat("flRadius", flashLight.radius);

  glBindVertexArray(vao);
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT,
                 (void*)(0 * sizeof(unsigned int)));

  SwapBuffers(g_hdc);
}

void checkCompileErrors(GLuint shader, const std::string& type) {
  GLint success;
  char infoLog[2048];
  ZeroMemory(infoLog, sizeof(infoLog));

  if (type != "PROGRAM") {
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
      glGetShaderInfoLog(shader, sizeof(infoLog), NULL, infoLog);

      std::string msg = "Shader Compilation Error (" + type + ")\n\n";
      msg += infoLog;

      MessageBoxA(NULL, msg.c_str(), "GLSL Error", MB_OK | MB_ICONERROR);
    }
  } else {
    glGetProgramiv(shader, GL_LINK_STATUS, &success);
    if (!success) {
      glGetProgramInfoLog(shader, sizeof(infoLog), NULL, infoLog);

      std::string msg = "Program Linking Error (" + type + ")\n\n";
      msg += infoLog;

      MessageBoxA(NULL, msg.c_str(), "GLSL Error", MB_OK | MB_ICONERROR);
    }
  }
}

GLuint createShader() {
  GLuint vertex, fragment;
  // vertex shader
  vertex = glCreateShader(GL_VERTEX_SHADER);
  const char* vertSrc = vertexShader.c_str();
  glShaderSource(vertex, 1, &vertSrc, NULL);
  glCompileShader(vertex);

  checkCompileErrors(vertex, "VERTEX");
  // fragment Shader
  const char* fragSrc = fragmentShader.c_str();
  fragment = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fragment, 1, &fragSrc, NULL);
  glCompileShader(fragment);
  checkCompileErrors(fragment, "FRAGMENT");
  // shader Program
  GLuint ID = glCreateProgram();
  glAttachShader(ID, vertex);
  glAttachShader(ID, fragment);
  glLinkProgram(ID);
  checkCompileErrors(ID, "PROGRAM");

  // delete the shaders as they're linked into our program now and no longer
  // necessary
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  return ID;
}
