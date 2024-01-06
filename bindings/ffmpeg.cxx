#include <lauxlib.h>
#include <libavformat/avformat.h>
#include <lua.h>
#include <lualib.h>

#pragma register_fn register_ffmpeg_functions

// clang-format off
#pragma gen_fn avformat_open_input:number lightuserdata string lightuserdata lightuserdata
#pragma gen_fn avformat_close_input lightuserdata
#pragma gen_fn avformat_find_stream_info:number lightuserdata lightuserdata
// clang-format on
