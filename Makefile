CC=gcc
JAVA_HOME=/System/Library/Frameworks/JavaVM.framework/Versions/1.6.0/Home
#JAVA_HEADERS=/System/Library/Frameworks/JavaVM.framework/Versions/Current/Headers
JAVA_HEADERS=/Developer/SDKs/MacOSX10.6.sdk/System/Library/Frameworks/JavaVM.framework/Versions/1.6.0/Headers

CFLAGS=-Ijava_crw_demo -fno-strict-aliasing                                  \
        -fPIC -fno-omit-frame-pointer -W -Wall  -Wno-unused -Wno-parentheses \
        -I$(JAVA_HEADERS)
LDFLAGS=-fno-strict-aliasing -fPIC -fno-omit-frame-pointer \
        -static-libgcc -mimpure-text -shared
DEBUG=-g

all: libheapster.jnilib Heapster.class

libheapster.jnilib: heapster.o sampler.o util.o java_crw_demo/java_crw_demo.o
	g++ $(DEBUG) $(LDFLAGS) -o $@ $^ -lc

%.o: %.cc
	g++ $(DEBUG) $(CFLAGS) -o $@ -c $<

%.class: %.java
	javac $<

clean:
	rm -f *.o
	rm -f libheapster.jnilib
