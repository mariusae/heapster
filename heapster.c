#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include <jvmti.h>

#include "java_crw_demo.h"
#include "queue.h"

#define  HELPER_CLASS              "HeapsterHelper"
#define  HELPER_METHOD_NEW_OBJECT  "newObject"
#define  HELPER_FIELD_IS_READY     "isReady"
#define  HELPER_FIELD_COUNT        "count"

#define TRACE_HASH_NUM_BUCKETS (1 << 12)
#define TRACE_HASH_MASK        (TRACE_HASH_NUM_BUCKETS - 1)

#define TRACE_NUM_FRAMES 10

typedef struct {
  /* On hit, move the entry to the head of the bucket.  Should be good
   * with tailq?  we should probably use a simpleq or something? */

  TAILQ_ENTRY(trace_t) entry;

  jvmtiFrameInfo frames[TRACE_NUM_FRAMES];
  jint           nframes;
  uint64_t       num_bytes;
} trace_t;

TAILQ_HEAD(traceq, trace_t);
typedef struct traceq traceq_t;

typedef struct {
  jvmtiEnv *jvmti;

  int vm_started;
  int vm_inited;
  int vm_dead;

  uint64_t total_num_bytes;

  /* The lock. */
  jrawMonitorID monitor;

  jint class_count;

  traceq_t traces[TRACE_HASH_NUM_BUCKETS];
} state_t;

static state_t* state;

static void JNICALL cb_class_file_load_hook(
    jvmtiEnv *, JNIEnv *, jclass, jobject, const char *, jobject,
    jint, const unsigned char *, jint *, unsigned char **);
static void JNICALL cb_object_free(jvmtiEnv *, jlong);

static void jni_newObject(JNIEnv *, jclass, jthread, jobject);

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

void jvmtiassert(jvmtiEnv *jvmti, jvmtiError err, const char *message) {
  char *strerr;

  if (err == JVMTI_ERROR_NONE)
    return;

  (*jvmti)->GetErrorName(jvmti, err, &strerr);
  errx(3, "jvmti error %s: %s\n", strerr, message);
}

void monitor_lock(jvmtiEnv *jvmti) {
  jvmtiError error;

  error = (*jvmti)->RawMonitorEnter(jvmti, state->monitor);
  jvmtiassert(jvmti, error, "failed to lock monitor");
}

void monitor_unlock(jvmtiEnv *jvmti) {
  jvmtiError error;

  error = (*jvmti)->RawMonitorExit(jvmti, state->monitor);
  jvmtiassert(jvmti, error, "failed to unlock monitor");
}

state_t *state_get(jvmtiEnv *jvmti) {
  if (jvmti == NULL)
    jvmti = state->jvmti;

  monitor_lock(jvmti);
  return (state);
}

void state_put(jvmtiEnv *jvmti) {
  if (jvmti == NULL)
    jvmti = state->jvmti;

  monitor_unlock(jvmti);
}

state_t *state_init(jvmtiEnv *jvmti) {
  jvmtiError error;
  uint32_t i;

  state_t *state = calloc(1, sizeof(state_t));
  if (state == NULL)
    errx(3, "malloc failed\n");

  state->jvmti = jvmti;

  error = (*jvmti)->CreateRawMonitor(
      jvmti, "agent state",
      &(state->monitor));
  jvmtiassert(jvmti, error, "monitor creation failed");

  for (i = 0; i < (sizeof(state->traces)/sizeof(*state->traces)); i++)
    TAILQ_INIT(&state->traces[i]);

  return (state);
}

/*
 * Dealing with stacks.
 */

trace_t *get_trace(jvmtiEnv *jvmti, jthread thread) {
  static trace_t trace;
  jvmtiError error;
  char *temp;

  if (thread == NULL)
    return (NULL);   /* do something more meaningful here? */

  /* 
   * Start at depth 2 so we skip our own invocations.
   *
   * TODO: use AsyncGetStackTrace()?
   */
  error = (*jvmti)->GetStackTrace(
      jvmti, thread, 2, TRACE_NUM_FRAMES,
      trace.frames, &trace.nframes);
  if (error == JVMTI_ERROR_WRONG_PHASE)
    return (NULL);

  jvmtiassert(jvmti, error, "failed to get stack trace");

  error = (*jvmti)->GetMethodName(
      jvmti, trace.frames[0].method, &temp, NULL, NULL);
  jvmtiassert(jvmti, error, "failed to get frame name");

  warnx("call: %s\n", temp);

  return (NULL);
}

jvmtiCapabilities *make_capabilities() {
  static jvmtiCapabilities c;

  memset(&c, 0, sizeof(c));
  c.can_generate_all_class_hook_events = 1;
  c.can_tag_objects                    = 1;
  c.can_generate_object_free_events    = 1;

  return (&c);
}

jvmtiEventCallbacks *make_callbacks() {
  static jvmtiEventCallbacks cb;

  memset(&cb, 0, sizeof(cb));
  cb.VMStart           = &cb_vm_start;
  cb.VMDeath           = &cb_vm_death;
  cb.ObjectFree        = &cb_object_free;
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
    JVMTI_EVENT_CLASS_FILE_LOAD_HOOK,
    JVMTI_EVENT_OBJECT_FREE
  };
  unsigned int i;

  if (((*jvm)->GetEnv(jvm, (void **)&jvmti, JVMTI_VERSION_1_0)) != JNI_OK ||
      jvmti == NULL) {
    fprintf(stderr, "unable to access JVMTI version 1\n");
    exit(1);
  }

  error = (*jvmti)->AddCapabilities(jvmti, caps);
  jvmtiassert(jvmti, error, "unable to get JVMTI capabilities");

  error = (*jvmti)->SetEventCallbacks(jvmti, cb, (jint)sizeof(*cb));
  jvmtiassert(jvmti, error, "failed to set callbacks");

  for (i = 0; i < sizeof(enable_events)/sizeof(*enable_events); i++) {
    error = (*jvmti)->SetEventNotificationMode(
        jvmti, JVMTI_ENABLE, enable_events[i], NULL);
    jvmtiassert(jvmti, error, "unable to enable event");
  }

  state = state_init(jvmti);

  return (JNI_OK);
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
      errx(3, "Failed to find classname\n");
  } else {
    if ((classname = strdup(name)) == NULL)
      errx(3, "malloc failed\n");
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

  // warnx("instrumenting %s (%d)\n", classname, class_num);

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
      HELPER_METHOD_NEW_OBJECT, "(Ljava/lang/Object;)V",
      &new_image,
      &new_length,
      NULL, NULL);

  if (new_length > 0L) {

    /*
     * We got a new class - we need to allocate it via JVMTI & copy it
     * over.
     *
     * TODO: keep the old class so we can swap it in when it's time to
     * stop profiling. (or rather, keep a cache of translated classes,
     * and swap them in when needed).
     */
    void *bufp;
    
    error = (*jvmti)->Allocate(jvmti, new_length, (unsigned char **)&bufp);
    jvmtiassert(jvmti, error, "allocation failed");
    
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
  static JNINativeMethod registry[] = {
    {"_newObject", "(Ljava/lang/Object;Ljava/lang/Object;)V", (void *)&jni_newObject}
  };

  if ((class = (*env)->FindClass(env, HELPER_CLASS)) == NULL)
    errx(3, "Failed to find the heapster helper class\n");

  warnx("Found the heapster helper class\n");

  monitor_lock(jvmti);
  state->vm_started = 1;

  /* Register our native(s). */
  if ((*env)->RegisterNatives(env, class, registry, sizeof(registry)/sizeof(*registry)) != 0)
    errx(3, "Failed to register natives for " HELPER_CLASS);

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

  monitor_lock(jvmti);
  state->vm_dead = 1;
  
  if ((class = (*env)->FindClass(env, HELPER_CLASS)) == NULL)
    errx(3, "Failed to find the heapster helper class\n");

  field = (*env)->GetStaticFieldID(env, class, HELPER_FIELD_COUNT, "I");
  if (field == NULL)
    errx(3, "Failed to get " HELPER_FIELD_COUNT " field\n");

  val = (*env)->GetStaticIntField(env, class, field);
  warnx("instrumented %d classes\n", state->class_count/*XXX*/);
  warnx("allocated %d bytes\n", state->total_num_bytes/*XXX*/);
  warnx("# of objects allocated: %d\n", val);
  warnx("jlong size is: %d\n", sizeof(jlong));

  monitor_unlock(jvmti);
}

static void JNICALL cb_object_free(jvmtiEnv *jvmti, jlong tag) {
  // warnx("freeing object %d\n", tag);
}

static void
jni_newObject(JNIEnv *env, jclass klass, jthread thread, jobject o)
{
  jlong size;
  jvmtiError error;
  jvmtiEnv *jvmti = state->jvmti;         /* XXX - locking? */

  error = (*jvmti)->GetObjectSize(jvmti, o, &size);
  jvmtiassert(jvmti, error, "failed to get object size");

  state->total_num_bytes += size;

  get_trace(jvmti, thread);

  /* tag = jlong */

  /* well, we've got a tag, and a size.  can we include the size in
   * the tag? */

  (*jvmti)->SetTag(jvmti, o, 123);
  /* if (rand() < RAND_MAX / 2) { */
  /*   (*jvmti)->SetTag(jvmti, o, 123); */
  /* } else { */
  /*   warnx("NOT tagging object!\n"); */
  /* } */

  // warnx("jni_newObject! (%d)\n", size);
}
