// The HeapsterHelper class is loaded together with the heapster agent
// in order to proxy bytecode-injected calls back into the agent
// library.
//
// TODO: it seems like this should be entirely unnecessary. fix?

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

public class Heapster {
  private static native byte[] _dumpProfile();
  private static native void _newObject(Object thread, Object o);
  private static native void _clearProfile();
  private static native void _setSamplingPeriod(int period);

  public static int isReady = 0;
  public static volatile boolean isProfiling = false;

  public static void start() {
    isProfiling = true;
  }

  public static void stop() {
    isProfiling = false;
  }

  public static void clear() {
    _clearProfile();
  }

  public static void setSamplingPeriod(int period) {
    _setSamplingPeriod(period);
  }

  public static void newObject(Object obj) {
    if (!isProfiling)
      return;

    // TODO: can we get a hold of the sizes here?  if so, we could do
    // the sampling here & never leave bytecode execution.

    Thread thread = Thread.currentThread();
    if (isReady == 1 && thread != null)
      _newObject(thread, obj);
  }

  public static byte[] dumpProfile() {
    return _dumpProfile();
  }

  public static void dumpProfileToFile(String path) throws IOException {
    File file = new File(path);
    FileOutputStream stream = new FileOutputStream(file);
    stream.write(dumpProfile());
    stream.close();
  }

}