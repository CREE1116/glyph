CXX = c++
CC = cc
AR = ar
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS = -Iinclude -Ithird_party
LDLIBS = -lsqlite3 -lpcre2-8
ifeq ($(shell uname -s),Darwin)
CPPFLAGS += -I/opt/homebrew/include
LDLIBS += -L/opt/homebrew/lib -framework Accelerate
else
LDLIBS += -lblas
endif
CORE_SOURCES = src/parser.cpp src/compiler.cpp src/graph.cpp src/expression.cpp src/synth.cpp runtime/kernel.cpp runtime/tensor.cpp runtime/model.cpp runtime/abi.cpp runtime/weights.cpp runtime/tokenizer.cpp runtime/qwen.cpp
CORE_OBJECTS = $(CORE_SOURCES:%.cpp=build/%.o)

all: build/glyph build/libglyph.a
build/glyph: build/src/main.o build/libglyph.a
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@
build/libglyph.a: $(CORE_OBJECTS)
	$(AR) rcs $@ $^
build/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
build/abi_test.o: tests/abi_test.c include/glyph/runtime.h
	$(CC) -Iinclude -Wall -Wextra -Werror -c $< -o $@
build/abi_test: build/abi_test.o build/libglyph.a
	$(CXX) $^ $(LDLIBS) -o $@
test: all build/abi_test
	python3 tests/test_cli.py
	./build/abi_test
	GLYPH_BIN=build/glyph python3 tests/test_model.py
clean:
	rm -rf build
-include $(CORE_OBJECTS:.o=.d) build/src/main.d
.PHONY: all test clean
