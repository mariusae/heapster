CC=gcc
CXX=g++
OS=$(shell uname -s | tr '[A-Z]' '[a-z]')
GENERATED=generated

XXD=xxd
XXD_OPTIONS=-i

ifeq ("$(OS)", "darwin")
CC=clang
CXX=clang++
JAVA_HOME=$(shell /usr/libexec/java_home)
JAVA_HEADERS=$(JAVA_HOME)/include -I$(JAVA_HOME)/include/darwin
OBJ=libheapster.jnilib
endif

ifeq ("$(OS)", "linux")
JAVA_HOME?=/usr/java/default
JAVA_HEADERS=$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux
OBJ=libheapster.so
endif

CFLAGS=-Ijava_crw_demo -fno-strict-aliasing                                  \
        -fPIC -fno-omit-frame-pointer -W -Wall  -Wno-unused -Wno-parentheses \
        -I$(JAVA_HEADERS) -I$(GENERATED)

LDFLAGS=-fno-strict-aliasing -fPIC -fno-omit-frame-pointer \
        -shared -undefined dynamic_lookup
DEBUG=-g

all: Heapster.class $(OBJ)

$(OBJ): heapster.o sampler.o util.o java_crw_demo/java_crw_demo.o
	$(CXX) $(DEBUG) $(LDFLAGS) -o $@ $^ -lc

%.o: %.cc
	$(CXX) $(DEBUG) $(CFLAGS) -o $@ -c $<

%.class: %.java
	javac $<
	$(XXD) $(XXD_OPTIONS) $@ > $(GENERATED)/$*-inl.h

clean:
	rm -f *.o
	rm -f $(OBJ)
	rm -f java_crw_demo/*.o
	rm -f $(GENERATED)/*
	rm -f *.class
