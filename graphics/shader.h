#pragma once

#include "../utils/filewatch.h"
#include "../utils/types.h"
#include <glad/gles2.h>

#define MAX_SHADERS_IN_PROGRAM 2
typedef struct {
  GLint location;
  GLenum type;
  GLchar *name;
  GLsizei name_len;
  GLint size;
} shader_uniform;

typedef struct shader_program {
  GLuint program;
  i32 num_shaders;
  GLenum shader_types[MAX_SHADERS_IN_PROGRAM];
  char *source_paths[MAX_SHADERS_IN_PROGRAM];
  i32 num_uniforms;
  shader_uniform *uniforms;
  i32 update_counter;

  struct shader_program *prev;
  struct shader_program *next;
} shader_program;

typedef struct {
  shader_program *head;
  char *shader_dir;
  filewatch *fw;
} shader_manager;

bool shader_manager_init(shader_manager *m, const char *shader_dir);
void shader_manager_free(shader_manager *m);
bool shader_manager_update(shader_manager *m);

shader_program *shader_create(shader_manager *m, i32 num_shaders,
                              const GLenum *shader_types,
                              const char **shader_paths);
shader_program *shader_create_vf(shader_manager *m, const char *vs,
                                 const char *fs);
shader_program *shader_create_compute(shader_manager *m, const char *cs);
void shader_program_destroy(shader_manager *m, shader_program *program);

// 0 -> unusable
// > 0 -> usable, return value is the update counter
i32 shader_program_use(shader_program *p);
