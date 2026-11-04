# fbuild - build.
#
#   make                 build build/<cxx>-<mode>/fbuild and fbuildd (clang++, debug), ca-lib first
#   make fbuildd         only the language server, build/<cxx>-<mode>/fbuildd
#   make test            run tests/run.sh against the built binary
#   make plugins         build plugins/*.cpp into build/<cxx>-<mode>/plugins/NAME.so
#   make CXX=g++ MODE=release
#   make LINK=libc       link against libc instead of ca's runtime (default: LINK=free, a
#                        static-pie with no libc: libca.a + libca_runtime.a; plugins load
#                        through ca::dl either way)
#   make matrix          clang++ and g++, debug and release
#   make clean           remove this project's build directory
#
# ca-lib is built in the same compiler and mode (NDEBUG changes the layout of
# optional and result, so both sides must agree). ca-lib is the ca-lib/ submodule when it is
# checked out, else ../ca-lib; CA overrides where it lives.

ifeq ($(origin CXX),default)
CXX := clang++
endif
MODE ?= debug
CA   ?= $(abspath $(if $(wildcard ca-lib/src/lib),ca-lib,../ca-lib))

ifneq ($(findstring clang,$(CXX)),)
STD ?= c++2c
else
STD ?= c++23
endif

LINK ?= free

CPPFLAGS := -Isrc -I$(CA)/src/lib -MMD -MP
CXXFLAGS := -std=$(STD) -nostdinc++ -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror -pthread
ifeq ($(LINK),free)
LDFLAGS  := -nostdlib -static-pie -Wl,-u,_start
else ifeq ($(LINK),libc)
LDFLAGS  := -nostdinc++ -pthread
ifneq ($(findstring clang,$(CXX)),)
LDFLAGS  += -nostdlib++
endif
else
$(error LINK must be free or libc)
endif

ifeq ($(MODE),release)
CXXFLAGS += -O2 -DNDEBUG
else ifeq ($(MODE),debug)
CXXFLAGS += -g -O1 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
else
$(error MODE must be debug or release)
endif

BUILD := build/$(notdir $(CXX))-$(MODE)
CALIB := $(CA)/build/$(notdir $(CXX))-$(MODE)/libca.a
# All of libca.a is linked in (--whole-archive), so a plugin can call any ca function, not
# only those fbuild itself uses.
ifeq ($(LINK),free)
CALIBS := -Wl,--whole-archive $(CALIB) -Wl,--no-whole-archive $(CA)/build/$(notdir $(CXX))-$(MODE)/libca_runtime.a -lgcc
else
CALIBS := -Wl,--whole-archive $(CALIB) -Wl,--no-whole-archive
endif

# fbuild and fbuildd (the language server, src/lsp/) share every other source.
SRCS      := $(shell find src -name '*.cpp')
LSP_SRCS  := $(wildcard src/lsp/*.cpp)
CORE_SRCS := $(filter-out src/main.cpp $(LSP_SRCS),$(SRCS))
CORE_OBJS := $(patsubst src/%.cpp,$(BUILD)/obj/%.o,$(CORE_SRCS))
LSP_OBJS  := $(patsubst src/%.cpp,$(BUILD)/obj/%.o,$(LSP_SRCS))
OBJS      := $(patsubst src/%.cpp,$(BUILD)/obj/%.o,$(SRCS))
EXE       := $(BUILD)/fbuild
DAEMON    := $(BUILD)/fbuildd

.PHONY: all ca test matrix clean plugins fbuildd
.DEFAULT_GOAL := all

all: $(EXE) $(DAEMON)

fbuildd: $(DAEMON)

ca:
	@$(MAKE) -s -C $(CA) CXX=$(CXX) MODE=$(MODE)

$(CALIB): ca

# --export-dynamic: plugins loaded by ca::dl bind to what fbuild (and all of ca, linked
# whole) defines.
$(EXE): $(CORE_OBJS) $(BUILD)/obj/main.o $(CALIB)
	$(CXX) $(CORE_OBJS) $(BUILD)/obj/main.o -o $@ $(LDFLAGS) -Wl,--export-dynamic $(CALIBS)

# The language server loads the same plugins, so it exports the same symbols.
$(DAEMON): $(CORE_OBJS) $(LSP_OBJS) $(CALIB)
	$(CXX) $(CORE_OBJS) $(LSP_OBJS) -o $@ $(LDFLAGS) -Wl,--export-dynamic $(CALIBS)

PLUGIN_SRCS := $(wildcard plugins/*.cpp)
PLUGINS     := $(patsubst plugins/%.cpp,$(BUILD)/plugins/%.so,$(PLUGIN_SRCS))
# The plugins tests/plugins.sh loads to check load errors: kept out of plugins/, so that
# pointing FBUILD_PLUGIN_PATH at it loads only working plugins.
TEST_PLUGINS := $(patsubst tests/plugins/%.cpp,$(BUILD)/test-plugins/%.so,$(wildcard tests/plugins/*.cpp))

plugins: $(PLUGINS)

$(BUILD)/plugins/%.so: plugins/%.cpp | ca
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fPIC -shared -nostdlib -fno-stack-protector $< -o $@

$(BUILD)/test-plugins/%.so: tests/plugins/%.cpp | ca
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fPIC -shared -nostdlib -fno-stack-protector $< -o $@

$(BUILD)/obj/%.o: src/%.cpp | ca
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

test: $(EXE) $(DAEMON) $(PLUGINS) $(TEST_PLUGINS)
	@FBUILD=$(EXE) FBUILDD=$(DAEMON) PLUGINS=$(BUILD)/plugins TEST_PLUGINS=$(BUILD)/test-plugins sh tests/run.sh

matrix:
	@$(MAKE) -s CXX=clang++ MODE=debug test
	@$(MAKE) -s CXX=clang++ MODE=release test
	@$(MAKE) -s CXX=g++ MODE=debug test
	@$(MAKE) -s CXX=g++ MODE=release test

clean:
	rm -rf build

-include $(OBJS:.o=.d) $(PLUGINS:.so=.d) $(TEST_PLUGINS:.so=.d)
