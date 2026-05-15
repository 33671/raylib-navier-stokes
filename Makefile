# Navier-Stokes Fluid Simulation Makefile
# Uses homebrew-installed raylib via pkg-config

CC       = gcc
PKG      = pkg-config
CFLAGS   = -Wall -Wextra -O2 $(shell $(PKG) --cflags raylib)
LDLIBS   = $(shell $(PKG) --libs raylib) -lm

TARGET = navier_stokes

.PHONY: all clean rebuild

all: $(TARGET)

# Compile and link
$(TARGET): main.c
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@

rebuild: clean $(TARGET)

clean:
	rm -f *.o $(TARGET) helloworld
