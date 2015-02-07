CC=g++
CFLAGS=-c -Wall -std=c++0x -O2
LDFLAGS=-Wall -std=c++0x -O2
LIBS= \
   -lboost_system \
   -lboost_filesystem \
   -lpthread
SOURCES=main.cc GPIO.cc
OBJECTS=$(SOURCES:.cpp=.o)
EXECUTABLE=GPIO
.PHONY: lockfree

ARCH := $(shell uname -m)
ifeq ($(ARCH), armv7l)
   CFLAGS += -march=armv7-a -mtune=cortex-a8 -mfloat-abi=hard -mfpu=neon
   LDFLAGS += -march=armv7-a -mtune=cortex-a8 -mfloat-abi=hard -mfpu=neon
endif


all: $(SOURCES) $(EXECUTABLE)

lockfree: $(SOURCES) $(OBJECTS)
	$(CC) $(LDFLAGS) -DLOCKFREE $(OBJECTS) -o $(EXECUTABLE) $(LIBS) -lboost_atomic

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@ $(LIBS)

.cpp.o:
	$(CC) $(CFLAGS) $< -o $@