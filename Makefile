CXX := g++
CXXFLAGS := -std=c++17 -O2 -pthread
LDFLAGS := -pthread

TARGET := client
SOURCES := client.cpp

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
