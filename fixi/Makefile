CXX=g++
WARNINGS=-Wall -Wextra -pedantic -Wconversion -Wshadow -fsanitize=undefined,address
CXXFLAGS=-std=c++11 -O0 $(WARNINGS) -lpthread

.PHONY: all clean

all: client server

DEPS=tcp_socket.h

client: client.o $(DEPS)
	$(CXX) client.o -o client $(CXXFLAGS)

server: server.o $(DEPS)
	$(CXX) server.o -o tsamP3_GROUP_2 $(CXXFLAGS)

clean:
	$(RM) server.o client.o client tsamP3_GROUP_2
