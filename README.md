# Heapster

Heapster provides an agent library to do heap profiling for JVM
processes with output compatible with
[Google perftools](http://code.google.com/p/google-perftools/). The
goal of Heapster is to be able to do meaningful (sampled) heap
profiling in a production setting.

Currently it allows for profiling similar to the TCMalloc library,
e.g.:

    $ HEAPPROFILE=/tmp/OUT java -Xbootclasspath/a:. -agentlib:heapster Test 
    $ pprof /tmp/OUT
    Welcome to pprof!  For help, type 'help'.
    (pprof) top
    Total: 2001520 samples
     2000024  99.9%  99.9%  2000048  99.9% LTest;main
        1056   0.1% 100.0%     1056   0.1% Ljava/lang/Object;
         296   0.0% 100.0%      296   0.0% Ljava/lang/String;toCharArray
         104   0.0% 100.0%      136   0.0% Ljava/lang/Shutdown;

This is still work in progress.
