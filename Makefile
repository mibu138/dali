CC = gcc
GLC = glslc

CFLAGS = -Wall -Wno-missing-braces -Wno-attributes -fPIC
LDFLAGS = -L/opt/hfs18.6/dsolib -L/home/michaelb/lib
INFLAGS = -I$(HOME)/dev
LIBS = -lm -lcoal -lobsidian -lvulkan -lxcb -lxcb-keysyms -lfreetype
GLFLAGS = --target-env=vulkan1.2
BIN = bin
LIB = $(HOME)/lib
LIBNAME = painter

O = build
GLSL = shaders
SPV  = shaders/spv

NAME = painter

DEPS =  \
		game.h \
		render.h \
		painter.h \
		layer.h \
		undo.h \
		common.h \
		shaders/common.glsl \
		shaders/selcommon.glsl \
		shaders/raycommon.glsl \
		shaders/brush.glsl

OBJS =  \
		$(O)/game.o \
		$(O)/render.o \
		$(O)/painter.o \
		$(O)/layer.o \
		$(O)/undo.o

FRAG  := $(patsubst %.frag,$(SPV)/%-frag.spv,$(notdir $(wildcard $(GLSL)/*.frag)))
VERT  := $(patsubst %.vert,$(SPV)/%-vert.spv,$(notdir $(wildcard $(GLSL)/*.vert)))
RGEN  := $(patsubst %.rgen,$(SPV)/%-rgen.spv,$(notdir $(wildcard $(GLSL)/*.rgen)))
RCHIT := $(patsubst %.rchit,$(SPV)/%-rchit.spv,$(notdir $(wildcard $(GLSL)/*.rchit)))
RMISS := $(patsubst %.rmiss,$(SPV)/%-rmiss.spv,$(notdir $(wildcard $(GLSL)/*.rmiss)))

debug: CFLAGS += -g -DVERBOSE=1
debug: all

release: CFLAGS += -DNDEBUG -O2
release: all

all: obsidian bin lib tags

shaders: $(FRAG) $(VERT) $(RGEN) $(RCHIT) $(RMISS)

.PHONY: obsidian
obsidian:
	make -C obsidian/ 

clean: 
	rm -f $(O)/* $(LIB)/$(LIBNAME) $(BIN)/* $(SPV)/*

tags:
	ctags -R .

bin: main.c $(OBJS) $(DEPS) shaders
	$(CC) $(CFLAGS) $(INFLAGS) $(LDFLAGS) $(OBJS) $< -o $(BIN)/$(NAME) $(LIBS)

lib: $(OBJS) $(DEPS) shaders
	$(CC) -shared -o $(LIB)/lib$(LIBNAME).so $(OBJS)

staticlib: $(OBJS) $(DEPS) shaders
	ar rcs $(LIB)/lib$(NAME).a $(OBJS)

$(O)/%.o:  %.c $(DEPS)
	$(CC) $(CFLAGS) $(INFLAGS) -c $< -o $@

$(SPV)/%-vert.spv: $(GLSL)/%.vert $(DEPS)
	$(GLC) $(GLFLAGS) $< -o $@

$(SPV)/%-frag.spv: $(GLSL)/%.frag
	$(GLC) $(GLFLAGS) $< -o $@

$(SPV)/%-rchit.spv: $(GLSL)/%.rchit
	$(GLC) $(GLFLAGS) $< -o $@

$(SPV)/%-rgen.spv: $(GLSL)/%.rgen
	$(GLC) $(GLFLAGS) $< -o $@

$(SPV)/%-rmiss.spv: $(GLSL)/%.rmiss
	$(GLC) $(GLFLAGS) $< -o $@
