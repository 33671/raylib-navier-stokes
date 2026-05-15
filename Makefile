# Navier-Stokes Fluid Simulation Makefile
# Builds raylib from source and links the fluid simulation

CC       = gcc
CFLAGS   = -Wall -Wextra -O2
LDLIBS   = -lm -lpthread -ldl -lrt -lX11

# Raylib paths
RAYLIB_DIR  = raylib
RAYLIB_SRC  = $(RAYLIB_DIR)/src
RAYLIB_LIB  = $(RAYLIB_DIR)/src/libraylib.a

TARGET = navier_stokes

.PHONY: all clean rebuild

all: $(TARGET)

# Build raylib static library using raylib's own Makefile
$(RAYLIB_LIB):
	cd $(RAYLIB_DIR)/src && $(MAKE) PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC

# Compile main program
main.o: main.c
	$(CC) $(CFLAGS) -I$(RAYLIB_SRC) -c $< -o $@

# Link everything
$(TARGET): main.o $(RAYLIB_LIB)
	$(CC) $(CFLAGS) main.o $(RAYLIB_LIB) $(LDLIBS) -o $@

# Quick rebuild (force recompile main.c only)
rebuild: clean main.o $(RAYLIB_LIB)
	$(CC) $(CFLAGS) main.o $(RAYLIB_LIB) $(LDLIBS) -o $(TARGET)

clean:
	rm -f *.o $(TARGET) helloworld
	cd $(RAYLIB_DIR)/src && $(MAKE) clean
