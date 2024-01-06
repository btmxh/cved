CC=gcc
CXX=g++
DEBUG ?= 1
ifeq ($(DEBUG), 1)
	DEBUG_FLAGS=-fsanitize=address,leak,undefined -fno-omit-frame-pointer
else
	DEBUG_FLAGS=-DNDEBUG
endif

BINDINGS_CFLAGS = -O0 -ggdb ${DEBUG_FLAGS}
CFLAGS=-Wall -Wextra ${BINDINGS_CFLAGS}

OBJ = main.o utils/mpmc.o media/read_thread.o media/decode_thread.o \
			bindings/gl.o bindings/ffmpeg.o graphics/shader.o utils/filewatch_inotify.o \
			utils/fs_linux.o audio/al_util.o
LIBS=-lglfw -lglad -llog -lm -llua -lavcodec -lavformat -lavutil -lswresample \
		 -ltimespec -lopenal

cved: $(OBJ)
	$(CC) -o $@ $^ $(LIBS) $(CFLAGS)
bindings/%.o: bindings/%.c
	$(CC) -c -o $@ $< $(BINDINGS_CFLAGS)
%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)
bindings/%.c: bindings/%.cxx
	cat $< | python bindings/generate_bindings.py > $@

.PHONY: clean
clean:
	rm -f *.o */**.o bindings/*.c cved
