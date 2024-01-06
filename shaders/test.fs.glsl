#version 320 es

precision highp float;
in vec2 tc;
out vec4 color;

uniform sampler2D y_plane;
uniform sampler2D chroma_plane;

const mat4 yuv2rgb = mat4(
    vec4(  1.1644,  1.1644,  1.1644,  0.0000 ),
    vec4(  0.0000, -0.2132,  2.1124,  0.0000 ),
    vec4(  1.7927, -0.5329,  0.0000,  0.0000 ),
    vec4( -0.9729,  0.3015, -1.1334,  1.0000 ));

void main() {
  float y = texture(y_plane, tc).r;
  vec2 chroma = texture(chroma_plane, tc).rg;
  color = yuv2rgb * vec4(y, chroma, 1.0);
  // color = vec4(y,y,y, 1.0);
}
