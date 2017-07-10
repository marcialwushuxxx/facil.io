/*
Copyright: Boaz segev, 2017
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/

#include "fiobj_types.h"

/* *****************************************************************************
String API
***************************************************************************** */

/** Creates a String object. Remember to use `fiobj_free`. */
fiobj_s *fiobj_str_new(const char *str, size_t len) {
  return fiobj_alloc(FIOBJ_T_STRING, len, (void *)str);
}

/** Creates a copy from an existing String. Remember to use `fiobj_free`. */
fiobj_s *fiobj_str_copy(fiobj_s *src) {
  fio_cstr_s s = fiobj_obj2cstr(src);
  return fiobj_alloc(FIOBJ_T_STRING, s.len, (void *)s.data);
}

/** Creates a String object using a printf like interface. */
__attribute__((format(printf, 1, 0))) fiobj_s *
fiobj_strvprintf(const char *restrict format, va_list argv) {
  fiobj_s *str = NULL;
  va_list argv_cpy;
  va_copy(argv_cpy, argv);
  int len = vsnprintf(NULL, 0, format, argv_cpy);
  va_end(argv_cpy);
  if (len == 0)
    str = fiobj_alloc(FIOBJ_T_STRING, 0, (void *)"");
  if (len <= 0)
    return str;
  str = fiobj_alloc(FIOBJ_T_STRING, len, NULL); /* adds 1 to len, for NUL */
  vsnprintf(((fio_str_s *)(str))->str, len + 1, format, argv);
  return str;
}
__attribute__((format(printf, 1, 2))) fiobj_s *
fiobj_strprintf(const char *restrict format, ...) {
  va_list argv;
  va_start(argv, format);
  fiobj_s *str = fiobj_strvprintf(format, argv);
  va_end(argv);
  return str;
}

/** Creates a buffer String object. Remember to use `fiobj_free`. */
fiobj_s *fiobj_str_buf(size_t capa) {
  if (capa)
    capa = capa - 1;
  else
    capa = 31;
  fiobj_s *str = fiobj_alloc(FIOBJ_T_STRING, capa, NULL);
  fiobj_str_clear(str);
  return str;
}

/** Resizes a String object, allocating more memory if required. */
void fiobj_str_resize(fiobj_s *str, size_t size) {
  if (str->type != FIOBJ_T_STRING)
    return;
  if (((fio_str_s *)str)->capa >= size + 1) {
    ((fio_str_s *)str)->len = size;
    ((fio_str_s *)str)->str[size] = 0;
    return;
  }
  /* it's better to crash than live without memory... */
  ((fio_str_s *)str)->str = realloc(((fio_str_s *)str)->str, size + 1);
  ((fio_str_s *)str)->capa = size + 1;
  ((fio_str_s *)str)->len = size;
  ((fio_str_s *)str)->str[size] = 0;
  return;
}

/** Return's a String's capacity, if any. */
size_t fiobj_str_capa(fiobj_s *str) {
  if (str->type != FIOBJ_T_STRING)
    return 0;
  return ((fio_str_s *)str)->capa;
}

/** Deallocates any unnecessary memory (if supported by OS). */
void fiobj_str_minimize(fiobj_s *str) {
  if (str->type != FIOBJ_T_STRING)
    return;
  ((fio_str_s *)str)->capa = ((fio_str_s *)str)->len + 1;
  ((fio_str_s *)str)->str =
      realloc(((fio_str_s *)str)->str, ((fio_str_s *)str)->capa);
  return;
}

/** Empties a String's data. */
void fiobj_str_clear(fiobj_s *str) {
  if (str->type != FIOBJ_T_STRING)
    return;
  ((fio_str_s *)str)->str[0] = 0;
  ((fio_str_s *)str)->len = 0;
}

/**
 * Writes data at the end of the string, resizing the string as required.
 * Returns the new length of the String
 */
size_t fiobj_str_write(fiobj_s *dest, const char *data, size_t len) {
  if (dest->type != FIOBJ_T_STRING)
    return 0;
  fiobj_str_resize(dest, ((fio_str_s *)dest)->len + len);
  memcpy(((fio_str_s *)dest)->str + ((fio_str_s *)dest)->len - len, data, len);
  ((fio_str_s *)dest)->str[((fio_str_s *)dest)->len] = 0;
  return ((fio_str_s *)dest)->len;
}
/**
 * Writes data at the end of the string, resizing the string as required.
 * Returns the new length of the String
 */
size_t fiobj_str_write2(fiobj_s *dest, const char *format, ...) {
  if (dest->type != FIOBJ_T_STRING)
    return 0;
  va_list argv;
  va_start(argv, format);
  int len = vsnprintf(NULL, 0, format, argv);
  va_end(argv);
  if (len <= 0)
    return ((fio_str_s *)dest)->len;
  fiobj_str_resize(dest, ((fio_str_s *)dest)->len + len);
  va_start(argv, format);
  vsnprintf(((fio_str_s *)(dest))->str + ((fio_str_s *)dest)->len - len,
            len + 1, format, argv);
  va_end(argv);
  ((fio_str_s *)dest)->str[((fio_str_s *)dest)->len] = 0;
  return ((fio_str_s *)dest)->len;
}
/**
 * Writes data at the end of the string, resizing the string as required.
 * Returns the new length of the String
 */
size_t fiobj_str_join(fiobj_s *dest, fiobj_s *obj) {
  if (dest->type != FIOBJ_T_STRING)
    return 0;
  fio_cstr_s o = fiobj_obj2cstr(obj);
  if (o.len == 0)
    return ((fio_str_s *)dest)->len;
  return fiobj_str_write(dest, o.data, o.len);
}
