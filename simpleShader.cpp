#include "simpleShader.h"
#include <windows.h>

static GLint checkCompileErrors(GLuint shader, const std::string& type) {
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

  return success;
}

GLuint createShader(const std::string& vert, const std::string& frag) {
  GLuint vertex, fragment;

  vertex = glCreateShader(GL_VERTEX_SHADER);
  const char* vertSrc = vert.c_str();
  glShaderSource(vertex, 1, &vertSrc, NULL);
  glCompileShader(vertex);
  if(!checkCompileErrors(vertex, "VERTEX")){
    glDeleteShader(vertex);
    return 0; 
  };

  const char* fragSrc = frag.c_str();
  fragment = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fragment, 1, &fragSrc, NULL);
  glCompileShader(fragment);
  if(!checkCompileErrors(fragment, "FRAGMENT")){
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return 0;
  };

  GLuint ID = glCreateProgram();
  glAttachShader(ID, vertex);
  glAttachShader(ID, fragment);
  glLinkProgram(ID);
  if(!checkCompileErrors(ID, "PROGRAM")){
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return 0;
  };

  glDeleteShader(vertex);
  glDeleteShader(fragment);
  return ID;
}
