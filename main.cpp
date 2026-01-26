#include <cassert>
#include <cstdio>
#include <string>
#include <libloaderapi.h>
#include <tchar.h>
#include <strsafe.h>
#include <windowsx.h>
#include <math.h>
#include <ShellScalingApi.h>

#ifdef FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include <map>
#endif

#include <glad/glad.h>

#define BUF_SIZE 1024
#define REFRESH_TIMER_ID 1

#define wheelScale 0.005
#define scaleFriction 3.0
#define rate 60.0
#define miniScale 0.01
#define radiusDeceleration 10.0
#define dragFriction 6.0

#define VELOCITY_THRESHOLD 15.0
#define INITIAL_FL_DELTA_RADIUS 250.0

const wchar_t WIN_CLASS_NAME[] = _T("WHAT_8MTfo7IzrQ");
const wchar_t MUTEX_NAME[] = _T("WHAT_1JzKDIayja");

template <typename T>
struct Vec2 {
  T x, y;
  Vec2() : x(0), y(0) {}
  Vec2(T _x, T _y) : x(_x), y(_y) {}
  Vec2 operator+(const T& s) { return Vec2(x + s, y + s); }
  Vec2 operator-(const T& s) { return Vec2(x - s, y - s); }
  Vec2 operator-(const Vec2& other) { return Vec2(x - other.x, y - other.y); }
  Vec2 operator*(const T& s) { return Vec2(x * s, y * s); }
  Vec2 operator/(const T& s) { return Vec2(x / s, y / s); }
  Vec2& operator+=(const Vec2& v) {
    this->x += v.x;
    this->y += v.y;
    return *this;
  }
  Vec2& operator-=(const Vec2& v) {
    this->x -= v.x;
    this->y -= v.y;
    return *this;
  }
  float length() { return static_cast<T>(sqrt(x * x + y * y)); }
};

template <typename T>
struct Vec3 {
  Vec3(T _x, T _y, T _z) : x(_x), y(_y), z(_z) {}
  T x, y, z;
};

typedef Vec2<float> Vec2f;
typedef Vec2<int> Vec2i;
typedef Vec3<int> Vec3i;
typedef Vec3<float> Vec3f;

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

#ifdef FREETYPE
struct Character {
  GLuint TextureID;  // 字形纹理的ID
  Vec2i Size;        // 字形大小
  Vec2i Bearing;     // 从基准线到字形左部/顶部的偏移值
  FT_Pos Advance;    // 原点距下一个字形原点的距离
};
#endif

struct Mat4 {
  float m[16];  // 列主序

  static Mat4 identity() {
    Mat4 r = {};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
  }
};

Mat4 ortho(float left, float right, float bottom, float top) {
  Mat4 r = {};

  r.m[0] = 2.0f / (right - left);
  r.m[5] = 2.0f / (top - bottom);
  r.m[10] = -1.0f;
  r.m[12] = -(right + left) / (right - left);
  r.m[13] = -(top + bottom) / (top - bottom);
  r.m[15] = 1.0f;

  return r;
}

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

std::string textVertShader = R"(
#version 330 core
layout (location = 0) in vec4 vertex; // <vec2 pos, vec2 tex>
out vec2 TexCoords;

uniform mat4 projection;

void main()
{
    gl_Position = projection * vec4(vertex.xy, 0.0, 1.0);
    TexCoords = vertex.zw;
}
)";

std::string textfragmentShader = R"(
#version 330 core
in vec2 TexCoords;
out vec4 color;

uniform sampler2D text;
uniform vec3 textColor;

void main()
{
    vec4 sampled = vec4(1.0, 1.0, 1.0, texture(text, TexCoords).r);
    color = vec4(textColor, 1.0) * sampled;
}

)";

//& >>>>>>>>>>>> state
HWND overlay;
COLORREF color;
POINT mouse_pos;
POINT last_pos;

int virtualLeft, virtualTop, virtualWidth, virtualHeight;

FlashLight flashLight;
Camera camera;

bool isDragging;
float dt;

#ifdef FREETYPE
std::map<GLchar, Character> Characters;
FT_UInt pixel_height = 16;
GLuint textVAO, textVBO;
GLuint shader_txt;
#endif

//& opengl
HDC g_hdc = NULL;
HGLRC g_glrc = NULL;
GLuint shader_img;
GLuint screen_texture;
GLuint screenVBO, screenVAO, screenEBO;

//& >>>>>>>>>>>> function
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void checkCompileErrors(GLuint shader, const std::string& type);
void RGBtoHSV(int r, int g, int b, float& h, float& s, float& v);
HBITMAP CaptureScreenToBitmap(int width, int height);
unsigned char* BitmapToMem(HBITMAP hbm, int width, int height);

GLuint createShader(std::string& vert, std::string& frag);
void RenderScreen_raw();
#ifdef FREETYPE
void RenderText(std::string& text, GLfloat x, GLfloat y, GLfloat scale,
                Vec3f color);
#endif

void RenderBegin() {
  glViewport(0, 0, virtualWidth, virtualHeight);
  glClearColor(0.1, 0.1, 0.1, 1);
  glClear(GL_COLOR_BUFFER_BIT);
}

void RenderEnd() { SwapBuffers(g_hdc); }

// Source - https://stackoverflow.com/a/24386991
// Posted by Pixelchemist
// Retrieved 2025-11-19, License - CC BY-SA 3.0
template <class T>
T file_path(T const& path, T const& delims = "/\\") {
  return path.substr(0, path.find_last_of(delims));
}

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
        return false;
      }
    }
  } else {
    MessageBoxA(NULL, "failed to execuate program", "Error",
                MB_OK | MB_ICONERROR);
    return false;
  }

  virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
  virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
  virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

  WNDCLASSEX wcex;
  wcex.cbSize = sizeof(wcex);
  wcex.style = CS_HREDRAW | CS_VREDRAW;
  wcex.lpfnWndProc = WindowProc;
  wcex.cbClsExtra = 0;
  wcex.cbWndExtra = 0;
  wcex.hInstance = hInstance;
  wcex.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
  wcex.hbrBackground = (HBRUSH)CreateSolidBrush(RGB(0, 0, 0));
  wcex.lpszMenuName = NULL;
  wcex.lpszClassName = WIN_CLASS_NAME;
  wcex.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

  RegisterClassEx(&wcex);

  RegisterHotKey(NULL, 1, MOD_CONTROL | MOD_SHIFT, VK_F12);

  overlay = CreateWindowEx(WS_EX_TOPMOST, WIN_CLASS_NAME, L"", WS_POPUP, 0, 0,
                           virtualWidth, virtualHeight, NULL, NULL,
                           GetModuleHandle(NULL), NULL);

  // todo 怎么用 overlay 实现
  // SetLayeredWindowAttributes(overlay, RGB(0, 0, 0), 0, LWA_COLORKEY);
  // SetLayeredWindowAttributes(overlay, 0, 255, LWA_ALPHA);

  if (overlay == NULL) {
    MessageBoxA(NULL, "fail to create window failed", "Error",
                MB_OK | MB_ICONERROR);
    return false;
  }

  ShowWindow(overlay, nCmdShow);
  SetWindowPos(overlay, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  SetFocus(overlay);

  camera.scale = 1.0f;
  camera.deltaScale = 0.0f;
  flashLight.radius = 100.0f;
  flashLight.deltaRadius = 0.0f;
  flashLight.isEnabled = false;
  dt = (float)1 / rate;

  SetTimer(overlay, REFRESH_TIMER_ID, 16, NULL);

  //& <<<<<<<<<<<<<<<<<<<<<<<<<<<<<< init opengl
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
    MessageBoxA(NULL, "failed to run gladLoadGL", "Error",
                MB_OK | MB_ICONERROR);
    return false;
  }

  shader_img = createShader(vertexShader, fragmentShader);

  glGenTextures(1, &screen_texture);
  glGenBuffers(1, &screenVBO);
  glGenVertexArrays(1, &screenVAO);
  glGenBuffers(1, &screenEBO);

  // clang-format off
  float vertices[] = {
      0,                    0,  0,  0,  0,     // top left
      (float)virtualWidth,  0,  0,  1,  0,     // top right
      0, (float)virtualHeight,  0,  0,  1,     // bottom left
      (float)virtualWidth,  (float)virtualHeight, 0,  1,  1 // bottom right
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

  // glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, screen_texture);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, virtualWidth, virtualHeight, 0,
               GL_BGRA, GL_UNSIGNED_BYTE, data);
  glGenerateMipmap(GL_TEXTURE_2D);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);

  glBindTexture(GL_TEXTURE_2D, 0);  // 解绑
  glBindVertexArray(0);             // 解绑

  DeleteObject(hbitmap);
  delete data;

  // these may not be modified
  float ratio[2] = {(float)virtualWidth, (float)virtualHeight};
  glUseProgram(shader_img);
  glUniform2fv(glGetUniformLocation(shader_img, "uResolution"), 1, ratio);
  glUniform2fv(glGetUniformLocation(shader_img, "windowSize"), 1, ratio);

#ifdef FREETYPE
  //& for text
  // https://learnopengl-cn.github.io/06%20In%20Practice/02%20Text%20Rendering/
  char pathBuf[BUF_SIZE] = {};
  GetModuleFileNameA(NULL, pathBuf, BUF_SIZE);
  std::string path(pathBuf);

  auto exePath = file_path(path);
  auto fontPath = file_path(exePath) + "\\fonts\\Px437_Acer_VGA_8x8.ttf";
  FT_Library ft;
  FT_Face face;

  if (FT_Init_FreeType(&ft)) {
    MessageBoxA(NULL, "ERROR::FREETYPE: Could not init FreeType Library",
                "Error", MB_OK | MB_ICONERROR);
    return 1;
  }
  if (FT_New_Face(ft, fontPath.c_str(), 0, &face)) {
    std::string msgerr("failed to load font:");
    msgerr += fontPath;
    MessageBoxA(NULL, msgerr.c_str(), "ERROR", MB_OK | MB_ICONERROR);
    return 1;
  }

  FT_Set_Pixel_Sizes(face, 0, pixel_height);

  shader_txt = createShader(textVertShader, textfragmentShader);

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);  // 禁用字节对齐限制

  for (GLubyte c = 0; c < 128; c++) {
    if (FT_Load_Char(face, c, FT_LOAD_RENDER)) {
      continue;
    }

    GLuint txture;
    glGenTextures(1, &txture);
    glBindTexture(GL_TEXTURE_2D, txture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, face->glyph->bitmap.width,
                 face->glyph->bitmap.rows, 0, GL_RED, GL_UNSIGNED_BYTE,
                 face->glyph->bitmap.buffer);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    Character character = {
        txture, Vec2i(face->glyph->bitmap.width, face->glyph->bitmap.rows),
        Vec2i(face->glyph->bitmap_left, face->glyph->bitmap_top),
        face->glyph->advance.x};

    Characters.insert(std::pair<GLchar, Character>(c, character));
  }

  glBindTexture(GL_TEXTURE_2D, 0);  // 解绑

  FT_Done_Face(face);
  FT_Done_FreeType(ft);

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glGenVertexArrays(1, &textVAO);
  glGenBuffers(1, &textVBO);

  glBindVertexArray(textVAO);
  glBindBuffer(GL_ARRAY_BUFFER, textVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 6 * 4, NULL, GL_DYNAMIC_DRAW);

  glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), 0);
  glEnableVertexAttribArray(0);

  glBindVertexArray(0);  // 解绑

#endif
  //& >>>>>>>>>>>>>>>>>>>>>>>>>>>>>> init opengl

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

#ifdef FREETYPE
        glDeleteBuffers(1, &textVBO);
        glDeleteVertexArrays(1, &textVAO);
#endif

        return 0;
      }
      if (msg.message == WM_HOTKEY && msg.wParam == 1) {
        PostQuitMessage(0);
        continue;
      }
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
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
      switch (wParam) {
        case 'F':
          flashLight.isEnabled = !flashLight.isEnabled;
          break;
        case 'R':
          camera.scale = 1.0f;
          camera.deltaScale = 0.0f;
          camera.position = Vec2f(0.0f, 0.0f);
          camera.velocity = Vec2f(0.0f, 0.0f);

          flashLight.radius = 100.0f;
          flashLight.deltaRadius = 0.0f;
          flashLight.isEnabled = false;
          SetFocus(overlay);
          break;
        case VK_ESCAPE:
          PostQuitMessage(0);
          return 0;
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
    case WM_PAINT: {
      PAINTSTRUCT ps;
      //! 这里必须要手动绘制一下，不然peekmessage不会处理wm_paint事件
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_TIMER: {
      // HDC allDC = GetDC(NULL);
      // GetCursorPos(&mouse_pos);
      // color = GetPixel(allDC, (int)mouse_pos.x, (int)mouse_pos.y);
      // DeleteDC(allDC);

      GetCursorPos(&mouse_pos);

      // 计算从屏幕坐标到原始截图坐标的变换
      // 考虑相机的缩放和平移
      float screenCenterX = virtualWidth * 0.5f;
      float screenCenterY = virtualHeight * 0.5f;

      float screenRelX = (mouse_pos.x - screenCenterX) / camera.scale;
      float screenRelY = (mouse_pos.y - screenCenterY) / camera.scale;

      int originalX = (int)(screenRelX + camera.position.x + screenCenterX);
      int originalY = (int)(screenRelY + camera.position.y + screenCenterY);

      // 确保坐标在有效范围内
      originalX = (originalX < 0)               ? 0
                  : (originalX >= virtualWidth) ? virtualWidth - 1
                                                : originalX;
      originalY = (originalY < 0)                ? 0
                  : (originalY >= virtualHeight) ? virtualHeight - 1
                                                 : originalY;

      color = GetPixel(g_hdc, originalX, originalY);

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

      camera.update(Vec2f(virtualWidth, virtualHeight), dt, isDragging);
      flashLight.update(dt);

      RenderBegin();
      RenderScreen_raw();

#ifdef FREETYPE
      if (flashLight.isEnabled) {
        // auto x = mouse_pos.x;
        // auto y = mouse_pos.y;
        auto r = GetRValue(color);
        auto g = GetGValue(color);
        auto b = GetBValue(color);
        float h, s, v;
        RGBtoHSV(r, g, b, h, s, v);
        int sz;
        sz = std::snprintf(nullptr, 0, "RGB: %d %d %d", r, g, b);
        std::string textbuf(sz + 1, '\0');
        std::sprintf(textbuf.data(), "RGB: %d %d %d", r, g, b);
        float tx = 25.0f;
        float ty = 20.0f;
        float scale = 1.0f;
        float padding = pixel_height * scale * 2;

        // Vec3f color = Vec3f(0.5, 0.8f, 0.2f);
        Vec3f color = Vec3f(r / 255.0f, g / 255.0f, b / 255.0f);

        RenderText(textbuf, tx, ty, scale, color);

        sz = std::snprintf(nullptr, 0, "HEX: #%02X%02X%02X", r, g, b);
        textbuf.resize(sz + 1);
        std::sprintf(textbuf.data(), "HEX: #%02X%02X%02X", r, g, b);

        ty += padding;
        RenderText(textbuf, tx, ty, scale, color);

        sz = std::snprintf(nullptr, 0, "HSV: (%.0f°, %.0f%%, %.0f%%)", h,
                           s * 100, v * 100);
        textbuf.resize(sz + 1);
        std::sprintf(textbuf.data(), "HSV: (%.0f°, %.0f%%, %.0f%%)", h, s * 100,
                     v * 100);
        ty += padding;
        RenderText(textbuf, tx, ty, scale, color);
      }
#endif
      RenderEnd();

      return 0;
    }
    case WM_ERASEBKGND: {
      return 1;
    }
  }
  return DefWindowProc(hwnd, uMsg, wParam, lParam);
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

GLuint createShader(std::string& vert, std::string& frag) {
  GLuint vertex, fragment;
  // vertex shader
  vertex = glCreateShader(GL_VERTEX_SHADER);
  const char* vertSrc = vert.c_str();
  glShaderSource(vertex, 1, &vertSrc, NULL);
  glCompileShader(vertex);

  checkCompileErrors(vertex, "VERTEX");
  // fragment Shader
  const char* fragSrc = frag.c_str();
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

void RenderScreen_raw() {
  // draw screen
  glUseProgram(shader_img);

  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(screenVAO);
  glBindTexture(GL_TEXTURE_2D, screen_texture);

  float cameraPos[2] = {camera.position.x, camera.position.y};
  float mousePos[2] = {(float)mouse_pos.x, (float)mouse_pos.y};

  glUniform1f(glGetUniformLocation(shader_img, "cameraScale"), camera.scale);
  glUniform1f(glGetUniformLocation(shader_img, "flShadow"), flashLight.shadow);
  glUniform1f(glGetUniformLocation(shader_img, "flRadius"), flashLight.radius);

  glUniform2fv(glGetUniformLocation(shader_img, "cameraPos"), 1, cameraPos);
  glUniform2fv(glGetUniformLocation(shader_img, "mousePos"), 1, mousePos);
  glUniform1i(glGetUniformLocation(shader_img, "uTexture"), 0);

  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT,
                 (void*)(0 * sizeof(unsigned int)));

  glBindVertexArray(0);             // 解绑
  glBindTexture(GL_TEXTURE_2D, 0);  // 解绑
}

#ifdef FREETYPE
void RenderText(std::string& text, GLfloat x, GLfloat y, GLfloat scale,
                Vec3f color) {
  // 激活对应的渲染状态
  glUseProgram(shader_txt);

  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(textVAO);
  Mat4 projection = ortho(0, virtualWidth, 0, virtualHeight);

  glUniform1i(glGetUniformLocation(shader_txt, "text"), 0);
  glUniform3f(glGetUniformLocation(shader_txt, "textColor"), color.x, color.y,
              color.z);
  glUniformMatrix4fv(glGetUniformLocation(shader_txt, "projection"), 1,
                     GL_FALSE, projection.m);

  // 遍历文本中所有的字符
  std::string::const_iterator c;
  for (c = text.begin(); c != text.end(); c++) {
    Character ch = Characters[*c];

    GLfloat xpos = x + ch.Bearing.x * scale;
    GLfloat ypos = y - (ch.Size.y - ch.Bearing.y) * scale;

    GLfloat w = ch.Size.x * scale;
    GLfloat h = ch.Size.y * scale;

    // 对每个字符更新VBO
    // clang-format off
    GLfloat vertices[6][4] = {
        {xpos, ypos + h, 0.0, 0.0},
        {xpos, ypos, 0.0, 1.0},
        {xpos + w, ypos, 1.0, 1.0},
        {xpos, ypos + h, 0.0, 0.0},
        {xpos + w, ypos, 1.0, 1.0},
        {xpos + w, ypos + h, 1.0, 0.0}};
    // clang-format on

    // 在四边形上绘制字形纹理
    glBindTexture(GL_TEXTURE_2D, ch.TextureID);
    // 更新VBO内存的内容
    glBindBuffer(GL_ARRAY_BUFFER, textVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    // 绘制四边形
    glDrawArrays(GL_TRIANGLES, 0, 6);
    // 更新位置到下一个字形的原点，注意单位是1/64像素
    x += (ch.Advance >> 6) *
         scale;  // 位偏移6个单位来获取单位为像素的值 (2^6 = 64)
  }
  glBindVertexArray(0);
  glBindTexture(GL_TEXTURE_2D, 0);
}
#endif
