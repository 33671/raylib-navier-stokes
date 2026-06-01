# Navier-Stokes Fluid Simulation Makefile
# Uses local raylib build (raylib/src/)

CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -I./raylib/src
LDLIBS   = ./raylib/src/libraylib.a -lGL -lm -lpthread -ldl -lrt -lX11

TARGET = navier_stokes

.PHONY: all clean rebuild

all: $(TARGET)

# Compile and link
$(TARGET): main.c
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@

rebuild: clean $(TARGET)

clean:
	rm -f *.o $(TARGET) helloworld
