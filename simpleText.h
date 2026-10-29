#pragma once

#include <string>
#include <glad/glad.h>
#include "simpleMath.h"

#define PIXEL_HEIGHT 16

template <class T>
T file_path(T const& path, T const& delims = "/\\") {
  return path.substr(0, path.find_last_of(delims));
}

std::string wCharToUft8(const std::wstring& w);

#ifdef FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include <map>

struct Character {
  GLuint TextureID;
  Vec2i Size;
  Vec2i Bearing;
  FT_Pos Advance;
};
bool InitFreetype();
void ClearFreetype();

void RenderText(std::string& text, GLfloat x, GLfloat y, GLfloat scale,
                Vec3f color, int width, int height);
void SimpleRenderOneLine(int x, int y, int scale, Vec3f color, int w, int h,
                         const char* format, ...);
#endif // FREETYPE
