#include "audio/al_util.h"
#include "bindings/ffmpeg.h"
#include "graphics/shader.h"
#include "utils/types.h"
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <GLFW/glfw3.h>
#include <ctype.h>
#include <glad/gles2.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <log.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
#include <timespec.h>
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_EGL
#include <GLFW/glfw3native.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "bindings/gl.h"
#include "media/decode_thread.h"
#include "media/read_thread.h"
#include "utils/threading_utils.h"

lua_State *lua;
static void glfw_error_callback(int error, const char *msg) {
  (void)error;
  log_error("GLFW error: %s", msg);
}

static void glfw_key_callback(GLFWwindow *w, int key, int scancode, int action,
                              int mods) {
  (void)scancode;
  (void)mods;
  if (action == GLFW_RELEASE && (key == 'Q' || key == GLFW_KEY_ESCAPE)) {
    log_debug("Q/Escape pressed. Attempting to close the program");
    glfwSetWindowShouldClose(w, GLFW_TRUE);
  } else if (action == GLFW_RELEASE && key == 'R' &&
             (mods & GLFW_MOD_CONTROL)) {
    lua_settop(lua, 0);
    luaL_dofile(lua, "test.lua");
  }
}

bool callback_ref_init = false;
int callback_ref;
static int lua_registerCallback(lua_State *s) {
  callback_ref = luaL_ref(s, LUA_REGISTRYINDEX);
  callback_ref_init = true;
  return 0;
}

void dump_time(FILE *f, i64 time, AVRational timebase) {
  timebase.num *= 1000;
  i64 ms = av_rescale(time, timebase.num, timebase.den);
  i64 sec = ms / 1000;
  ms %= 1000;
  i64 min = sec / 60;
  sec %= 60;
  i64 hr = min / 60;
  min %= 60;
  fprintf(f, "%.2" PRIi64 ":%.2" PRIi64 ":%.2" PRIi64 ".%.3" PRIi64, hr, min,
          sec, ms);
}

static void log_lock_fn(bool lock, void *arg) {
  mtx_t *m = arg;
  i32 error;
  if (lock) {
    if ((error = mtx_lock(m)) != thrd_success) {
      log_debug("unable to lock log mutex: %s", thrd_error_to_string(error));
    }
  } else {
    if ((error = mtx_unlock(m)) != thrd_success) {
      log_debug("unable to lock log mutex: %s", thrd_error_to_string(error));
    }
  }
}

static void init_logging() {
  static mtx_t log_mutex;
  i32 error;
  if ((error = mtx_init(&log_mutex, mtx_plain)) != thrd_success) {
    log_warn("unable to initialize log mutex: %s", thrd_error_to_string(error));
    return;
  }

  log_set_lock(log_lock_fn, &log_mutex);
}
static void GLAD_API_PTR gl_debug_callback(GLenum source, GLenum type,
                                           GLuint id, GLenum severity,
                                           GLsizei length,
                                           const GLchar *message,
                                           const void *userParam) {
  (void)length;
  (void)userParam;
  i32 level;
  const char *src_str;
  const char *type_str;
  switch (source) {
  case GL_DEBUG_SOURCE_API:
    src_str = "API";
    break;
  case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
    src_str = "WINDOW SYSTEM";
    break;
  case GL_DEBUG_SOURCE_SHADER_COMPILER:
    src_str = "SHADER COMPILER";
    break;
  case GL_DEBUG_SOURCE_THIRD_PARTY:
    src_str = "THIRD PARTY";
    break;
  case GL_DEBUG_SOURCE_APPLICATION:
    src_str = "APPLICATION";
    break;
  default:
    src_str = "OTHER";
    break;
  }

  switch (type) {
  case GL_DEBUG_TYPE_ERROR:
    type_str = "ERROR";
    break;
  case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
    type_str = "DEPRECATED_BEHAVIOR";
    break;
  case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
    type_str = "UNDEFINED_BEHAVIOR";
    break;
  case GL_DEBUG_TYPE_PORTABILITY:
    type_str = "PORTABILITY";
    break;
  case GL_DEBUG_TYPE_PERFORMANCE:
    type_str = "PERFORMANCE";
    break;
  case GL_DEBUG_TYPE_MARKER:
    type_str = "MARKER";
    break;
  default:
    type_str = "OTHER";
    break;
  }

  switch (severity) {
  case GL_DEBUG_SEVERITY_NOTIFICATION:
    level = LOG_INFO;
    break;
  case GL_DEBUG_SEVERITY_LOW:
    level = LOG_DEBUG;
    break;
  case GL_DEBUG_SEVERITY_MEDIUM:
    level = LOG_WARN;
    break;
  case GL_DEBUG_SEVERITY_HIGH:
    level = LOG_ERROR;
    break;
  default:
    level = LOG_FATAL;
    break;
  }

  log_log(level, __FILE__, __LINE__, "%s (source: %s, type: %s, id: %d)",
          message, src_str, type_str, (int)id);
}

static void init_gl_logging() {
  glEnable(GL_DEBUG_OUTPUT);
  glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  glDebugMessageCallback(gl_debug_callback, NULL);
}

void av_log_callback(void *avcl, int level, const char *fmt, va_list vl) {
  if (level > av_log_get_level()) {
    return;
  }
  static char smallbuf[256];
  (void)avcl;
  switch (level) {
  case AV_LOG_PANIC:
  case AV_LOG_FATAL:
    level = LOG_FATAL;
    break;
  case AV_LOG_ERROR:
    level = LOG_ERROR;
    break;
  case AV_LOG_WARNING:
    level = LOG_WARN;
    break;
  case AV_LOG_INFO:
    level = LOG_INFO;
    break;
  case AV_LOG_DEBUG:
    level = LOG_DEBUG;
    break;
  case AV_LOG_VERBOSE:
  case AV_LOG_TRACE:
    level = LOG_TRACE;
    break;
  }

  va_list va;
  va_copy(va, vl);
  i32 len = vsnprintf(NULL, 0, fmt, vl);
  char *buf = len < (i32)sizeof(smallbuf) ? smallbuf : malloc(len + 1);
  if (!buf) {
    buf = smallbuf;
    return;
  }
  vsnprintf(buf, len + 1, fmt, va);
  i32 i = len - 1;
  while (i >= 0 && isspace(buf[i])) {
    buf[i] = '\0';
    --i;
  }
  log_log(level, __FILE__, __LINE__, "%s", buf);
  if (buf != smallbuf) {
    free(buf);
  }
}

struct timespec get_now() {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);
  return ts;
}

int main() {
  init_logging();
  av_log_set_callback(av_log_callback);
  av_log_set_level(AV_LOG_DEBUG);

  if (!glfwInit()) {
    log_fatal("unable to initialize GLFW");
    goto fail_glfw;
  }

  glfwSetErrorCallback(glfw_error_callback);

  glfwDefaultWindowHints();
  glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  GLFWwindow *w = glfwCreateWindow(1280, 720, "preview", NULL, NULL);
  if (!w) {
    log_fatal("unable to create preview window");
    goto fail_window;
  }

  glfwMakeContextCurrent(w);
  glfwSetKeyCallback(w, glfw_key_callback);

  if (!gladLoadGLES2(glfwGetProcAddress)) {
    log_fatal("unable to load OpenGL function pointers");
    goto fail_glad;
  }

  if (!gladLoadEGL(glfwGetEGLDisplay(), glfwGetProcAddress)) {
    log_fatal("unable to load EGL function pointers");
    goto fail_glad;
  }

  init_gl_logging();
  lua = luaL_newstate();
  if (!lua) {
    log_fatal("unable to create lua state");
    goto fail_lua;
  }

  luaL_openlibs(lua);
  register_graphics_functions(lua);
  register_ffmpeg_functions(lua);
  lua_pushcfunction(lua, lua_registerCallback);
  lua_setglobal(lua, "setDrawCallback");
  luaL_dofile(lua, "test.lua");

  AVFormatContext *f;
  avformat_open_input(
      &f,
      "/home/torani/Downloads/[ASW] Tearmoon Teikoku "
      "Monogatari - 12 [1080p HEVC][C6FC48AF].mkv",
      /* "/home/torani/Videos/ortensia3.mkv", */
      /* "/home/torani/OSU IS DYING #osu #osugame #gaming #fyp " */
      /* "[7158923633832824107].mp4", */
      NULL, NULL);
  avformat_find_stream_info(f, NULL);
  read_thread_handle rt;
  i32 streams[] = {READ_THREAD_STREAM_INDEX_AUTO_VIDEO,
                   READ_THREAD_STREAM_INDEX_AUTO_AUDIO};
  i32 num_streams = sizeof(streams) / sizeof(streams[0]);
  stream_info stream_infos[sizeof(streams) / sizeof(streams[0])];
  if (!read_thread_init(&rt,
                        &(read_thread_init_info){
                            .format_context = f,
                            .num_streams = num_streams,
                            .stream_indices = streams,
                            .num_buffered_packets = NULL,
                        },
                        stream_infos)) {
    log_error("unable to start read thread");
  }

  decode_context video, audio;
  if (!decode_context_init(&video, &(decode_thread_init_info){
                                       .fmt = f,
                                       .rt = &rt,
                                       .si = stream_infos[0],
                                       .hwaccel = true,
                                   })) {
    log_error("unable to create video decoding context");
  }
  if (!decode_context_init(&audio, &(decode_thread_init_info){
                                       .fmt = f,
                                       .rt = &rt,
                                       .si = stream_infos[1],
                                   })) {
    log_error("unable to create audio decoding context");
  }

  glfwSwapInterval(1);

  GLuint vao;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);

  shader_manager sm;
  if (!shader_manager_init(&sm, "shaders")) {
    log_error("unable to initialize shader manager");
  }

  shader_program *p = shader_create_vf(&sm, "test.vs.glsl", "test.fs.glsl");
  if (!p) {
    log_error("unable to create shader program");
  }

  ALCdevice *al_device = alcOpenDevice(NULL);
  ALCcontext *al = alcCreateContext(al_device, NULL);
  alcMakeContextCurrent(al);

  ALuint buffers[4], source;
  i32 num_buffers = sizeof(buffers) / sizeof(buffers[0]);
  alGenBuffers(num_buffers, buffers);
  alGenSources(1, &source);

  audio_playback_context apc;
  audio_playback_context_init(&apc, audio);
  i32 samples_per_buffer = apc.frame_size * audio.cc->sample_rate / 60;
  for (i32 i = 0; i < num_buffers; ++i) {
    ALuint buffer = buffers[i];
    audio_playback_context_fill_buffer(&apc, buffer, samples_per_buffer);
    alSourceQueueBuffers(source, 1, &buffer);
  }


  i32 p_local_counter = -1;
  hw_texture cur_frame;
  cur_frame.pixfmt = AV_PIX_FMT_NONE;
  struct timespec start = get_now();
  struct timespec next_pts = get_now();
  alSourcePlay(source);
  while (!glfwWindowShouldClose(w)) {
    glfwPollEvents();

    i32 width, height;
    glfwGetFramebufferSize(w, &width, &height);
    glViewport(0, 0, width, height);

    if (callback_ref_init) {
      lua_rawgeti(lua, LUA_REGISTRYINDEX, callback_ref);
      lua_pushvalue(lua, 1);
      if (lua_pcall(lua, 0, 0, 0) != 0) {
        log_error("error calling lua callback: %s", lua_tostring(lua, -1));
      }
    }

    if (!shader_manager_update(&sm)) {
      log_error("error while updating shaders");
    }

    AVFrame *next_frame = av_frame_alloc();
    while (timespec_ge(get_now(), next_pts)) {
      decode_frame_result r = decode_context_decode_frame(
          &video, next_frame,
          &(decode_frame_info){.packet_receive_info = {
                                   .block = true,
                                   .num_messages = 1,
                               }});
      if (r == DECODE_FRAME_RESULT_SUCCESS) {
        i64 frame_end_pts = next_frame->pts + next_frame->duration;
        AVRational tb = f->streams[video.si.index]->time_base;
        next_pts = timespec_add(
            start, timespec_from_double(frame_end_pts * av_q2d(tb)));
        if (timespec_lt(get_now(), next_pts)) {
          decode_context_map_texture(&video, next_frame, &cur_frame);
          break;
        }
      } else if (r == DECODE_FRAME_RESULT_EOF) {
        glfwSetWindowShouldClose(w, true);
        break;
      } else if (r == DECODE_FRAME_RESULT_ERROR) {
        log_fatal("unable to decode frame");
        glfwSetWindowShouldClose(w, true);
        break;
      }
      av_frame_unref(next_frame);
    }
    av_frame_free(&next_frame);

    // render
    i32 p_counter;
    if (cur_frame.pixfmt != AV_PIX_FMT_NONE &&
        (p_counter = shader_program_use(p))) {
      if (p_counter != p_local_counter) {
        for (i32 i = 0; i < p->num_uniforms; ++i) {
          if (strcmp(p->uniforms[i].name, "y_plane") == 0) {
            glUniform1i(p->uniforms[i].location, 0);
          } else if (strcmp(p->uniforms[i].name, "chroma_plane") == 0) {
            glUniform1i(p->uniforms[i].location, 1);
          }
        }
        p_local_counter = p_counter;
      }

      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, cur_frame.textures[0]);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, cur_frame.textures[1]);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glfwSwapBuffers(w);

    ALint num_processed;
    alGetSourcei(source, AL_BUFFERS_PROCESSED, &num_processed);
    for (i32 i = 0; i < num_processed; ++i) {
      ALuint buffer;
      alSourceUnqueueBuffers(source, 1, &buffer);
      if (audio_playback_context_fill_buffer(&apc, buffer, samples_per_buffer) <
          0) {
        break;
      }
      alSourceQueueBuffers(source, 1, &buffer);
    }
  }

  alDeleteBuffers(num_buffers, buffers);
  alDeleteSources(1, &source);
  alcMakeContextCurrent(NULL);
  alcDestroyContext(al);
  alcCloseDevice(al_device);
  audio_playback_context_free(&apc);

  decode_thread_free_texture(&cur_frame);
  shader_manager_free(&sm);

  decode_context_free(&video);
  read_thread_free(&rt);
  stream_info_free(stream_infos, num_streams);
  avformat_close_input(&f);

  lua_close(lua);
  glfwDestroyWindow(w);
  glfwTerminate();

  return EXIT_SUCCESS;

fail_lua:
fail_glad:
  glfwDestroyWindow(w);
fail_window:
  glfwTerminate();
fail_glfw:
  return EXIT_FAILURE;
}
