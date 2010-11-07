// Heapster is a (sampling) heap profiler for the JVM that is
// output-compatible with google-perftools.
//
// Author: marius a. eriksen <marius@monkey.org>

#include <sys/types.h>
#include <sys/stat.h>

#include <string>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <jvmti.h>
#include <string.h>
#include <fcntl.h>
#include "java_crw_demo.h"

#include <set>
#include <map>

#include "sampler.h"

// NB: only works on little-endian machines
// NB: this is basically a giant race condition right now

using namespace std;

#define arraysize(a) (sizeof(a)/sizeof(*(a)))

void warnx(const char* fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fflush(stderr);
  va_end(ap);
}

void errx(int code, const char* fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fflush(stderr);
  va_end(ap);
  exit(code);
}

// Bogarted from various OpenBSD.
size_t AtomicIO(ssize_t (*f)(int, const void*, size_t),
                int fd, void* _s, size_t n) {
  char* s = reinterpret_cast<char*>(_s);
  size_t pos = 0;
  ssize_t res;
  
  while (n > pos) {
    res = (f) (fd, s + pos, n - pos);
    switch (res) {
      case -1:
        if (errno == EINTR || errno == EAGAIN)
          continue;
        return 0;
      case 0:
        errno = EPIPE;
        return pos;
      default:
        pos += (u_int)res;
    }
  }

  return pos;
}

void AtomicWrite(int fd, char* buf, int n) {
  if (AtomicIO(write, fd, (void*)buf, n) != (size_t)n) {
    perror("AtomicWrite");
    exit(1);
  }
}

class Monitor {
 public:
  inline explicit Monitor(jvmtiEnv* jvmti, const char* descr)
      : jvmti_(jvmti) {
    jvmtiError err = jvmti_->CreateRawMonitor(descr, &raw_monitor_);
    if (err != JVMTI_ERROR_NONE) {
      char* strerr;
      jvmti_->GetErrorName(err, &strerr);
      errx(3, "jvmti error while creating monitor: %s\n", strerr);
    }
  }
        
  jvmtiEnv* jvmti() {
    return jvmti_;
  }

  jrawMonitorID monitor() {
    return raw_monitor_;
  }

 private:
  jvmtiEnv* jvmti_;
  jrawMonitorID raw_monitor_;
};

class Lock {
 public:
  inline explicit Lock(Monitor* monitor) {
    lock(monitor->jvmti(), monitor->monitor());
  }

  inline explicit Lock(jvmtiEnv* jvmti, jrawMonitorID monitor) {
    lock(jvmti, monitor);
  }

  inline ~Lock() {
    jvmtiError error = jvmti_->RawMonitorExit(monitor_);
    if (error != JVMTI_ERROR_NONE)
      errx(3, "Failed to unlock monitor");
  }

 private:
  void lock(jvmtiEnv* jvmti, jrawMonitorID monitor) {
    jvmti_ = jvmti;
    monitor_ = monitor;

    jvmtiError error = jvmti_->RawMonitorEnter(monitor_);
    if (error != JVMTI_ERROR_NONE)
      errx(3, "Failed to lock monitor");
  }

  jvmtiEnv* jvmti_;
  jrawMonitorID monitor_;
};

#define HELPER_CLASS "HeapsterHelper"
#define HELPER_FIELD_ISREADY "isReady"

class Heapster {
 public:
  static const uint32_t kHashTableSize;
  static const uint32_t kMaxStackFrames;

  struct Site {
    Site(Site* _next, long _hash, int _nframes, jvmtiFrameInfo* frames)
        : next(_next), hash(_hash), nframes(_nframes),
          stack(new jmethodID[_nframes]), num_allocs(0), num_bytes(0) {
      for (int i = 0; i < nframes; ++i)
        stack[i] = frames[i].method;
    }

    ~Site() {
      delete[] stack;
    }

    Site*      next;
    long       hash;
    int        nframes;
    jmethodID* stack;

    // Stats.
    int num_allocs;
    int num_bytes;
  };

  typedef pair<Site*, int> Allocation;

  static Heapster* instance;

  // * Static JNI hooks.
  static void JNI_NewObject(
      JNIEnv* env, jclass klass,
      jthread thread, jobject o) {
    instance->NewObject(env, klass, thread, o);
  }

  // * Static JVMTI hooks
  static void JNICALL JVMTI_VMStart(jvmtiEnv* jvmti, JNIEnv* env) {
    instance->VMStart(env);
  }
   
  static void JNICALL JVMTI_VMDeath(jvmtiEnv* jvmti, JNIEnv* env) {
    instance->VMDeath(env);
  }

  static void JNICALL JVMTI_ObjectFree(jvmtiEnv* jvmti, jlong tag) {
    instance->ObjectFree(tag);
  }

  static void JNICALL JVMTI_ClassFileLoadHook(
      jvmtiEnv* jvmti, JNIEnv* env,
      jclass class_being_redefined, jobject loader,
      const char* name, jobject protection_domain,
      jint class_data_len, const unsigned char* class_data,
      jint* new_class_data_len, unsigned char** new_class_data) {
    instance->ClassFileLoadHook(
        jvmti, env, class_being_redefined, loader, name,
        protection_domain, class_data_len, class_data,
        new_class_data_len, new_class_data);
  }

  // * Instance methods.

  Heapster(jvmtiEnv* jvmti)
      : jvmti_(jvmti), monitor_(NULL),
        class_count_(0), vm_started_(false) {
    Setup();
  }

  ~Heapster() {
    // TODO: deallocate raw_monitor.
    // TODO: deallocate sites table.
    delete monitor_;
  }

  void VMStart(JNIEnv* env) {
    jclass klass;
    static JNINativeMethod registry[] = {
      { (char*)"_newObject",
        (char*)"(Ljava/lang/Object;Ljava/lang/Object;)V",
        (void*)&Heapster::JNI_NewObject }
    };

    klass = env->FindClass(HELPER_CLASS);
    if ((klass = env->FindClass(HELPER_CLASS)) == NULL)
      errx(3, "Failed to find the heapster helper class (%s)\n", HELPER_CLASS);

    { // Register natives.
      Lock l(monitor_);
      vm_started_ = true;

      // TODO: Does this need to be inside of the lock?
      if (env->RegisterNatives(klass, registry, arraysize(registry)) != 0)
        errx(3, "Failed to register natives for %s", HELPER_CLASS);
    }

    // Set the static field to hint the helper.
    jfieldID field = env->GetStaticFieldID(klass, HELPER_FIELD_ISREADY, "I");
    if (field == NULL)
      errx(3, "Failed to get %s field\n", HELPER_FIELD_ISREADY);

    env->SetStaticIntField(klass, field, 1);
  }

  void JNICALL VMDeath(JNIEnv* env) {
    char* profile_path = getenv("HEAPPROFILE");
    if (profile_path == NULL)
      return;

    jvmtiError error = jvmti_->ForceGarbageCollection();
    if (error != JVMTI_ERROR_NONE)
      warnx("Failed to force garbage collection.\n");

    int fd = open(
        profile_path, O_WRONLY | O_TRUNC | O_CREAT,
        S_IRUSR | S_IWUSR);
    if (fd < 0) {
      perror("open");
      return;
    }
    FILE* file = fdopen(fd, "w");

    // TODO: change "binary" to main class name?
    fprintf(file, "--- symbol\nbinary=heapster\n");

    // Write out symbol information (traverse the sites & resolve
    // method names.)
    set<jmethodID> seen_methods;
    for (uint32_t i = 0; i < kHashTableSize; ++i) {
      for (Site* s = sites_[i]; s != NULL; s = s->next) {
        for (int i = 0; i < s->nframes; ++i) {
          const jmethodID method = s->stack[i];

          if (seen_methods.find(method) != seen_methods.end())
            continue;

          uintptr_t frame = reinterpret_cast<uintptr_t>(method);
          char* method_name;
          jvmtiError error =
              jvmti_->GetMethodName(method, &method_name, NULL, NULL);
          if (error != JVMTI_ERROR_NONE)
            continue;

          jclass declaring_class;
          error = jvmti_->GetMethodDeclaringClass(method, &declaring_class);
          if (error != JVMTI_ERROR_NONE)
            continue;

          char* class_name;
          error = jvmti_->GetClassSignature(
              declaring_class, &class_name, NULL);
          if (error != JVMTI_ERROR_NONE)
            continue;

#ifdef __x86_64
          fprintf(file, "0x%016lx %s%s\n", frame, class_name, method_name);
#else
          fprintf(file, "0x%08lx %s%s\n", frame, class_name, method_name);
#endif

          seen_methods.insert(method);
        }
      }
    }

    fprintf(file, "---\n");
    fprintf(file, "--- profile\n");
    fflush(file);

    int count = 0;
    int total_num_bytes = 0, total_num_allocs = 0;

    uintptr_t buf[2 + kMaxStackFrames];

    // Write out the header.
    buf[0] = 0;
    buf[1] = 3;
    buf[2] = 0;
    buf[3] = 1;
    buf[4] = 0;

    AtomicWrite(fd, reinterpret_cast<char*>(buf), sizeof(buf[0]) * 5);

    for (uint32_t i = 0; i < kHashTableSize; ++i) {
      for (Site* s = sites_[i]; s != NULL; s = s->next) {
        buf[0] = s->num_bytes;          // nsamples
        buf[1] = s->nframes;            // depth
        memcpy(&buf[2], s->stack, s->nframes * sizeof(s->stack[0]));

        AtomicWrite(
            fd, reinterpret_cast<char*>(buf),
            sizeof(buf[0]) * (2 + s->nframes));

        ++count;
        total_num_bytes += s->num_bytes;
        total_num_allocs += s->num_allocs;
      }
    }

    close(fd);
  }

  void JNICALL ObjectFree(jlong tag) {
    Allocation* alloc = reinterpret_cast<Allocation*>(tag);
    // TODO: remove the site if num_bytes == 0?; keep an alloc object
    // pool?
    (alloc->first)->num_bytes -= alloc->second;
    delete alloc;
  }

  void JNICALL ClassFileLoadHook(
      jvmtiEnv* jvmti, JNIEnv* env,
      jclass class_being_redefined, jobject loader,
      const char* name, jobject protection_domain,
      jint class_data_len, const unsigned char* class_data,
      jint* new_class_data_len, unsigned char** new_class_data) {
    // This is where the magic rewriting happens.

    char* classname;
    if (name == NULL) {
      classname = java_crw_demo_classname(class_data, class_data_len, NULL);
      if (classname == NULL)
        errx(3, "Failed to find classname\n");
    } else {
      if ((classname = strdup(name)) == NULL)
        errx(3, "malloc failed\n");
    }

    // Ignore the helper class.
    if (strcmp(classname, HELPER_CLASS) == 0)
      return;

    int class_num;
    bool is_system_class;
    {
      Lock l(monitor_);
      class_num = class_count_++;
      is_system_class = !vm_started_;
    }

    // The big magic: rewrite the class with our instrumentation.
    unsigned char* new_image = NULL;
    long new_length = 0L;

    java_crw_demo(
      class_num,
      classname,
      class_data,
      class_data_len,
      is_system_class ? 1 : 0,
      (char*)HELPER_CLASS,
      (char*)("L" HELPER_CLASS ";"),
      NULL, NULL,
      NULL, NULL,
      (char*)"newObject", (char*)"(Ljava/lang/Object;)V",
      (char*)"newObject", (char*)"(Ljava/lang/Object;)V",
      &new_image,
      &new_length,
      NULL, NULL);

    if (new_length > 0L) {
      // Success. We now need to allocate it with the JVMTI allocator,
      // copy the definition there, and set the corresponding
      // pointers.
      void* bufp;
      Assert(jvmti_->Allocate(new_length, (unsigned char**)&bufp),
             "failed to allocate buffer for new classfile");

      memcpy(bufp, new_image, new_length);
      *new_class_data_len = (jint)new_length;
      *new_class_data = (unsigned char*)bufp;
    }

    if (new_image != NULL)
      free(new_image);
  }

  void NewObject(JNIEnv* env, jclass klass, jthread thread, jobject o) {
    // Compute the size of the allocation & decide whether to sample
    // it.
    jlong size;
    Assert(jvmti_->GetObjectSize(o, &size),
           "failed to get size of object");

    { Lock l(sampler_monitor_);
      if (!sampler_.SampleAllocation(size))
        return;
    }

    jvmtiFrameInfo frames[kMaxStackFrames];
    jint nframes;

    // Subtract 2 stack frames to start outside of our own code. TODO:
    // use AsyncGetStackTrace?
    jvmtiError error =
        jvmti_->GetStackTrace(
            thread, 2, arraysize(frames),
            frames, &nframes);

    // TODO: keep track of these?
    if (error == JVMTI_ERROR_WRONG_PHASE)
      return;

    // This hash function was adapted from Google perftools.
    long h = 0;
    for (int i = 0; i < nframes; i++) {
      h += reinterpret_cast<uintptr_t>(frames[i].method);
      h += h << 10;
      h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;

    uint32_t bucket = h % kHashTableSize;
    Site* s;
    // TODO: use a concurrent data structure here.
    { Lock l(monitor_);
      s = sites_[bucket];
      for (; s != NULL; s = s->next) {
        if (s->hash == h && s->nframes == nframes) {
          int i = 0;
          for (; i < nframes; i++) {
            if (frames[i].method != s->stack[i]) {
              break;
            }
          }
   
          if (i == nframes)
            break;
        }
      }
   
      if (s == NULL)
        sites_[bucket] = s = new Site(sites_[bucket], h, nframes, frames);
   
      // warnx("object size is: %d\n", size);
   
      s->num_allocs++;
      s->num_bytes += size;
    }

    // Record this allocation (& sampled size) for deallocation.
    Allocation* alloc = new Allocation(s, size);
    jvmti_->SetTag(o, reinterpret_cast<jlong>(alloc));
  }

  jvmtiEnv* jvmti() { return jvmti_; }

 private:
  void Assert(jvmtiError err, string message) {
    char* strerr;

    if (err == JVMTI_ERROR_NONE)
      return;

    jvmti_->GetErrorName(err, &strerr);
    errx(3, "jvmti error %s: %s\n", strerr, message.c_str());
  }

  void Setup() {
    // Initialize the sampler.  If we have multiple samplers (eg. one
    // per thread), we need to initialize them with different seeds.
    char* sample_period_env = getenv("HEAPSTER_SAMPLE_PERIOD");
    int sample_period = 1<<19;  // default: 512 KB
    if (sample_period_env != NULL)
      sample_period = strtoll(sample_period_env, NULL, 10);

    sampler_.Init(123/*seed*/, sample_period);

    jvmtiCapabilities c; 
    memset(&c, 0, sizeof(c));
    c.can_generate_all_class_hook_events = 1;
    c.can_tag_objects                    = 1;
    c.can_generate_object_free_events    = 1;
    Assert(jvmti_->AddCapabilities(&c), "failed to add capabilities");

    jvmtiEventCallbacks cb; 
    memset(&cb, 0, sizeof(cb));
    cb.VMStart           = &Heapster::JVMTI_VMStart;
    cb.VMDeath           = &Heapster::JVMTI_VMDeath;
    cb.ObjectFree        = &Heapster::JVMTI_ObjectFree;
    cb.ClassFileLoadHook = &Heapster::JVMTI_ClassFileLoadHook;
    Assert(jvmti_->SetEventCallbacks(&cb, (jint)sizeof(cb)),
           "failed to set callbacks");

    jvmtiEvent events[] = {
      JVMTI_EVENT_VM_START, JVMTI_EVENT_VM_DEATH,
      JVMTI_EVENT_CLASS_FILE_LOAD_HOOK,
      JVMTI_EVENT_OBJECT_FREE
    };

    for (uint32_t i = 0; i < arraysize(events); i++) {
      Assert(jvmti_->SetEventNotificationMode(JVMTI_ENABLE, events[i], NULL),
             "failed to set event notification mode");
    }

    monitor_ = new Monitor(jvmti_, "heapster state");
    sampler_monitor_ = new Monitor(jvmti_, "sampler state");

    // Set up allocation site table.
    sites_ = new Site*[kHashTableSize];
    memset(sites_, 0, sizeof(Site*) * kHashTableSize);
  }

  jvmtiEnv*         jvmti_;
  Monitor*          monitor_;
  Monitor*          sampler_monitor_;
  Site**            sites_;
  tcmalloc::Sampler sampler_;

  int  class_count_;
  bool vm_started_;
};

// Same hash table size as TCMalloc.
const uint32_t Heapster::kHashTableSize = 179999;
const uint32_t Heapster::kMaxStackFrames = 100;
Heapster* Heapster::instance = NULL;

// This instantiates a singleton for the above heapster class, which
// is used in subsequent hooks.
JNIEXPORT jint JNICALL Agent_OnLoad(
    JavaVM* jvm, char* options, void* _unused) {
  jvmtiEnv* jvmti = NULL;

  if ((jvm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_0)) != JNI_OK ||
      jvmti == NULL) {
    fprintf(stderr, "unable to access JVMTI version 1\n");
    exit(1);
  }

  // Static initialization.
  tcmalloc::Sampler::InitStatics();

  // Create the actual heapster instance & run with it!
  Heapster::instance = new Heapster(jvmti);

  return JNI_OK;
}
