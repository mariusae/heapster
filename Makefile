CC=g++
JAVA_HOME=/System/Library/Frameworks/JavaVM.framework/Versions/1.6.0/Home
CFLAGS=-Ijava_crw_demo -fno-strict-aliasing                                  \
        -fPIC -fno-omit-frame-pointer -W -Wall  -Wno-unused -Wno-parentheses \
        -I$(JAVA_HOME)/include
LDFLAGS=-fno-strict-aliasing -fPIC -fno-omit-frame-pointer \
        -static-libgcc -mimpure-text -shared
DEBUG=-g

all: libheapster.jnilib HeapsterHelper.class

libheapster.jnilib: heapster.o java_crw_demo/java_crw_demo.o
	$(CC) $(DEBUG) $(LDFLAGS) -o $@ $^ -lc

%.o: %.cc
	$(CC) $(DEBUG) $(CFLAGS) -o $@ -c $<

%.class: %.java
	javac $<

clean:
	rm -f *.o
	rm -f libheapster.jnilib
