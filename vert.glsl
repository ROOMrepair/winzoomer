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
