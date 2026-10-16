CXX = clang++
BREW = /opt/homebrew
CXXFLAGS = -std=c++17 -O2 -g -Wall -Wno-unused-function -Wno-deprecated-declarations -DGL_SILENCE_DEPRECATION \
  -Ithird_party/unicorn/include -I$(BREW)/include $(shell pkg-config --cflags sdl2 libopenmpt)
LDFLAGS = third_party/unicorn/lib/libunicorn.a $(shell pkg-config --libs sdl2 libopenmpt) -framework OpenGL
SRCS = $(wildcard src/*.cpp)
OBJS = $(SRCS:src/%.cpp=build/%.o)

hbwrap: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

build/%.o: src/%.cpp src/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf build hbwrap

# ---- statically recompiled CPU (recomp/): the game's x86 code translated to C ------------
# make recomp: regenerate build/recomp/*.c from game/hamsterball.exe and the coverage files
# in recomp/coverage/, then build ./hbwrap-recomp (no Unicorn).
CC = clang
GENFLAGS = -O2 -g0 -w -fno-strict-aliasing -Isrc -Ibuild/recomp
RCXXFLAGS = $(filter-out -Ithird_party/unicorn/include,$(CXXFLAGS)) -DHB_RECOMP
GEN_SRCS = $(wildcard build/recomp/gen_*.c)
GEN_OBJS = $(GEN_SRCS:build/recomp/%.c=build/recomp/%.o)
ROBJS = $(SRCS:src/%.cpp=build/robj/%.o)

recomp-gen:
	build/venv/bin/python recomp/recomp.py game/hamsterball.exe build/recomp $(wildcard recomp/coverage/*.txt)

hbwrap-recomp: $(ROBJS) $(GEN_OBJS)
	$(CXX) -o $@ $(ROBJS) $(GEN_OBJS) $(filter-out third_party/unicorn/lib/libunicorn.a,$(LDFLAGS))

build/robj/%.o: src/%.cpp src/*.h
	@mkdir -p build/robj
	$(CXX) $(RCXXFLAGS) -c $< -o $@

build/recomp/%.o: build/recomp/%.c src/recomp_rt.h
	$(CC) $(GENFLAGS) -c $< -o $@
