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

class Locker {
 public:
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

  Heapster(jvmtiEnv* jvmti) : jvmti_(jvmti), vm_started_(false) {
    Setup();
  }

  void VMStart(JNIEnv* env) {
    jclass klass;
    static JNINativeMethod registry[] = {
      { "_newObject", "(Ljava/lang/Object;Ljava/lang/Object;)V",
        (void *)&NewObjectJNI }
    };

    klass = env->FindClass(kHelperClass);
    warnx("klass = %p\n", klass);
    if ((klass = env->FindClass(kHelperClass)) == NULL)
      errx(3, "Failed to find the heapster helper class (%s)\n", kHelperClass);

    warnx("Found the heapster helper class\n");
    
    { // Register natives.
      Locker l(jvmti_, monitor_);
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
    cb.VMStart           = &VMStartCB;
    cb.VMDeath           = &VMDeathCB;
    cb.ObjectFree        = &ObjectFreeCB;
    cb.ClassFileLoadHook = &ClassFileLoadHookCB;
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

    Assert(jvmti_->CreateRawMonitor("heapster state", &monitor_),
           "failed to create heapster monitor");
  }

  jvmtiEnv*     jvmti_;
  jrawMonitorID monitor_;
  bool          vm_started_;
};

const char* const Heapster::kHelperClass = "HeapsterHelper";
const char* const Heapster::kHelperField_IsReady = "isReady";


static Heapster* heapster;

static void JNICALL VMStartCB(jvmtiEnv *jvmti, JNIEnv *env) {
  heapster->VMStart(env);
}

static void JNICALL VMDeathCB(jvmtiEnv *jvmti, JNIEnv *env) {
  // heapster->VMDeath();
}

static void JNICALL ObjectFreeCB(jvmtiEnv *jvmti, jlong tag) {
  // heapster->ObjectFree(tag);
}

static void JNICALL ClassFileLoadHookCB(
    jvmtiEnv *jvmti, JNIEnv* env,
    jclass class_being_redefined, jobject loader,
    const char* name, jobject protection_domain,
    jint class_data_len, const unsigned char* class_data,
    jint* new_class_data_len, unsigned char** new_class_data) {
  
}

// JVMTI
JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* jvm, char* options, void* _unused) {
  jvmtiEnv* jvmti = NULL;

  if ((jvm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_0)) != JNI_OK ||
      jvmti == NULL) {
    fprintf(stderr, "unable to access JVMTI version 1\n");
    exit(1);
  }

  heapster = new Heapster(jvmti);

  printf("Started heapster!\n");
  return JNI_OK;
}

static void NewObjectJNI(JNIEnv* env, jclass klass, jthread thread, jobject o) {  warnx("new object!\n");
}
