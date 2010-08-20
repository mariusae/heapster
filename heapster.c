#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <jvmti.h>


static jvmtiEnv *jvmti = NULL;
jvmtiError jvmti_error;


jvmtiCapabilities *make_capabilities() {
  static jvmtiCapabilities c;

  memset(&c, 0, sizeof(c));
  c.can_generate_vm_object_alloc_events = 1;

  return (&c);
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *jvm, char *options, void *_unused) {
  jvmtiCapabilities *caps = make_capabilities();

  if (((*jvm)->GetEnv(jvm, (void **)&jvmti, JVMTI_VERSION_1_0)) != JNI_OK ||
      jvmti == NULL) {
    fprintf(stderr, "unable to access JVMTI version 1\n");
    exit(1);
  }

  return JNI_OK;
}
