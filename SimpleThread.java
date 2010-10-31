public class SimpleThread {
      native static void check(Thread thr, ClassLoader cl);
      static MyThread t;
      static char[] hey;

      public static void main(String args[]) throws Throwable{
//          System.loadLibrary("a");

          hey = new char[1000000];

	  t = new MyThread();
          System.out.println("Creating and running 5 threads...");
          for(int i = 0; i < 5; i++) {
	  	Thread thr = new Thread(t,"MyThread"+i);
          	thr.start();

 //         	check(thr, thr.getContextClassLoader());
		try {
                 thr.join();
                } catch (Throwable t) {
                }
	  }

      }
}

class MyThread implements Runnable {

   Thread t;
     
   public MyThread() {
   }
   
   public void run() {
     /* NO-OP */
		try {
                  for (int i = 0; i < 100000; i++) {
                    // System.out.println("allocating..");
                    char[] hey = new char[i];
                    // System.out.println("[done]..");
                  }

                        "a".getBytes("ASCII");
			excep();
			Thread.sleep(1000);
		} catch (java.lang.InterruptedException e){
			e.printStackTrace();
		} catch (Throwable t) {
                }
   }

   public void excep() throws Throwable{

		 throw new Exception("Thread Exception from MyThread");
	}
   }        



