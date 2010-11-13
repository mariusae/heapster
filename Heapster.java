// The HeapsterHelper class is loaded together with the heapster agent
// in order to proxy bytecode-injected calls back into the agent
// library.
//
// TODO: it seems like this should be entirely unnecessary. fix?

public class Heapster {
  public static int isReady = 0;
  public static volatile boolean isProfiling = false;

  public static void start() {
    isProfiling = true;
  }

  public static void stop() {
    isProfiling = false;
  }

  private static native byte[] _dumpProfile();
  private static native void _newObject(Object thread, Object o);

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
}