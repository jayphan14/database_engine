CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2

dbms: main.o parser.o
	$(CXX) $(CXXFLAGS) -o dbms main.o parser.o

%.o: %.cpp parser.h
	$(CXX) $(CXXFLAGS) -c $<

run: dbms
	./dbms

clean:
	rm -f *.o dbms

.PHONY: run clean
