/*
 * time() that returns SOURCE_DATE_EPOCH, preloaded into mklittlefs only (TIN-7545). mklittlefs stamps the image's
 * root directory with time(NULL) (its main.cpp, the 't' and 'c' attributes of "/"), so without this two rebuilds
 * of one commit differ in that stamp and the metadata CRCs. Built with -nostdlib so it runs inside the
 * PlatformIO binary, which links the host's glibc: `environ` resolves to the host libc at load time.
 */
typedef long time_t;
extern char **environ;

time_t time(time_t *out)
{
  static const char key[] = "SOURCE_DATE_EPOCH=";
  time_t value = 0;
  for (char **e = environ; e != 0 && *e != 0; e++) {
    const char *s = *e;
    int i = 0;
    while (key[i] != 0 && s[i] == key[i]) i++;
    if (key[i] != 0) continue;
    for (s += i; *s >= '0' && *s <= '9'; s++) value = value * 10 + (*s - '0');
    break;
  }
  if (out != 0) *out = value;
  return value;
}
