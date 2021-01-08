CC = gcc
GLC = glslc

CFLAGS = -Wall -Wno-missing-braces -Wno-attributes -fPIC
LDFLAGS = -L/opt/hfs18.0/dsolib -L/home/michaelb/lib
INFLAGS = -I$(HOME)/dev
LIBS = -lm -lcoal -ltanto -lvulkan -lxcb -lxcb-keysyms -lfreetype
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
		common.h \
		shaders/common.glsl \
		shaders/selcommon.glsl \
		shaders/raycommon.glsl

OBJS =  \
		$(O)/game.o \
		$(O)/render.o \
		$(O)/painter.o \
		$(O)/layer.o

SHADERS =  $(SPV)/post-frag.spv \
		   $(SPV)/comp-frag.spv \
		   $(SPV)/raster-vert.spv \
		   $(SPV)/raster-frag.spv \
		   $(SPV)/raytraceShadow-rmiss.spv \
		   $(SPV)/paint-rgen.spv \
		   $(SPV)/paint-rchit.spv \
		   $(SPV)/paint-rmiss.spv \
		   $(SPV)/select-rgen.spv \
		   $(SPV)/select-rchit.spv \
		   $(SPV)/applyPaint-frag.spv \
		   $(SPV)/layerStack-frag.spv \
		   $(SPV)/select-rmiss.spv

debug: CFLAGS += -g -DVERBOSE=1
debug: all

release: CFLAGS += -DNDEBUG -O3
release: all

all: tanto bin lib tags

shaders: $(SHADERS)

.PHONY: tanto
tanto:
	make -C tanto/ 

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
