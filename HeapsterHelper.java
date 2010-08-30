public class HeapsterHelper {
  public static int isReady = 0;
  public static int count = 0;
  private static boolean isActive = false;

  private static native void _newObject(Object thread, Object o);
  public static void newObject(Object obj) {
    // XXX - per thread, etc?

    // XXX - isActive is a nice big race condition :-)
    if (isActive)                       // don't recurse!
      return;

    if (isReady == 0 || System.out == null || Thread.currentThread() == null)
      return;

    isActive = true;
    count += 1;

    // System.out.println("***");
    // for (StackTraceElement elem : Thread.currentThread().getStackTrace()) {
    //   System.out.println(elem.toString());
    // }

    _newObject(Thread.currentThread(), obj);

    isActive = false;
  }
}