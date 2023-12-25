#include <GL/gl.h>
#include <glad/gles2.h>
#include <libavutil/mathematics.h>
#include <stdio.h>
#include <stdlib.h>

#include <GLFW/glfw3.h>

#include <log.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "bindings/gl.h"
#include "read_thread.h"
#include "threading_utils.h"

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

int main() {
  init_logging();
  if (true) {
    AVFormatContext *f;
    avformat_open_input(&f, "/home/torani/Videos/ortensia.mkv", NULL, NULL);
    avformat_find_stream_info(f, NULL);
    read_thread_handle rt;
    i32 streams[] = {READ_THREAD_STREAM_INDEX_AUTO_VIDEO};
    i32 num_streams = sizeof(streams) / sizeof(streams[0]);
    stream_info stream_infos[sizeof(streams) / sizeof(streams[0])];
    if (!read_thread_init(&rt,
                          &(read_thread_init_info){
                              .format_context = f,
                              .num_streams = num_streams,
                              .stream_indices = streams,
                          },
                          stream_infos)) {
      log_error("unable to start read thread");
    }

    while (true) {
      packet_msg msg;
      if (mpmc_receive(&stream_infos[0].receiver, &(mpmc_receive_info){
                                                      .message_data = &msg,
                                                      .num_messages = 1,
                                                      .block = true,
                                                  }) != 1) {
        log_error("error receiving packets");
        continue;
      }

      if (msg.tag == PACKET_MSG_TAG_PACKET) {
        char buf[256];
        FILE *memfile = fmemopen(buf, sizeof buf, "w");
        fprintf(memfile, "%d", msg.pkt->stream_index);
        fprintf(memfile, " ");
        dump_time(memfile, msg.pkt->pts, f->streams[0]->time_base);
        fprintf(memfile, " ");
        dump_time(memfile, msg.pkt->dts, f->streams[0]->time_base);
        fclose(memfile);
        av_packet_free(&msg.pkt);
        log_debug("%s", buf);
      } else {
        break;
      }
    }

    stream_info_free(stream_infos, num_streams);
    read_thread_free(&rt);
    avformat_close_input(&f);
    return 0;
  }

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
  glfwSwapInterval(1);
  glfwSetKeyCallback(w, glfw_key_callback);

  if (!gladLoadGLES2(glfwGetProcAddress)) {
    log_fatal("unable to load OpenGL function pointers");
    goto fail_glad;
  }

  lua = luaL_newstate();
  if (!lua) {
    log_fatal("unable to create lua state");
    goto fail_lua;
  }

  luaL_openlibs(lua);
  register_graphics_functions(lua);
  lua_pushcfunction(lua, lua_registerCallback);
  lua_setglobal(lua, "setDrawCallback");
  luaL_dofile(lua, "test.lua");

  while (!glfwWindowShouldClose(w)) {
    glfwPollEvents();

    if (callback_ref_init) {
      lua_rawgeti(lua, LUA_REGISTRYINDEX, callback_ref);
      lua_pushvalue(lua, 1);
      if (lua_pcall(lua, 0, 0, 0) != 0) {
        log_error("error calling lua callback: %s", lua_tostring(lua, -1));
      }
    }

    glfwSwapBuffers(w);
  }

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
