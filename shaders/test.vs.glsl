#version 320 es

const vec2 vertices[] = vec2[](
  vec2(-1.0, 1.0),
  vec2(1.0, 1.0),
  vec2(-1.0, -1.0),
  vec2(1.0, -1.0)
);

out vec2 tc;

void main(){
  gl_Position = vec4(vertices[gl_VertexID], 0.0, 1.0);
  tc = vertices[gl_VertexID] * 0.5 + 0.5;
  tc.y = 1.0 - tc.y;
}
