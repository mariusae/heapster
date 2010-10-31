public class HeapsterHelper {
  public static int isReady = 0;

  private static native void _newObject(Object thread, Object o);
  public static void newObject(Object obj) {
    Thread thread = Thread.currentThread();
    if (isReady == 1 && thread != null)
      _newObject(thread, obj);
  }
}