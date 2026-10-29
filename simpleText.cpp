#include "simpleText.h"

#include <libloaderapi.h>
#include <minwindef.h>
#include <windows.h>

std::string wCharToUft8(const std::wstring& w) {
  if (w.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, w.data(), w.size(), nullptr, 0,
                                 nullptr, nullptr);
  std::string result(size, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), result.data(), size,
                      nullptr, nullptr);

  return result;
}

#ifdef FREETYPE

#include "simpleShader.h"

std::map<GLchar, Character> Characters;
GLuint textVAO, textVBO;
GLuint shader_txt;


bool InitFreetype() {
  wchar_t pathBuf[MAX_PATH] = {};
  GetModuleFileNameW(NULL, pathBuf, MAX_PATH);
  std::wstring path(pathBuf);

  std::wstring exePath = file_path(path, std::wstring(L"/\\"));
  std::wstring fontPath = file_path(exePath, std::wstring(L"/\\")) +
                          L"\\fonts\\Px437_Acer_VGA_8x8.ttf";
  FT_Library ft;
  FT_Face face;

  if (FT_Init_FreeType(&ft)) {
    MessageBoxW(NULL, L"ERROR::FREETYPE: Could not init FreeType Library",
                L"Error", MB_OK | MB_ICONERROR);
    return false;
  }

  if (FT_New_Face(ft, wCharToUft8(fontPath).c_str(), 0, &face)) {
    std::wstring msgerr(L"failed to load font:");
    msgerr += fontPath;
    MessageBoxW(NULL, msgerr.c_str(), L"ERROR", MB_OK | MB_ICONERROR);
    return false;
  }

  FT_Set_Pixel_Sizes(face, 0, PIXEL_HEIGHT);

  shader_txt = createShader(textVertShader, textfragmentShader);
  if (shader_txt == 0) {
    return false;
  }

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

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

  glBindTexture(GL_TEXTURE_2D, 0);

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

  glBindVertexArray(0);
  return true;
}

void ClearFreetype() {
  glDeleteBuffers(1, &textVBO);
  glDeleteVertexArrays(1, &textVAO);
};

void RenderText(std::string& text, GLfloat x, GLfloat y, GLfloat scale,
                Vec3f color, int width, int height) {
  glUseProgram(shader_txt);

  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(textVAO);
  Mat4 projection = ortho(0, width, 0, height);

  glUniform1i(glGetUniformLocation(shader_txt, "text"), 0);
  glUniform3f(glGetUniformLocation(shader_txt, "textColor"), color.x, color.y,
              color.z);
  glUniformMatrix4fv(glGetUniformLocation(shader_txt, "projection"), 1,
                     GL_FALSE, projection.m);

  std::string::const_iterator c;
  for (c = text.begin(); c != text.end(); c++) {
    Character ch = Characters[*c];

    GLfloat xpos = x + ch.Bearing.x * scale;
    GLfloat ypos = y - (ch.Size.y - ch.Bearing.y) * scale;

    GLfloat w = ch.Size.x * scale;
    GLfloat h = ch.Size.y * scale;

    GLfloat vertices[6][4] = {
        {xpos, ypos + h, 0.0, 0.0}, {xpos, ypos, 0.0, 1.0},
        {xpos + w, ypos, 1.0, 1.0}, {xpos, ypos + h, 0.0, 0.0},
        {xpos + w, ypos, 1.0, 1.0}, {xpos + w, ypos + h, 1.0, 0.0}};

    glBindTexture(GL_TEXTURE_2D, ch.TextureID);
    glBindBuffer(GL_ARRAY_BUFFER, textVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    x += (ch.Advance >> 6) * scale;
  }
  glBindVertexArray(0);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void SimpleRenderOneLine(int x, int y, int scale, Vec3f color, int w, int h,
                         const char* format, ...) {
  va_list args;
  va_start(args, format);

  va_list argsCopy;
  va_copy(argsCopy, args);
  int length = std::vsnprintf(nullptr, 0, format, argsCopy);
  va_end(argsCopy);  //? 只能用一次
  if (length <= 0) return;

  std::string result(length + 1, '\0');
  va_copy(argsCopy, args);

  std::vsnprintf(result.data(), result.size(), format, argsCopy);

  va_end(argsCopy);

  RenderText(result, x, y, scale, color, w, h);
}

#endif  //  FREETYPE
