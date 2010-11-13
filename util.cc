// Lifted from google-perftools.

#include <string>

using namespace std;

string StringPrintf(const char* format, ...) {
  char buf[10240];
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  return buf;  // implicit conversion
}
