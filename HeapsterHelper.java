public class HeapsterHelper {
  public static int isReady = 0;
  public static int count = 0;

  public static void newObject(Object obj) {
    count += 1;
  }

  public static void newArray(Object arr) {
  }
}