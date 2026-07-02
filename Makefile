CXX := g++

SYSTEMC_HOME ?= /usr/local/systemc-2.3.4
SYSTEMC_INC := $(SYSTEMC_HOME)/include
SYSTEMC_LIB := $(SYSTEMC_HOME)/lib

CXXFLAGS := -std=c++14 -Wall -Wextra -O2 -I$(SYSTEMC_INC)
LDFLAGS := -L$(SYSTEMC_LIB) -Wl,-rpath,$(SYSTEMC_LIB)
LDLIBS := -lsystemc

TARGET := run_cpu
SOURCES := main.cpp
OBJECTS := $(SOURCES:.cpp=.o)

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

%.o: %.cpp *.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJECTS) $(TARGET)
