#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <jvmti.h>

#include "java_crw_demo.h"

#define HELPER_CLASS  "HeapsterHelper"
#define HELPER_METHOD_NEW_OBJECT "newObject"
#define HELPER_METHOD_NEW_ARRAY "newArray"
#define HELPER_FIELD_IS_READY "isReady"
#define HELPER_FIELD_COUNT "count"

/* XXX - (not yet!) */
/* #define HELPER_NATIVE_NEW_OBJECT "_newObject" */
/* #define HELPER_NATIVE_NEW_ARRAY "_newArray" */

typedef struct {
  jvmtiEnv *jvmti;
  jboolean vm_started;
  jboolean vm_initialized;
  jboolean vm_dead;

  jrawMonitorID monitor;

  jint class_count;
} state_t;

static state_t* state;

static void JNICALL cb_class_file_load_hook(
    jvmtiEnv *, JNIEnv *, jclass, jobject, const char *, jobject,
    jint, const unsigned char *, jint *, unsigned char **);

static void JNICALL cb_vm_start(jvmtiEnv *, JNIEnv *);
static void JNICALL cb_vm_death(jvmtiEnv *, JNIEnv *);

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

void jvmti_errchk(jvmtiEnv *jvmti, jvmtiError err, const char *message) {
  char *strerr;

  if (err == JVMTI_ERROR_NONE)
    return;

  (*jvmti)->GetErrorName(jvmti, err, &strerr);
  errx(3, "jvmti error %s: %s\n", strerr, message);
}

void monitor_lock(jvmtiEnv *jvmti) {
  jvmtiError error;

  error = (*jvmti)->RawMonitorEnter(jvmti, state->monitor);
  jvmti_errchk(jvmti, error, "failed to lock monitor");
}

void monitor_unlock(jvmtiEnv *jvmti) {
  jvmtiError error;

  error = (*jvmti)->RawMonitorExit(jvmti, state->monitor);
  jvmti_errchk(jvmti, error, "failed to unlock monitor");
}

state_t *init_state(jvmtiEnv *jvmti) {
  jvmtiError error;

  state_t *state = calloc(1, sizeof(state_t));
  if (state == NULL)
    errx(3, "malloc failed");

  state->jvmti = jvmti;

  error = (*jvmti)->CreateRawMonitor(
      jvmti, "agent state",
      &(state->monitor));
  jvmti_errchk(jvmti, error, "monitor creation failed");

  return (state);
}

jvmtiCapabilities *make_capabilities() {
  static jvmtiCapabilities c;

  memset(&c, 0, sizeof(c));
  c.can_generate_all_class_hook_events = 1;

  /* XXX */
    c.can_tag_objects  = 1;
    c.can_generate_object_free_events  = 1;
    c.can_get_source_file_name  = 1;
    c.can_get_line_numbers  = 1;
    c.can_generate_vm_object_alloc_events  = 1;


  return (&c);
}

jvmtiEventCallbacks *make_callbacks() {
  static jvmtiEventCallbacks cb;

  memset(&cb, 0, sizeof(cb));
  cb.VMStart = &cb_vm_start;
  cb.VMDeath = &cb_vm_death;
  cb.ClassFileLoadHook = &cb_class_file_load_hook;

  return (&cb);
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *jvm, char *options, void *_unused) {
  jvmtiCapabilities *caps = make_capabilities();
  jvmtiEventCallbacks *cb = make_callbacks();
  jvmtiEnv *jvmti = NULL;
  jvmtiError error;
  jvmtiEvent enable_events[] = {
    JVMTI_EVENT_VM_START, JVMTI_EVENT_VM_DEATH,
    JVMTI_EVENT_CLASS_FILE_LOAD_HOOK
  };
  unsigned int i;

  if (((*jvm)->GetEnv(jvm, (void **)&jvmti, JVMTI_VERSION_1_0)) != JNI_OK ||
      jvmti == NULL) {
    fprintf(stderr, "unable to access JVMTI version 1\n");
    exit(1);
  }

  error = (*jvmti)->AddCapabilities(jvmti, caps);
  jvmti_errchk(jvmti, error, "unable to get JVMTI capabilities");

  error = (*jvmti)->SetEventCallbacks(jvmti, cb, (jint)sizeof(*cb));
  jvmti_errchk(jvmti, error, "failed to set callbacks");

  for (i = 0; i < sizeof(enable_events)/sizeof(*enable_events); i++) {
    error = (*jvmti)->SetEventNotificationMode(
        jvmti, JVMTI_ENABLE, enable_events[i], NULL);
    jvmti_errchk(jvmti, error, "unable to enable event");
  }

  state = init_state(jvmti);

  return JNI_OK;
}

static void JNICALL cb_class_file_load_hook(
    jvmtiEnv *jvmti, JNIEnv* env,
    jclass class_being_redefined, jobject loader,
    const char* name, jobject protection_domain,
    jint class_data_len, const unsigned char* class_data,
    jint* new_class_data_len, unsigned char** new_class_data) {
  /* Serialize access? */
  
  /* TODO: ensure we haven't entered after a vm death event (we need
   * to track vm death) */
  char *classname;

  unsigned char *new_image = NULL;
  long new_length = 0L;
  int is_system_class = 0;
  jint class_num;

  jvmtiError error;

  if (name == NULL) {
    classname = java_crw_demo_classname(class_data, class_data_len, NULL);
    if (classname == NULL)
      errx(3, "Failed to find classname");
  } else {
    if ((classname = strdup(name)) == NULL)
      errx(3, "malloc failed");
  }

  /* Ignore the helper class. */
  if (strcmp(classname, HELPER_CLASS) == 0)
    return;

  {
    monitor_lock(jvmti);
    is_system_class = state->vm_started ? 0 : 1;
    class_num = state->class_count++;
    monitor_unlock(jvmti);
  }

  /* warnx("instrumenting %s (%d)\n", classname, class_num); */

  /* java_crw_demo reentrant? */
  java_crw_demo(
      class_num,
      classname,
      class_data,
      class_data_len,
      is_system_class,
      HELPER_CLASS,
      "L" HELPER_CLASS ";",
      NULL, NULL,
      NULL, NULL,
      HELPER_METHOD_NEW_OBJECT, "(Ljava/lang/Object;)V",
      HELPER_METHOD_NEW_ARRAY,  "(Ljava/lang/Object;)V",
      &new_image,
      &new_length,
      NULL, NULL);

  if (new_length > 0L) {

    /*
     * We got a new class - we need to allocate it via JVMTI & copy it
     * over.
     */
    void *bufp;
    
    error = (*jvmti)->Allocate(jvmti, new_length, (unsigned char **)&bufp);
    jvmti_errchk(jvmti, error, "allocation failed");
    
    memcpy(bufp, new_image, new_length);
    *new_class_data_len = (jint)new_length;
    *new_class_data = bufp;
  }

  if (new_image != NULL)
    free(new_image);
}


static void JNICALL cb_vm_start(jvmtiEnv *jvmti, JNIEnv *env) {
  jclass class;
  jfieldID field;

  if ((class = (*env)->FindClass(env, HELPER_CLASS)) == NULL)
    errx(3, "Failed to find the heapster helper class\n");

  warnx("Found the heapster helper class\n");

  /* This is where we'll have to register our natives. */
  monitor_lock(jvmti);
  state->vm_started = JNI_TRUE;
  monitor_unlock(jvmti);

  /* Tell the helper class that it's ready to go. */
  field = (*env)->GetStaticFieldID(env, class, HELPER_FIELD_IS_READY, "I");
  if (field == NULL)
    errx(3, "Failed to get " HELPER_FIELD_IS_READY " field\n");

  (*env)->SetStaticIntField(env, class, field, 1);
}

static void JNICALL cb_vm_death(jvmtiEnv *jvmti, JNIEnv *env) {
  jint val;
  jclass class;
  jfieldID field;

  if ((class = (*env)->FindClass(env, HELPER_CLASS)) == NULL)
    errx(3, "Failed to find the heapster helper class\n");

  field = (*env)->GetStaticFieldID(env, class, HELPER_FIELD_COUNT, "I");
  if (field == NULL)
    errx(3, "Failed to get " HELPER_FIELD_COUNT " field\n");

  val = (*env)->GetStaticIntField(env, class, field);
  warnx("instrumented %d classes\n", state->class_count/*XXX*/);
  warnx("# of objects allocated: %d\n", val);
}
