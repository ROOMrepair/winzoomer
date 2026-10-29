#pragma once

#include <string>
#include <glad/glad.h>

const std::string textVertShader = R"(
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

const std::string textfragmentShader = R"(
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

const std::string vertexShader = R"(
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
    vec2 ndc = vec2((((aPos.x - cameraPos.x) / uResolution.x) * 2.0 - 1.0) * cameraScale,
        (((aPos.y + cameraPos.y ) / uResolution.y) * 2.0 - 1.0) * cameraScale);
    gl_Position = vec4(ndc, 0, 1.0);
    TexCoord = aTexCoord;
}
)";

const std::string fragmentShader = R"(
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

const std::string cursorVertShader = R"(
#version 330 core
layout (location = 0) in vec2 vertex;

uniform mat4 projection;

void main()
{
    gl_Position = projection * vec4(vertex.xy, 0.0, 1.0);
}
)";

const std::string cursorFragShader = R"(
#version 330 core
out vec4 color;

uniform vec3 fillColor;

void main()
{
    color = vec4(fillColor, 1.0);
}
)";

GLuint createShader(const std::string& vert, const std::string& frag);
