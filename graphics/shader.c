#include "shader.h"
#include "../utils/fs.h"
#include <assert.h>
#include <ctype.h>
#include <log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool shader_manager_init(shader_manager *m, const char *shader_dir) {
  m->head = NULL;
  m->shader_dir = path_absolute(shader_dir);
  if (!m->shader_dir) {
    goto fail_strdup_dir;
  }

  m->fw = filewatch_init(shader_dir);
  if (!m->fw) {
    goto fail_filewatch_init;
  }

  return true;

  filewatch_free(m->fw);
fail_filewatch_init:
  free(m->shader_dir);
fail_strdup_dir:
  return false;
}

static void free_uniforms(shader_uniform *uniforms, i32 num_uniforms) {
  for (i32 i = 0; i < num_uniforms; ++i) {
    free(uniforms[i].name);
  }
  free(uniforms);
}

static void free_shader_program(shader_program *program) {
  glDeleteProgram(program->program);
  free_uniforms(program->uniforms, program->num_uniforms);
  for (i32 i = 0; i < program->num_shaders; ++i) {
    free(program->source_paths[i]);
  }
  free(program);
}

void shader_manager_free(shader_manager *m) {
  free(m->shader_dir);
  filewatch_free(m->fw);
  while (m->head) {
    shader_program_destroy(m, m->head);
  }
}

static bool load_gl_program(shader_program *p);
bool shader_manager_update(shader_manager *m) {
  filewatch_event e;
  while (filewatch_poll(m->fw, &e)) {
    log_info("%s %d%d%d%d%d%d", e.name, e.created, e.modified, e.deleted,
             e.movedfrom, e.movedto, e.isdir);
    shader_program *sp = m->head;
    while (sp) {
      for (i32 i = 0; i < sp->num_shaders; ++i) {
        if (strcmp(sp->source_paths[i], e.name) == 0) {
          log_info("reloading shader '%s'", e.name);
          load_gl_program(sp);
          break;
        }
      }
      sp = sp->next;
    }
    filewatch_free_event(m->fw, &e);
  }
  return true;
}

void shader_program_destroy(shader_manager *m, shader_program *program) {
  if (!program) {
    return;
  }

  assert(m && m->head);

  if (program->prev) {
    program->prev->next = program->next;
  }
  if (program->next) {
    program->next->prev = program->prev;
  }
  if (program == m->head) {
    m->head = program->next;
  }

  free_shader_program(program);
}

shader_program *shader_create_vf(shader_manager *m, const char *vs,
                                 const char *fs) {
  return shader_create(m, 2, (GLenum[]){GL_VERTEX_SHADER, GL_FRAGMENT_SHADER},
                       (const char *[]){vs, fs});
}

shader_program *shader_create_compute(shader_manager *m, const char *cs) {
  return shader_create(m, 1, (GLenum[]){GL_COMPUTE_SHADER},
                       (const char *[]){cs});
}

static void log_multiline(int log_level, char *msg, const char *file,
                          int line) {
  char *begin = msg;
  bool have_more = true;
  while (have_more) {
    char *end = strchr(begin, '\n');
    if (!end) {
      end = begin + strlen(begin) + 1;
      have_more = false;
    } else {
      *end = '\0';
    }

    while (end > begin && isspace(*(end - 1))) {
      *end = '\0';
      --end;
    }

    while (begin < end && isspace(*begin)) {
      ++begin;
    }

    log_log(log_level, file, line, "%s", begin);
    begin = end + 1;
  }
}

static const char *shader_type_to_string(GLenum type) {
  switch (type) {
  case GL_VERTEX_SHADER:
    return "vertex";
  case GL_FRAGMENT_SHADER:
    return "fragment";
  case GL_COMPUTE_SHADER:
    return "compute";
  }

  return "unknown";
}

static GLuint load_gl_shader_from_file(GLenum type, const char *path) {
  GLuint shader = glCreateShader(type);
  if (shader == 0) {
    goto fail_create_shader;
  }
  FILE *file = fopen(path, "rb");
  if (!file) {
    goto fail_open_file;
  }

  if (fseek(file, 0, SEEK_END)) {
    goto fail_seek;
  }
  i64 size = ftell(file);
  if (fseek(file, 0, SEEK_SET)) {
    goto fail_seek;
  }

  GLchar *buffer = malloc(size);
  if (!buffer) {
    goto fail_alloc_buffer;
  }

  if ((i64)fread(buffer, 1, size, file) != size) {
    goto fail_fread;
  }

  glShaderSource(shader, 1, (const GLchar *[]){buffer}, (GLint[]){size});
  glCompileShader(shader);
  GLint status, log_length;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (!status) {
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    log_warn("error compiling %s shader at path '%s'",
             shader_type_to_string(type), path);
    char *log = malloc(log_length + 1);
    if (log) {
      glGetShaderInfoLog(shader, log_length + 1, NULL, log);
      log_multiline(LOG_WARN, log, __FILE__, __LINE__);
    } else {
      log_fatal("<unable to allocate log message buffer>");
    }
    goto fail_compile;
  }

  free(buffer);
  fclose(file);
  return shader;

fail_compile:
fail_fread:
  free(buffer);
fail_alloc_buffer:
fail_seek:
  fclose(file);
fail_open_file:
  glDeleteShader(shader);
fail_create_shader:
  return 0;
}

static void delete_attached_shaders(GLuint program) {
  GLuint shaders[MAX_SHADERS_IN_PROGRAM];
  GLsizei num_shaders;
  glGetAttachedShaders(program, MAX_SHADERS_IN_PROGRAM, &num_shaders, shaders);
  for (i32 i = 0; i < (i32)num_shaders; ++i) {
    glDetachShader(program, shaders[i]);
    glDeleteShader(shaders[i]);
  }
}

static bool fetch_uniforms(GLuint program, i32 *num_uniforms,
                           shader_uniform **uniforms) {
  free_uniforms(*uniforms, *num_uniforms);
  glGetProgramiv(program, GL_ACTIVE_UNIFORMS, num_uniforms);
  *uniforms = malloc(*num_uniforms * sizeof(*uniforms[0]));
  if (!*uniforms) {
    goto fail_alloc_structs;
  }

  i32 i;
  for (i = 0; i < *num_uniforms; ++i) {
    shader_uniform *u = &(*uniforms)[i];
    // TODO: driver bug?
    char test[256];
    glGetActiveUniform(program, i, sizeof test, &u->name_len, &u->size,
                       &u->type, test);
    u->name = malloc(u->name_len + 1);
    if (!u->name) {
      goto fail_alloc_names;
    }
    glGetActiveUniform(program, i, u->name_len + 1, &u->name_len, &u->size,
                       &u->type, u->name);
    u->location = glGetUniformLocation(program, u->name);
  }

  return true;

fail_alloc_names:
  for (i32 j = 0; j < i; ++j) {
    free((*uniforms)[i].name);
  }
  free(*uniforms);
fail_alloc_structs:
  *uniforms = NULL;
  *num_uniforms = 0;
  return false;
}

static bool load_gl_program(shader_program *p) {
  GLuint program = glCreateProgram();
  if (program == 0) {
    goto fail_create_program;
  }

  assert(p->num_shaders <= MAX_SHADERS_IN_PROGRAM);
  for (i32 i = 0; i < p->num_shaders; ++i) {
    GLuint shader =
        load_gl_shader_from_file(p->shader_types[i], p->source_paths[i]);
    if (!shader) {
      goto fail_create_shaders;
    }
    glAttachShader(program, shader);
  }

  glLinkProgram(program);
  GLint status, log_length;
  glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (!status) {
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
    log_warn("error compiling shader program, with shader paths:");
    for (i32 i = 0; i < p->num_shaders; ++i) {
      log_warn("- %s", p->source_paths[i]);
    }
    char *log = malloc(log_length + 1);
    if (log) {
      glGetProgramInfoLog(program, log_length + 1, NULL, log);
      log_multiline(LOG_WARN, log, __FILE__, __LINE__);
    } else {
      log_fatal("<unable to allocate log message buffer>");
    }

    goto fail_link_program;
  }

  p->program = program;
  ++p->update_counter;
  delete_attached_shaders(program);
  if (!fetch_uniforms(p->program, &p->num_uniforms, &p->uniforms)) {
    log_warn("error fetching uniform variables from shader program");
  }
  return true;

fail_link_program:
fail_create_shaders:
  delete_attached_shaders(program);
fail_create_program:
  return false;
}

static shader_program *create_shader_program(shader_manager *m, i32 num_shaders,
                                             const GLenum *shader_types,
                                             const char **shader_paths) {
  (void)m;
  assert(num_shaders <= MAX_SHADERS_IN_PROGRAM &&
         "too many shaders, raise the constant MAX_SHADERS_IN_PROGRAM to "
         "increase the limit");
  shader_program *p = malloc(sizeof *p);
  if (!p) {
    return false;
    goto fail_alloc;
  }

  p->prev = NULL;
  p->next = NULL;
  p->num_shaders = num_shaders;
  memcpy(p->shader_types, shader_types, num_shaders * sizeof(shader_types[0]));
  for (i32 i = 0; i < num_shaders; ++i) {
    if (!(p->source_paths[i] =
              path_concat(m->shader_dir, shader_paths[i], false))) {
      goto fail_alloc_paths;
    }
  }
  p->program = 0;
  p->update_counter = 0;
  p->num_uniforms = 0;
  p->uniforms = NULL;

  if (!load_gl_program(p)) {
    log_warn("error loading gl program, program will be in unusable state");
  }
  return p;

fail_alloc_paths:
  free(p);
fail_alloc:
  return NULL;
}

shader_program *shader_create(shader_manager *m, i32 num_shaders,
                              const GLenum *shader_types,
                              const char **shader_paths) {
  shader_program *program =
      create_shader_program(m, num_shaders, shader_types, shader_paths);
  if (!program) {
    return NULL;
  }

  program->prev = NULL;
  program->next = m->head;
  if (m->head) {
    m->head = program;
  }
  m->head = program;
  return program;
}

i32 shader_program_use(shader_program *p) {
  if (p->update_counter > 0) {
    glUseProgram(p->program);
  }

  return p->update_counter;
}
