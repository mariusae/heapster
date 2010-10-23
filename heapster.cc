#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <jvmti.h>
#include <string.h>
#include "java_crw_demo.h"

using std::string;

#define arraysize(a) (sizeof(a)/sizeof(*(a)))

void warnx(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fflush(stderr);
  va_end(ap);
}

void errx(int code, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fflush(stderr);
  va_end(ap);
  exit(code);
}

static void JNICALL VMStartCB(jvmtiEnv *jvmti, JNIEnv *env);
static void JNICALL VMDeathCB(jvmtiEnv *jvmti, JNIEnv *env);
static void JNICALL ObjectFreeCB(jvmtiEnv *jvmti, jlong tag);
static void JNICALL ClassFileLoadHookCB(
    jvmtiEnv* jvmti, JNIEnv* env,
    jclass class_being_redefined, jobject loader,
    const char* name, jobject protection_domain,
    jint class_data_len, const unsigned char* class_data,
    jint* new_class_data_len, unsigned char** new_class_data);
static void NewObjectJNI(JNIEnv* env, jclass klass, jthread thread, jobject o);

class Monitor {
 public:
  inline explicit Monitor(jvmtiEnv* jvmti, jrawMonitorID monitor)
      : jvmti_(jvmti), monitor_(monitor)
  {}
        
  jvmtiEnv* jvmti() {
    return jvmti_;
  }

  jrawMonitorID monitor() {
    return monitor_;
  }

 private:
  jvmtiEnv* jvmti_;
  jrawMonitorID monitor_;
};

class Locker {
 public:
  inline explicit Locker(Monitor* monitor) {
    Locker(monitor->jvmti(), monitor->monitor());
  }

  inline explicit Locker(jvmtiEnv* jvmti, jrawMonitorID monitor)
      : jvmti_(jvmti), monitor_(monitor) {
    jvmtiError error = jvmti_->RawMonitorEnter(monitor_);
    if (error != JVMTI_ERROR_NONE)
      errx(3, "Failed to lock monitor");
  }

  inline ~Locker() {
    jvmtiError error = jvmti_->RawMonitorExit(monitor_);
    if (error != JVMTI_ERROR_NONE)
      errx(3, "Failed to unlock monitor");
  }

 private:
  jvmtiEnv* jvmti_;
  jrawMonitorID monitor_;
};

class Heapster {
 public:
  static const char* const kHelperClass;
  static const char* const kHelperField_IsReady;
  static Heapster* instance;

  // * Static JNI hooks.
  static void JNI_NewObject(
      JNIEnv* env, jclass klass,
      jthread thread, jobject o) {
    warnx("new object!\n");
  }

  // * Static JVMTI hooks
  static void JNICALL JVMTI_VMStart(jvmtiEnv *jvmti, JNIEnv *env) {
    instance->VMStart(env);
  }
   
  static void JNICALL JVMTI_VMDeath(jvmtiEnv *jvmti, JNIEnv *env) {
    // heapster->VMDeath();
  }
   
  static void JNICALL JVMTI_ObjectFree(jvmtiEnv *jvmti, jlong tag) {
    // heapster->ObjectFree(tag);
  }

  static void JNICALL JVMTI_ClassFileLoadHook(
      jvmtiEnv *jvmti, JNIEnv* env,
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

  Heapster(jvmtiEnv* jvmti) : jvmti_(jvmti), vm_started_(false) {
    Setup();
  }

  void VMStart(JNIEnv* env) {
    jclass klass;
    static JNINativeMethod registry[] = {
      { (char*)"_newObject",
        (char*)"(Ljava/lang/Object;Ljava/lang/Object;)V",
        (void *)&Heapster::JNI_NewObject }
    };

    klass = env->FindClass(kHelperClass);
    if ((klass = env->FindClass(kHelperClass)) == NULL)
      errx(3, "Failed to find the heapster helper class (%s)\n", kHelperClass);

    { // Register natives.
      Locker l(monitor_);
      vm_started_ = true;

      if (env->RegisterNatives(klass, registry, arraysize(registry)) != 0)
        errx(3, "Failed to register natives for %s", kHelperClass);
    }

    // Set the static field to hint the helper.
    jfieldID field = env->GetStaticFieldID(klass, kHelperField_IsReady, "I");
    if (field == NULL)
      errx(3, "Failed to get %s field\n", kHelperField_IsReady);

    env->SetStaticIntField(klass, field, 1);
  }

  void JNICALL ClassFileLoadHook(
      jvmtiEnv *jvmti, JNIEnv* env,
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
    if (strcmp(classname, kHelperClass) == 0)
      return;

    

  }

 private:
  void Assert(jvmtiError err, string message) {
    char* strerr;

    if (err == JVMTI_ERROR_NONE)
      return;

    jvmti_->GetErrorName(err, &strerr);
    errx(3, "jvmti error %s: %s\n", strerr, message.c_str());
  }

  void Setup() {
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

    Assert(jvmti_->CreateRawMonitor("heapster state", &raw_monitor_),
           "failed to create heapster monitor");

    monitor_ = new Monitor(jvmti_, raw_monitor_);
  }

  jvmtiEnv*     jvmti_;
  jrawMonitorID raw_monitor_;
  Monitor*      monitor_;

  bool          vm_started_;
};

const char* const Heapster::kHelperClass = "HeapsterHelper";
const char* const Heapster::kHelperField_IsReady = "isReady";
Heapster* Heapster::instance = NULL;

// This instantiates a singleton for the above heapster class, which
// is used in subsequent hooks.
JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* jvm, char* options, void* _unused) {
  jvmtiEnv* jvmti = NULL;

  if ((jvm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_0)) != JNI_OK ||
      jvmti == NULL) {
    fprintf(stderr, "unable to access JVMTI version 1\n");
    exit(1);
  }

  Heapster::instance = new Heapster(jvmti);

  return JNI_OK;
}
