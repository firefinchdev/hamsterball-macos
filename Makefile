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
