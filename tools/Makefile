# Makefile for Akira Console Simulator Viewer

CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = `pkg-config --cflags --libs sdl2` -lrt -lm

TARGET = akira_viewer
SOURCE = akira_simulator_viewer.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	@echo "🔨 Building Akira Console Simulator Viewer..."
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE) $(LDFLAGS)
	@echo "✅ Build complete!"
	@echo ""
	@echo "🚀 To run the simulator:"
	@echo "   1. ./$(TARGET) &"
	@echo "   2. cd ../build_native_sim/zephyr && ./zephyr.exe"

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
