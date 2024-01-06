#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/gles2.h>

#pragma register_fn register_graphics_functions


// clang-format off
#pragma gen_const number GL_COLOR_BUFFER_BIT
#pragma gen_fn glfwInit:boolean
#pragma gen_fn glfwTerminate
#pragma gen_fn glClearColor number number number number
#pragma gen_fn glClear number
#pragma gen_fn glfwGetTime:number
// clang-format on
