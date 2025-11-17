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
