MAKEFLAGS += -j2

CC=g++
CXXFLAGS=-c -Wall -std=c++11 -O2 -flto
LDFLAGS=    -Wall -std=c++11 -O2 -flto
LIBS= \
   -lboost_system \
   -lboost_filesystem \
   -lpthread
SOURCES=main.cc GPIO.cc
OBJECTS=$(SOURCES:.cc=.o)
EXECUTABLE=GPIO

ARCH := $(shell uname -m)
ifeq ($(ARCH), armv7l)
   CXXFLAGS += -march=armv7-a -mtune=cortex-a8 -mfloat-abi=hard -mfpu=neon
   LDFLAGS  += -march=armv7-a -mtune=cortex-a8 -mfloat-abi=hard -mfpu=neon
endif

lockfree : CXXFLAGS += -DLOCKFREE

all: $(SOURCES) $(EXECUTABLE)

lockfree: $(SOURCES) $(EXECUTABLE) 

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@ $(LIBS)

.cc.o:
	$(CC) $(CXXFLAGS) $< -o $@

clean:
	rm -f GPIO *.o
