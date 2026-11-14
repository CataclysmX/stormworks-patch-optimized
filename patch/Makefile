CC      = x86_64-w64-mingw32-gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11 -fno-strict-aliasing
LDFLAGS = -shared -static -static-libgcc -s -Wl,--enable-stdcall-fixup
LIBS    = -lpsapi -lkernel32 -ladvapi32 -lbcrypt

MI      = ../third_party/mimalloc
MIFLAGS = -O2 -std=gnu11 -DNDEBUG -DMI_DEBUG=0 -DMI_SECURE=0 -DMI_STAT=0 -I$(MI)/include -w

SRC = src/fastalloc.c src/dllmain.c src/vehicle_pool.c src/heap_track.c src/physics.c src/narrowphase.c src/crashguard.c src/resolver.c src/sampler.c src/net.c src/branding.c src/editor_fix.c src/unstick.c

CFLAGS += -I$(MI)/include

all: build/OpenAL64.dll build/opengl32.dll

build/mimalloc.o: $(MI)/src/static.c
	mkdir -p build
	$(CC) $(MIFLAGS) -c -o $@ $<

# client: proxy of OpenAL64.dll (stormworks64.exe imports it, server64.exe does not)
build/OpenAL64.dll: $(SRC) src/stwpatch.h src/openal64.def build/mimalloc.o
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC) build/mimalloc.o src/openal64.def $(LDFLAGS) $(LIBS)

# dedicated server: proxy of opengl32.dll (steam_api64.dll is SHA-1 checked by the game: never touch it)
src/opengl32_thunks.S src/opengl32.def: src/opengl32.exports tools_gen_gl.py
	python3 -I tools_gen_gl.py

build/opengl32.dll: $(SRC) src/stwpatch.h src/opengl32_forward.c src/opengl32_thunks.S src/opengl32.def build/mimalloc.o
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC) build/mimalloc.o src/opengl32_forward.c src/opengl32_thunks.S src/opengl32.def $(LDFLAGS) $(LIBS)

# tests: build/narrow_test.exe (run with wine), build/unstick_test (native Linux)
tests: build/narrow_test.exe build/unstick_test

build/narrow_test.exe: test/narrow_test.c src/narrowphase.c src/stwpatch.h
	mkdir -p build
	$(CC) -O2 -Wall -std=gnu11 -Isrc -I$(MI)/include -o $@ test/narrow_test.c

build/unstick_test: test/unstick_test.c src/unstick.c src/stwpatch.h test/linux_shim/windows.h
	mkdir -p build
	gcc -O1 -Wall -Wextra -Itest/linux_shim -o $@ test/unstick_test.c

clean:
	rm -rf build
