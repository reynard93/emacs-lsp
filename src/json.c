/* JSON parsing and serialization.

Copyright (C) 2017-2023 Free Software Foundation, Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.  */

#include <config.h>

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <jansson.h>
#include <string.h>

#include "lisp.h"
#include "intervals.h"
#include "process.h"
#include "spsupr.h"
#include "thread.h"
#include "buffer.h"
#include "coding.h"
#include "spsupr.c"

#define JSON_HAS_ERROR_CODE (JANSSON_VERSION_HEX >= 0x020B00)

const int BUFFER_SIZE = 1000;

#ifdef WINDOWSNT
#include <windows.h>
#include "w32common.h"
#include "w32.h"

DEF_DLL_FN (void, json_set_alloc_funcs,
	    (json_malloc_t malloc_fn, json_free_t free_fn));
DEF_DLL_FN (void, json_delete, (json_t * json));
DEF_DLL_FN (json_t *, json_array, (void) );
DEF_DLL_FN (int, json_array_append_new, (json_t * array, json_t *value));
DEF_DLL_FN (size_t, json_array_size, (const json_t *array));
DEF_DLL_FN (json_t *, json_object, (void) );
DEF_DLL_FN (int, json_object_set_new,
	    (json_t * object, const char *key, json_t *value));
DEF_DLL_FN (json_t *, json_null, (void) );
DEF_DLL_FN (json_t *, json_true, (void) );
DEF_DLL_FN (json_t *, json_false, (void) );
DEF_DLL_FN (json_t *, json_integer, (json_int_t value));
DEF_DLL_FN (json_t *, json_real, (double value));
DEF_DLL_FN (json_t *, json_stringn, (const char *value, size_t len));
DEF_DLL_FN (char *, json_dumps, (const json_t *json, size_t flags));
DEF_DLL_FN (int, json_dump_callback,
	    (const json_t *json, json_dump_callback_t callback, void *data,
	     size_t flags));
DEF_DLL_FN (json_int_t, json_integer_value, (const json_t *integer));
DEF_DLL_FN (double, json_real_value, (const json_t *real));
DEF_DLL_FN (const char *, json_string_value, (const json_t *string));
DEF_DLL_FN (size_t, json_string_length, (const json_t *string));
DEF_DLL_FN (json_t *, json_array_get, (const json_t *array, size_t index));
DEF_DLL_FN (json_t *, json_object_get, (const json_t *object, const char *key));
DEF_DLL_FN (size_t, json_object_size, (const json_t *object));
DEF_DLL_FN (const char *, json_object_iter_key, (void *iter));
DEF_DLL_FN (void *, json_object_iter, (json_t * object));
DEF_DLL_FN (json_t *, json_object_iter_value, (void *iter));
DEF_DLL_FN (void *, json_object_key_to_iter, (const char *key));
DEF_DLL_FN (void *, json_object_iter_next, (json_t * object, void *iter));
DEF_DLL_FN (json_t *, json_loads,
	    (const char *input, size_t flags, json_error_t *error));
DEF_DLL_FN (json_t *, json_load_callback,
	    (json_load_callback_t callback, void *data, size_t flags,
	     json_error_t *error));

/* This is called by json_decref, which is an inline function.  */
void
json_delete (json_t *json)
{
  fn_json_delete (json);
}

static bool json_initialized;

static bool
init_json_functions (void)
{
  HMODULE library = w32_delayed_load (Qjson);

  if (!library)
    return false;

  LOAD_DLL_FN (library, json_set_alloc_funcs);
  LOAD_DLL_FN (library, json_delete);
  LOAD_DLL_FN (library, json_array);
  LOAD_DLL_FN (library, json_array_append_new);
  LOAD_DLL_FN (library, json_array_size);
  LOAD_DLL_FN (library, json_object);
  LOAD_DLL_FN (library, json_object_set_new);
  LOAD_DLL_FN (library, json_null);
  LOAD_DLL_FN (library, json_true);
  LOAD_DLL_FN (library, json_false);
  LOAD_DLL_FN (library, json_integer);
  LOAD_DLL_FN (library, json_real);
  LOAD_DLL_FN (library, json_stringn);
  LOAD_DLL_FN (library, json_dumps);
  LOAD_DLL_FN (library, json_dump_callback);
  LOAD_DLL_FN (library, json_integer_value);
  LOAD_DLL_FN (library, json_real_value);
  LOAD_DLL_FN (library, json_string_value);
  LOAD_DLL_FN (library, json_string_length);
  LOAD_DLL_FN (library, json_array_get);
  LOAD_DLL_FN (library, json_object_get);
  LOAD_DLL_FN (library, json_object_size);
  LOAD_DLL_FN (library, json_object_iter_key);
  LOAD_DLL_FN (library, json_object_iter);
  LOAD_DLL_FN (library, json_object_iter_value);
  LOAD_DLL_FN (library, json_object_key_to_iter);
  LOAD_DLL_FN (library, json_object_iter_next);
  LOAD_DLL_FN (library, json_loads);
  LOAD_DLL_FN (library, json_load_callback);

  init_json ();

  return true;
}

#define json_set_alloc_funcs fn_json_set_alloc_funcs
#define json_array fn_json_array
#define json_array_append_new fn_json_array_append_new
#define json_array_size fn_json_array_size
#define json_object fn_json_object
#define json_object_set_new fn_json_object_set_new
#define json_null fn_json_null
#define json_true fn_json_true
#define json_false fn_json_false
#define json_integer fn_json_integer
#define json_real fn_json_real
#define json_stringn fn_json_stringn
#define json_dumps fn_json_dumps
#define json_dump_callback fn_json_dump_callback
#define json_integer_value fn_json_integer_value
#define json_real_value fn_json_real_value
#define json_string_value fn_json_string_value
#define json_string_length fn_json_string_length
#define json_array_get fn_json_array_get
#define json_object_get fn_json_object_get
#define json_object_size fn_json_object_size
#define json_object_iter_key fn_json_object_iter_key
#define json_object_iter fn_json_object_iter
#define json_object_iter_value fn_json_object_iter_value
#define json_object_key_to_iter fn_json_object_key_to_iter
#define json_object_iter_next fn_json_object_iter_next
#define json_loads fn_json_loads
#define json_load_callback fn_json_load_callback

#endif /* WINDOWSNT */

/* We install a custom allocator so that we can avoid objects larger
   than PTRDIFF_MAX.  Such objects wouldn't play well with the rest of
   Emacs's codebase, which generally uses ptrdiff_t for sizes and
   indices.  The other functions in this file also generally assume
   that size_t values never exceed PTRDIFF_MAX.

   In addition, we need to use a custom allocator because on
   MS-Windows we replace malloc/free with our own functions, see
   w32heap.c, so we must force the library to use our allocator, or
   else we won't be able to free storage allocated by the library.  */

static void *
json_malloc (size_t size)
{
  if (size > PTRDIFF_MAX)
    {
      errno = ENOMEM;
      return NULL;
    }
  return malloc (size);
}

static void
json_free (void *ptr)
{
  free (ptr);
}

void
init_json (void)
{
  json_set_alloc_funcs (json_malloc, json_free);
}

#if !JSON_HAS_ERROR_CODE

/* Return whether STRING starts with PREFIX.  */

static bool
json_has_prefix (const char *string, const char *prefix)
{
  return strncmp (string, prefix, strlen (prefix)) == 0;
}

/* Return whether STRING ends with SUFFIX.  */

static bool
json_has_suffix (const char *string, const char *suffix)
{
  size_t string_len = strlen (string);
  size_t suffix_len = strlen (suffix);
  return string_len >= suffix_len
	 && memcmp (string + string_len - suffix_len, suffix, suffix_len) == 0;
}

#endif

/* Note that all callers of make_string_from_utf8 and build_string_from_utf8
   below either pass only value UTF-8 strings or use the functionf for
   formatting error messages; in the latter case correctness isn't
   critical.  */

/* Return a unibyte string containing the sequence of UTF-8 encoding
   units of the UTF-8 representation of STRING.  If STRING does not
   represent a sequence of Unicode scalar values, return a string with
   unspecified contents.  */

static Lisp_Object
json_encode (Lisp_Object string)
{
  /* FIXME: Raise an error if STRING is not a scalar value
     sequence.  */
  return encode_string_utf_8 (string, Qnil, false, Qt, Qt);
}

static AVOID
json_out_of_memory (void)
{
  xsignal0 (Qjson_out_of_memory);
}

/* Signal a Lisp error corresponding to the JSON ERROR.  */

static AVOID
json_parse_error (const json_error_t *error)
{
  Lisp_Object symbol;
#if JSON_HAS_ERROR_CODE
  switch (json_error_code (error))
    {
    case json_error_premature_end_of_input:
      symbol = Qjson_end_of_file;
      break;
    case json_error_end_of_input_expected:
      symbol = Qjson_trailing_content;
      break;
    default:
      symbol = Qjson_parse_error;
      break;
    }
#else
  if (json_has_suffix (error->text, "expected near end of file"))
    symbol = Qjson_end_of_file;
  else if (json_has_prefix (error->text, "end of file expected"))
    symbol = Qjson_trailing_content;
  else
    symbol = Qjson_parse_error;
#endif
  xsignal (symbol,
	   list5 (build_string_from_utf8 (error->text),
		  build_string_from_utf8 (error->source),
		  INT_TO_INTEGER (error->line), INT_TO_INTEGER (error->column),
		  INT_TO_INTEGER (error->position)));
}

static void
json_release_object (void *object)
{
  json_decref (object);
}

/* Signal an error if OBJECT is not a string, or if OBJECT contains
   embedded null characters.  */

static void
check_string_without_embedded_nulls (Lisp_Object object)
{
  CHECK_STRING (object);
  CHECK_TYPE (memchr (SDATA (object), '\0', SBYTES (object)) == NULL,
	      Qstring_without_embedded_nulls_p, object);
}

/* Signal an error of type `json-out-of-memory' if OBJECT is
   NULL.  */

static json_t *
json_check (json_t *object)
{
  if (object == NULL)
    json_out_of_memory ();
  return object;
}

/* If STRING is not a valid UTF-8 string, signal an error of type
   `wrong-type-argument'.  STRING must be a unibyte string.  */

static void
json_check_utf8 (Lisp_Object string)
{
  CHECK_TYPE (utf8_string_p (string), Qutf_8_string_p, string);
}

enum json_object_type
{
  json_object_hashtable,
  json_object_alist,
  json_object_plist
};

enum json_array_type
{
  json_array_array,
  json_array_list
};

struct json_configuration
{
  enum json_object_type object_type;
  enum json_array_type array_type;
  Lisp_Object null_object;
  Lisp_Object false_object;
};

static json_t *
lisp_to_json (Lisp_Object, const struct json_configuration *conf);

/* Convert a Lisp object to a nonscalar JSON object (array or object).  */

static json_t *
lisp_to_json_nonscalar_1 (Lisp_Object lisp,
			  const struct json_configuration *conf)
{
  json_t *json;
  specpdl_ref count;

  if (VECTORP (lisp))
    {
      ptrdiff_t size = ASIZE (lisp);
      json = json_check (json_array ());
      count = SPECPDL_INDEX ();
      record_unwind_protect_ptr (json_release_object, json);
      for (ptrdiff_t i = 0; i < size; ++i)
	{
	  int status
	    = json_array_append_new (json, lisp_to_json (AREF (lisp, i), conf));
	  if (status == -1)
	    json_out_of_memory ();
	}
      eassert (json_array_size (json) == size);
    }
  else if (HASH_TABLE_P (lisp))
    {
      struct Lisp_Hash_Table *h = XHASH_TABLE (lisp);
      json = json_check (json_object ());
      count = SPECPDL_INDEX ();
      record_unwind_protect_ptr (json_release_object, json);
      for (ptrdiff_t i = 0; i < HASH_TABLE_SIZE (h); ++i)
	{
	  Lisp_Object key = HASH_KEY (h, i);
	  if (!BASE_EQ (key, Qunbound))
	    {
	      CHECK_STRING (key);
	      Lisp_Object ekey = json_encode (key);
	      /* We can't specify the length, so the string must be
		 null-terminated.  */
	      check_string_without_embedded_nulls (ekey);
	      const char *key_str = SSDATA (ekey);
	      /* Reject duplicate keys.  These are possible if the hash
		 table test is not `equal'.  */
	      if (json_object_get (json, key_str) != NULL)
		wrong_type_argument (Qjson_value_p, lisp);
	      int status
		= json_object_set_new (json, key_str,
				       lisp_to_json (HASH_VALUE (h, i), conf));
	      if (status == -1)
		{
		  /* A failure can be caused either by an invalid key or
		   by low memory.  */
		  json_check_utf8 (ekey);
		  json_out_of_memory ();
		}
	    }
	}
    }
  else if (NILP (lisp))
    return json_check (json_object ());
  else if (CONSP (lisp))
    {
      Lisp_Object tail = lisp;
      json = json_check (json_object ());
      count = SPECPDL_INDEX ();
      record_unwind_protect_ptr (json_release_object, json);
      bool is_plist = !CONSP (XCAR (tail));
      FOR_EACH_TAIL (tail)
      {
	const char *key_str;
	Lisp_Object value;
	Lisp_Object key_symbol;
	if (is_plist)
	  {
	    key_symbol = XCAR (tail);
	    tail = XCDR (tail);
	    CHECK_CONS (tail);
	    value = XCAR (tail);
	  }
	else
	  {
	    Lisp_Object pair = XCAR (tail);
	    CHECK_CONS (pair);
	    key_symbol = XCAR (pair);
	    value = XCDR (pair);
	  }
	CHECK_SYMBOL (key_symbol);
	Lisp_Object key = SYMBOL_NAME (key_symbol);
	/* We can't specify the length, so the string must be
	   null-terminated.  */
	check_string_without_embedded_nulls (key);
	key_str = SSDATA (key);
	/* In plists, ensure leading ":" in keys is stripped.  It
	   will be reconstructed later in `json_to_lisp'.*/
	if (is_plist && ':' == key_str[0] && key_str[1])
	  {
	    key_str = &key_str[1];
	  }
	/* Only add element if key is not already present.  */
	if (json_object_get (json, key_str) == NULL)
	  {
	    int status
	      = json_object_set_new (json, key_str, lisp_to_json (value, conf));
	    if (status == -1)
	      json_out_of_memory ();
	  }
      }
      CHECK_LIST_END (tail, lisp);
    }
  else
    wrong_type_argument (Qjson_value_p, lisp);

  clear_unwind_protect (count);
  unbind_to (count, Qnil);
  return json;
}

/* Convert LISP to a nonscalar JSON object (array or object).  Signal
   an error of type `wrong-type-argument' if LISP is not a vector,
   hashtable, alist, or plist.  */

static json_t *
lisp_to_json_nonscalar (Lisp_Object lisp, const struct json_configuration *conf)
{
  if (++lisp_eval_depth > max_lisp_eval_depth)
    xsignal0 (Qjson_object_too_deep);
  json_t *json = lisp_to_json_nonscalar_1 (lisp, conf);
  --lisp_eval_depth;
  return json;
}

/* Convert LISP to any JSON object.  Signal an error of type
   `wrong-type-argument' if the type of LISP can't be converted to a
   JSON object.  */

static json_t *
lisp_to_json (Lisp_Object lisp, const struct json_configuration *conf)
{
  if (EQ (lisp, conf->null_object))
    return json_check (json_null ());
  else if (EQ (lisp, conf->false_object))
    return json_check (json_false ());
  else if (EQ (lisp, Qt))
    return json_check (json_true ());
  else if (INTEGERP (lisp))
    {
      intmax_t low = TYPE_MINIMUM (json_int_t);
      intmax_t high = TYPE_MAXIMUM (json_int_t);
      intmax_t value = check_integer_range (lisp, low, high);
      return json_check (json_integer (value));
    }
  else if (FLOATP (lisp))
    return json_check (json_real (XFLOAT_DATA (lisp)));
  else if (STRINGP (lisp))
    {
      Lisp_Object encoded = json_encode (lisp);
      json_t *json = json_stringn (SSDATA (encoded), SBYTES (encoded));
      if (json == NULL)
	{
	  /* A failure can be caused either by an invalid string or by
	     low memory.  */
	  json_check_utf8 (encoded);
	  json_out_of_memory ();
	}
      return json;
    }

  /* LISP now must be a vector, hashtable, alist, or plist.  */
  return lisp_to_json_nonscalar (lisp, conf);
}

static void
json_parse_args (ptrdiff_t nargs, Lisp_Object *args,
		 struct json_configuration *conf, bool parse_object_types)
{
  if ((nargs % 2) != 0)
    wrong_type_argument (Qplistp, Flist (nargs, args));

  /* Start from the back so keyword values appearing
     first take precedence. */
  for (ptrdiff_t i = nargs; i > 0; i -= 2)
    {
      Lisp_Object key = args[i - 2];
      Lisp_Object value = args[i - 1];
      if (parse_object_types && EQ (key, QCobject_type))
	{
	  if (EQ (value, Qhash_table))
	    conf->object_type = json_object_hashtable;
	  else if (EQ (value, Qalist))
	    conf->object_type = json_object_alist;
	  else if (EQ (value, Qplist))
	    conf->object_type = json_object_plist;
	  else
	    wrong_choice (list3 (Qhash_table, Qalist, Qplist), value);
	}
      else if (parse_object_types && EQ (key, QCarray_type))
	{
	  if (EQ (value, Qarray))
	    conf->array_type = json_array_array;
	  else if (EQ (value, Qlist))
	    conf->array_type = json_array_list;
	  else
	    wrong_choice (list2 (Qarray, Qlist), value);
	}
      else if (EQ (key, QCnull_object))
	conf->null_object = value;
      else if (EQ (key, QCfalse_object))
	conf->false_object = value;
      else if (parse_object_types)
	wrong_choice (list4 (QCobject_type, QCarray_type, QCnull_object,
			     QCfalse_object),
		      value);
      else
	wrong_choice (list2 (QCnull_object, QCfalse_object), value);
    }
}

static bool
json_available_p (void)
{
#ifdef WINDOWSNT
  if (!json_initialized)
    {
      Lisp_Object status;
      json_initialized = init_json_functions ();
      status = json_initialized ? Qt : Qnil;
      Vlibrary_cache = Fcons (Fcons (Qjson, status), Vlibrary_cache);
    }
  return json_initialized;
#else /* !WINDOWSNT */
  return true;
#endif
}

#ifdef WINDOWSNT
static void
ensure_json_available (void)
{
  if (!json_available_p ())
    Fsignal (Qjson_unavailable,
	     list1 (build_unibyte_string ("jansson library not found")));
}
#endif

DEFUN ("json--available-p", Fjson__available_p, Sjson__available_p, 0, 0, NULL,
       doc
       : /* Return non-nil if libjansson is available (internal use only).  */)
(void) { return json_available_p () ? Qt : Qnil; }

DEFUN ("json-serialize", Fjson_serialize, Sjson_serialize, 1, MANY,
       NULL,
       doc: /* Return the JSON representation of OBJECT as a string.

OBJECT must be t, a number, string, vector, hashtable, alist, plist,
or the Lisp equivalents to the JSON null and false values, and its
elements must recursively consist of the same kinds of values.  t will
be converted to the JSON true value.  Vectors will be converted to
JSON arrays, whereas hashtables, alists and plists are converted to
JSON objects.  Hashtable keys must be strings without embedded null
characters and must be unique within each object.  Alist and plist
keys must be symbols; if a key is duplicate, the first instance is
used.

The Lisp equivalents to the JSON null and false values are
configurable in the arguments ARGS, a list of keyword/argument pairs:

The keyword argument `:null-object' specifies which object to use
to represent a JSON null value.  It defaults to `:null'.

The keyword argument `:false-object' specifies which object to use to
represent a JSON false value.  It defaults to `:false'.

In you specify the same value for `:null-object' and `:false-object',
a potentially ambiguous situation, the JSON output will not contain
any JSON false values.
usage: (json-serialize OBJECT &rest ARGS)  */)
(ptrdiff_t nargs, Lisp_Object *args)
{
  specpdl_ref count = SPECPDL_INDEX ();

#ifdef WINDOWSNT
  ensure_json_available ();
#endif

  struct json_configuration conf
    = {json_object_hashtable, json_array_array, QCnull, QCfalse};
  json_parse_args (nargs - 1, args + 1, &conf, false);

  json_t *json = lisp_to_json (args[0], &conf);
  record_unwind_protect_ptr (json_release_object, json);

  char *string = json_dumps (json, JSON_COMPACT | JSON_ENCODE_ANY);
  if (string == NULL)
    json_out_of_memory ();
  record_unwind_protect_ptr (json_free, string);

  return unbind_to (count, build_string_from_utf8 (string));
}

struct json_buffer_and_size
{
  const char *buffer;
  ptrdiff_t size;
  /* This tracks how many bytes were inserted by the callback since
     json_dump_callback was called.  */
  ptrdiff_t inserted_bytes;
};

static Lisp_Object
json_insert (void *data)
{
  struct json_buffer_and_size *buffer_and_size = data;
  ptrdiff_t len = buffer_and_size->size;
  ptrdiff_t inserted_bytes = buffer_and_size->inserted_bytes;
  ptrdiff_t gap_size = GAP_SIZE - inserted_bytes;

  /* Enlarge the gap if necessary.  */
  if (gap_size < len)
    make_gap (len - gap_size);

  /* Copy this chunk of data into the gap.  */
  memcpy ((char *) BEG_ADDR + PT_BYTE - BEG_BYTE + inserted_bytes,
	  buffer_and_size->buffer, len);
  buffer_and_size->inserted_bytes += len;
  return Qnil;
}

static Lisp_Object
json_handle_nonlocal_exit (enum nonlocal_exit type, Lisp_Object data)
{
  switch (type)
    {
    case NONLOCAL_EXIT_SIGNAL:
      return data;
    case NONLOCAL_EXIT_THROW:
      return Fcons (Qno_catch, data);
    default:
      eassume (false);
    }
}

struct json_insert_data
{
  /* This tracks how many bytes were inserted by the callback since
     json_dump_callback was called.  */
  ptrdiff_t inserted_bytes;
  /* nil if json_insert succeeded, otherwise the symbol
     Qcatch_all_memory_full or a cons (ERROR-SYMBOL . ERROR-DATA).  */
  Lisp_Object error;
};

/* Callback for json_dump_callback that inserts a JSON representation
   as a unibyte string into the gap.  DATA must point to a structure
   of type json_insert_data.  This function may not exit nonlocally.
   It catches all nonlocal exits and stores them in data->error for
   reraising.  */

static int
json_insert_callback (const char *buffer, size_t size, void *data)
{
  struct json_insert_data *d = data;
  struct json_buffer_and_size buffer_and_size
    = {.buffer = buffer, .size = size, .inserted_bytes = d->inserted_bytes};
  d->error = internal_catch_all (json_insert, &buffer_and_size,
				 json_handle_nonlocal_exit);
  d->inserted_bytes = buffer_and_size.inserted_bytes;
  return NILP (d->error) ? 0 : -1;
}

DEFUN ("json-insert", Fjson_insert, Sjson_insert, 1, MANY,
       NULL,
       doc: /* Insert the JSON representation of OBJECT before point.
This is the same as (insert (json-serialize OBJECT)), but potentially
faster.  See the function `json-serialize' for allowed values of
OBJECT.
usage: (json-insert OBJECT &rest ARGS)  */)
(ptrdiff_t nargs, Lisp_Object *args)
{
  specpdl_ref count = SPECPDL_INDEX ();

#ifdef WINDOWSNT
  ensure_json_available ();
#endif

  struct json_configuration conf
    = {json_object_hashtable, json_array_array, QCnull, QCfalse};
  json_parse_args (nargs - 1, args + 1, &conf, false);

  json_t *json = lisp_to_json (args[0], &conf);
  record_unwind_protect_ptr (json_release_object, json);

  prepare_to_modify_buffer (PT, PT, NULL);
  move_gap_both (PT, PT_BYTE);
  struct json_insert_data data;
  data.inserted_bytes = 0;
  /* Could have used json_dumpb, but that became available only in
     Jansson 2.10, whereas we want to support 2.7 and upward.  */
  int status = json_dump_callback (json, json_insert_callback, &data,
				   JSON_COMPACT | JSON_ENCODE_ANY);
  if (status == -1)
    {
      if (CONSP (data.error))
	xsignal (XCAR (data.error), XCDR (data.error));
      else
	json_out_of_memory ();
    }

  ptrdiff_t inserted = 0;
  ptrdiff_t inserted_bytes = data.inserted_bytes;
  if (inserted_bytes > 0)
    {
      /* If required, decode the stuff we've read into the gap.  */
      struct coding_system coding;
      /* JSON strings are UTF-8 encoded strings.  If for some reason
	 the text returned by the Jansson library includes invalid
	 byte sequences, they will be represented by raw bytes in the
	 buffer text.  */
      setup_coding_system (Qutf_8_unix, &coding);
      coding.dst_multibyte
	= !NILP (BVAR (current_buffer, enable_multibyte_characters));
      if (CODING_MAY_REQUIRE_DECODING (&coding))
	{
	  /* Now we have all the new bytes at the beginning of the gap,
	     but `decode_coding_gap` needs them at the end of the gap, so
	     we need to move them.  */
	  memmove (GAP_END_ADDR - inserted_bytes, GPT_ADDR, inserted_bytes);
	  decode_coding_gap (&coding, inserted_bytes);
	  inserted = coding.produced_char;
	}
      else
	{
	  /* Make the inserted text part of the buffer, as unibyte text.  */
	  eassert (NILP (BVAR (current_buffer, enable_multibyte_characters)));
	  insert_from_gap_1 (inserted_bytes, inserted_bytes, false);

	  /* The target buffer is unibyte, so we don't need to decode.  */
	  invalidate_buffer_caches (current_buffer, PT, PT + inserted_bytes);
	  adjust_after_insert (PT, PT_BYTE, PT + inserted_bytes,
			       PT_BYTE + inserted_bytes, inserted_bytes);
	  inserted = inserted_bytes;
	}
    }

  /* Call after-change hooks.  */
  signal_after_change (PT, 0, inserted);
  if (inserted > 0)
    {
      update_compositions (PT, PT, CHECK_BORDER);
      /* Move point to after the inserted text.  */
      SET_PT_BOTH (PT + inserted, PT_BYTE + inserted_bytes);
    }

  return unbind_to (count, Qnil);
}

/* Convert a JSON object to a Lisp object.  */

static Lisp_Object ARG_NONNULL ((1))
  json_to_lisp (json_t *json, const struct json_configuration *conf)
{
  switch (json_typeof (json))
    {
    case JSON_NULL:
      return conf->null_object;
    case JSON_FALSE:
      return conf->false_object;
    case JSON_TRUE:
      return Qt;
      case JSON_INTEGER: {
	json_int_t i = json_integer_value (json);
	return INT_TO_INTEGER (i);
      }
    case JSON_REAL:
      return make_float (json_real_value (json));
    case JSON_STRING:
      return make_string_from_utf8 (json_string_value (json),
				    json_string_length (json));
      case JSON_ARRAY: {
	if (++lisp_eval_depth > max_lisp_eval_depth)
	  xsignal0 (Qjson_object_too_deep);
	size_t size = json_array_size (json);
	if (PTRDIFF_MAX < size)
	  overflow_error ();
	Lisp_Object result;
	switch (conf->array_type)
	  {
	    case json_array_array: {
	      result = make_vector (size, Qunbound);
	      for (ptrdiff_t i = 0; i < size; ++i)
		{
		  rarely_quit (i);
		  ASET (result, i,
			json_to_lisp (json_array_get (json, i), conf));
		}
	      break;
	    }
	    case json_array_list: {
	      result = Qnil;
	      for (ptrdiff_t i = size - 1; i >= 0; --i)
		{
		  rarely_quit (i);
		  result = Fcons (json_to_lisp (json_array_get (json, i), conf),
				  result);
		}
	      break;
	    }
	  default:
	    /* Can't get here.  */
	    emacs_abort ();
	  }
	--lisp_eval_depth;
	return result;
      }
      case JSON_OBJECT: {
	if (++lisp_eval_depth > max_lisp_eval_depth)
	  xsignal0 (Qjson_object_too_deep);
	Lisp_Object result;
	switch (conf->object_type)
	  {
	    case json_object_hashtable: {
	      size_t size = json_object_size (json);
	      if (FIXNUM_OVERFLOW_P (size))
		overflow_error ();
	      result = CALLN (Fmake_hash_table, QCtest, Qequal, QCsize,
			      make_fixed_natnum (size));
	      struct Lisp_Hash_Table *h = XHASH_TABLE (result);
	      const char *key_str;
	      json_t *value;
	      json_object_foreach (json, key_str, value)
	      {
		Lisp_Object key = build_string_from_utf8 (key_str), hash;
		ptrdiff_t i = hash_lookup (h, key, &hash);
		/* Keys in JSON objects are unique, so the key can't
		   be present yet.  */
		eassert (i < 0);
		hash_put (h, key, json_to_lisp (value, conf), hash);
	      }
	      break;
	    }
	    case json_object_alist: {
	      result = Qnil;
	      const char *key_str;
	      json_t *value;
	      json_object_foreach (json, key_str, value)
	      {
		Lisp_Object key
		  = Fintern (build_string_from_utf8 (key_str), Qnil);
		result
		  = Fcons (Fcons (key, json_to_lisp (value, conf)), result);
	      }
	      result = Fnreverse (result);
	      break;
	    }
	    case json_object_plist: {
	      result = Qnil;
	      const char *key_str;
	      json_t *value;
	      json_object_foreach (json, key_str, value)
	      {
		USE_SAFE_ALLOCA;
		ptrdiff_t key_str_len = strlen (key_str);
		char *keyword_key_str = SAFE_ALLOCA (1 + key_str_len + 1);
		keyword_key_str[0] = ':';
		strcpy (&keyword_key_str[1], key_str);
		Lisp_Object key = intern_1 (keyword_key_str, key_str_len + 1);
		/* Build the plist as value-key since we're going to
		   reverse it in the end.*/
		result = Fcons (key, result);
		result = Fcons (json_to_lisp (value, conf), result);
		SAFE_FREE ();
	      }
	      result = Fnreverse (result);
	      break;
	    }
	  default:
	    /* Can't get here.  */
	    emacs_abort ();
	  }
	--lisp_eval_depth;
	return result;
      }
    }
  /* Can't get here.  */
  emacs_abort ();
}

DEFUN ("json-parse-string", Fjson_parse_string, Sjson_parse_string, 1, MANY,
       NULL,
       doc: /* Parse the JSON STRING into a Lisp object.
This is essentially the reverse operation of `json-serialize', which
see.  The returned object will be the JSON null value, the JSON false
value, t, a number, a string, a vector, a list, a hashtable, an alist,
or a plist.  Its elements will be further objects of these types.  If
there are duplicate keys in an object, all but the last one are
ignored.  If STRING doesn't contain a valid JSON object, this function
signals an error of type `json-parse-error'.

The arguments ARGS are a list of keyword/argument pairs:

The keyword argument `:object-type' specifies which Lisp type is used
to represent objects; it can be `hash-table', `alist' or `plist'.  It
defaults to `hash-table'.

The keyword argument `:array-type' specifies which Lisp type is used
to represent arrays; it can be `array' (the default) or `list'.

The keyword argument `:null-object' specifies which object to use
to represent a JSON null value.  It defaults to `:null'.

The keyword argument `:false-object' specifies which object to use to
represent a JSON false value.  It defaults to `:false'.
usage: (json-parse-string STRING &rest ARGS) */)
(ptrdiff_t nargs, Lisp_Object *args)
{
  specpdl_ref count = SPECPDL_INDEX ();

#ifdef WINDOWSNT
  ensure_json_available ();
#endif

  Lisp_Object string = args[0];
  CHECK_STRING (string);
  Lisp_Object encoded = json_encode (string);
  check_string_without_embedded_nulls (encoded);
  struct json_configuration conf
    = {json_object_hashtable, json_array_array, QCnull, QCfalse};
  json_parse_args (nargs - 1, args + 1, &conf, true);

  json_error_t error;
  json_t *object
    = json_loads (SSDATA (encoded), JSON_DECODE_ANY | JSON_ALLOW_NUL, &error);
  if (object == NULL)
    json_parse_error (&error);

  /* Avoid leaking the object in case of further errors.  */
  if (object != NULL)
    record_unwind_protect_ptr (json_release_object, object);

  return unbind_to (count, json_to_lisp (object, &conf));
}

// JSONRPC

#define ERROR_BUFFER_SIZE 1024 * 1024 * 4

struct json_rpc_state
{
  pthread_mutex_t handle_mx;
  struct SSP_Handle *handle;
  json_t *message;
  json_error_t error;
  bool done;
  pid_t pid;
  // ssp_send could in principle block indefinitely while holding the
  // handle mutex, so we'll have it use a send timeout to
  // periodically check for a cancel flag that can be set on shutdown
  // (done would probably do the job, so the separate flag
  // cancel_send may later be done away with).  Since this is a
  // monotonic 1-bit value, I assume no locking is needed (?)
  bool cancel_send;
  char error_buffer[ERROR_BUFFER_SIZE + 1];
  int error_buffer_read;
};

/* Usage:
 * if (can_use_handle (state))
 *   {
 *     ... use state->handle
 *     end_using_handle (state);
 *   }
 */
inline static void
end_using_handle (struct json_rpc_state *state)
{
  assert (state->handle);
  pthread_mutex_unlock (&state->handle_mx);
}
inline static int
can_use_handle (struct json_rpc_state *state, struct timespec *timeout)
{
  if (pthread_mutex_timedlock (&state->handle_mx, timeout) == 0)
    {
      if (state->handle)
	return 1; /* handle is good */
      /* else handle is already gone */
      end_using_handle (state);
    }
  return 0;
}

inline static void
CHECK_RPC_CONNECTION (Lisp_Object obj)
{
  CHECK_TYPE (USER_PTRP (obj), Quser_ptrp, obj);
}

static void
json_rpc_state_free (void *ptr)
{
  struct json_rpc_state *state = ptr;
  assert (state->handle == NULL); /* Loop must be exited */
  pthread_mutex_destroy (&state->handle_mx);
  free (state);
}

DEFUN ("json-rpc-connection", Fjson_rpc_connection, Sjson_rpc_connection, 1,
       MANY, NULL, doc
       : /* Create JSONRPC connection. */)
(ptrdiff_t nargs, Lisp_Object *args)
{
  USE_SAFE_ALLOCA;
  char **new_argv;
  SAFE_NALLOCA (new_argv, 1, nargs + 1);
  new_argv[nargs] = NULL;

  for (int i = 0; i < nargs; i++)
    {
      CHECK_STRING (args[i]);
      new_argv[i] = SSDATA (args[i]);
    }

  struct SSP_Opts opts;
  memset (&opts, 0, sizeof (opts));
  opts.binary = new_argv[0];
  opts.argv = new_argv;
  opts.read_timeout_ms = -1;
  const Lisp_Object current_dir = get_current_directory (true);
  opts.envp = make_environment_block (current_dir);
  struct SSP_Handle *handle = ssp_spawn (&opts);
  if (!handle)
    {
      Fsignal (Qerror, list1 (build_string ("Failed to start process.")));
    }
  else
    {
      struct json_rpc_state *state = malloc (sizeof (struct json_rpc_state));
      /* TODO: state might be NULL */
      pthread_mutex_init (&state->handle_mx, NULL);
      /* TODO: mutex_init could fail */
      state->handle = handle;
      state->cancel_send = false;
      state->done = false;
      state->error_buffer_read = 0;
      state->pid = handle->pid;
      SAFE_FREE ();
      return make_user_ptr (json_rpc_state_free, state);
    }
}

struct json_rpc_send_params
{
  struct json_rpc_state *state;
  json_t *message;
  // out parameter, will hold the result of send()
  int res;
};

static void
json_rpc_send_callback (void *arg)
{
  struct json_rpc_send_params *param = arg;
  struct json_rpc_state *state = param->state;
  json_t *message = param->message;
  struct thread_state *self = current_thread;
  // TODO: according to @yyoncho, release_global_lock (and, I guess,
  // sys_thread_yield?) should precede can_use_handle. Haven't looked
  // into this yet, so for now I'll just leave the order as it was
  struct timespec timeout = {.tv_sec = 0, .tv_nsec = 5000000};
  param->res = -1;
  release_global_lock ();
  sys_thread_yield ();
  if (can_use_handle (state, &timeout))
    {
      char *string = json_dumps (message, JSON_COMPACT | JSON_ENCODE_ANY);
      /* TODO: no point in copying whole message */
      size_t size = strlen (string);
      char *msg = malloc (size + 100);
      /* TODO: missing test if msg != NULL */
      sprintf (msg, "Content-Length: %zu\r\n\r\n%s", size, string);
      /* TODO: send could do a partial send */
      const int bytes_to_send = strlen (msg);
      const int bytes_sent
	= state->handle->send (state->handle, &state->cancel_send, msg,
			       bytes_to_send);
      assert (bytes_sent <= bytes_to_send);
      if (bytes_sent == bytes_to_send)
	param->res = 0;
      end_using_handle (state);
      free (msg);
      free (string);
    }
  acquire_global_lock (self);
}

static struct json_rpc_state *
json_rpc_state (Lisp_Object connection)
{
  return XUSER_PTR (connection)->p;
}

DEFUN ("json-rpc-send", Fjson_rpc_send, Sjson_rpc_send, 1, MANY, NULL, doc
       : /* Send message to jsonrpc connection */)
(ptrdiff_t nargs, Lisp_Object *args)
{
  Lisp_Object connection = args[0];
  CHECK_RPC_CONNECTION (connection);

  struct json_configuration conf
    = {json_object_hashtable, json_array_array, QCnull, QCfalse};
  json_parse_args (nargs - 2, args + 2, &conf, false);

  json_t *message = lisp_to_json (args[1], &conf);

  /* TODO: params is on the stack; is this an issue? */
  struct json_rpc_send_params params
    = {.state = json_rpc_state (connection), .message = message, .res = -1};
  flush_stack_call_func (json_rpc_send_callback, &params);
  if (params.res < 0)
    {
      Fsignal (Qerror, list1 (build_string ("Failed to send message")));
    }
  return Qnil;
}

DEFUN ("json-rpc-shutdown", Fjson_rpc_shutdown, Sjson_rpc_shutdown, 1, 1, 0, doc
       : /* Shutdowns json rpc connection */)
(Lisp_Object connection)
{
  CHECK_RPC_CONNECTION (connection);
  struct json_rpc_state *state = json_rpc_state (connection);
  // even if send() currently holds the handle mutex, and is executing
  // a blocking send, setting this to true should cause it to finish
  // after the send socket's timeout
  state->cancel_send = true;
  /* should only block up to sndtimeo in practice, but we'll still specify a
   * timeout just in case */
  struct timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
  if (can_use_handle (state, &timeout))
    {
      state->handle->cancel_recv (state->handle);
      end_using_handle (state);
    }
  return Qnil;
}

DEFUN ("json-rpc-pid", Fjson_rpc_pid, Sjson_rpc_pid, 1, 1, 0, doc
       : /* Shutdowns json rpc connection */)
(Lisp_Object connection)
{
  CHECK_RPC_CONNECTION (connection);
  struct json_rpc_state *state = json_rpc_state (connection);
  return make_int (state->done ? -1 : state->pid);
}

DEFUN ("json-rpc-stderr", Fjson_rpc_stderr, Sjson_rpc_stderr, 1, 1, 0, doc
       : /* Shutdowns json rpc connection */)
(Lisp_Object connection)
{
  CHECK_RPC_CONNECTION (connection);
  struct json_rpc_state *state = json_rpc_state (connection);
  return make_string (state->error_buffer, state->error_buffer_read);
}

static size_t
read_stdout (struct json_rpc_state *param, char *buffer, size_t size)
{
  struct SSP_Handle *handle = param->handle;
  size_t result, read_res;
  do
    {
      result = size;
      size_t stderr_size = ERROR_BUFFER_SIZE - param->error_buffer_read;
      read_res = handle->recv (handle, buffer, &result,
			       param->error_buffer + param->error_buffer_read,
			       &stderr_size);

      if (stderr_size)
	{
	  param->error_buffer_read += stderr_size;
	  if (param->error_buffer_read == ERROR_BUFFER_SIZE)
	    {
	      param->error_buffer_read = ERROR_BUFFER_SIZE / 2;
	      strcpy (param->error_buffer,
		      param->error_buffer + ERROR_BUFFER_SIZE / 2);
	    }
	}

      if (result)
	break;
  } while (read_res > 0 || handle->isalive (handle));

  return result;
}

static bool
read_until (struct json_rpc_state *param, const char *needle, char *output)
{
  // XXX: optimize the first read and make sure output is not
  // overflowing
  size_t bytes_read = 0;
  int read = 0;
  while (strstr (output, needle) == NULL)
    {
      bytes_read = read_stdout (param, output + read, 1);
      if (bytes_read == 0)
	return false;
      read += bytes_read;
    }
  return true;
}

static void
json_rpc_callback (void *arg)
{
  struct json_rpc_state *param = arg;
  struct thread_state *self = current_thread;

  release_global_lock ();
  sys_thread_yield ();

  char data[BUFFER_SIZE + 1];

  memset (data, '\0', BUFFER_SIZE);
  if (read_until (param, "Content-Length:", data))
    {
      memset (data, '\0', BUFFER_SIZE);

      if (read_until (param, "\r\n\r\n", data))
	{
	  char *_end;
	  const size_t content_length = strtol (data, &_end, 10);
	  size_t has_to_read = content_length;
	  char *msg = malloc (content_length + 1);
	  while (has_to_read > 0)
	    {
	      int bytes_read
		= read_stdout (param, msg + (content_length - has_to_read),
			       has_to_read);
	      if (bytes_read == 0)
		{
		  param->done = true;
		  break;
		}

	      has_to_read -= bytes_read;
	    }
	  if (!param->done)
	    {
	      msg[content_length] = '\0';
	      param->message
		= json_loads (msg, JSON_DECODE_ANY | JSON_ALLOW_NUL,
			      &param->error);
	      free (msg);
	    }
	}
      else
	{
	  param->done = true;
	}
    }
  else
    {
      param->done = true;
    }

  acquire_global_lock (self);
}

static Lisp_Object
get_json_parse_error (const json_error_t *error)
{
  Lisp_Object symbol;
#if JSON_HAS_ERROR_CODE
  switch (json_error_code (error))
    {
    case json_error_premature_end_of_input:
      symbol = Qjson_end_of_file;
      break;
    case json_error_end_of_input_expected:
      symbol = Qjson_trailing_content;
      break;
    default:
      symbol = Qjson_parse_error;
      break;
    }
#else
  if (json_has_suffix (error->text, "expected near end of file"))
    symbol = Qjson_end_of_file;
  else if (json_has_prefix (error->text, "end of file expected"))
    symbol = Qjson_trailing_content;
  else
    symbol = Qjson_parse_error;
#endif
  return Fcons (symbol, list5 (build_string_from_utf8 (error->text),
			       build_string_from_utf8 (error->source),
			       INT_TO_INTEGER (error->line),
			       INT_TO_INTEGER (error->column),
			       INT_TO_INTEGER (error->position)));
}

DEFUN ("json-rpc", Fjson_rpc, Sjson_rpc, 1, MANY, NULL, doc
       : /* Runs json-rpc dispach loop over jsonrpc connection */)
(ptrdiff_t nargs, Lisp_Object *args)
{
  Lisp_Object connection = args[0];
  CHECK_RPC_CONNECTION (connection);

  Lisp_Object callback = args[1];

  struct json_configuration conf
    = {json_object_hashtable, json_array_array, QCnull, QCfalse};

  json_parse_args (nargs - 2, args + 2, &conf, true);

  struct json_rpc_state *param = json_rpc_state (connection);
  struct SSP_Handle *handle = param->handle;

  while (!param->done && handle->isalive (handle))
    {
      flush_stack_call_func (json_rpc_callback, param);

      if (!param->done)
	{
	  if (param->message != NULL)
	    {
	      Lisp_Object msg = json_to_lisp (param->message, &conf);
	      free (param->message);
	      param->message = NULL;
	      CALLN (Ffuncall, callback, msg, Qnil, Qnil);
	    }
	  else
	    {
	      Lisp_Object error = get_json_parse_error (&param->error);
	      CALLN (Ffuncall, callback, Qnil, error, Qnil);
	    }
	}
      else
	{
	  // ??
	}
    }
  CALLN (Ffuncall, callback, Qnil, Qnil, Qt);
  // timeout probably not necessary here (?), but let's rather be safe than
  // sorry
  struct timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
  if (pthread_mutex_timedlock (&param->handle_mx, &timeout) == 0)
    {
      param->handle->close (param->handle);
      param->handle = NULL;
      pthread_mutex_unlock (&param->handle_mx);
    }
  /* TODO: what if mutex_lock fails? */
  return Qnil;
}

// jsonrpc end

struct json_read_buffer_data
{
  /* Byte position of position to read the next chunk from.  */
  ptrdiff_t point;
};

/* Callback for json_load_callback that reads from the current buffer.
   DATA must point to a structure of type json_read_buffer_data.
   data->point must point to the byte position to read from; after
   reading, data->point is advanced accordingly.  The buffer point
   itself is ignored.  This function may not exit nonlocally.  */

static size_t
json_read_buffer_callback (void *buffer, size_t buflen, void *data)
{
  struct json_read_buffer_data *d = data;

  /* First, parse from point to the gap or the end of the accessible
     portion, whatever is closer.  */
  ptrdiff_t point = d->point;
  ptrdiff_t end = BUFFER_CEILING_OF (point) + 1;
  ptrdiff_t count = end - point;
  if (buflen < count)
    count = buflen;
  memcpy (buffer, BYTE_POS_ADDR (point), count);
  d->point += count;
  return count;
}

DEFUN ("json-parse-buffer", Fjson_parse_buffer, Sjson_parse_buffer,
       0, MANY, NULL,
       doc: /* Read JSON object from current buffer starting at point.
Move point after the end of the object if parsing was successful.
On error, don't move point.

The returned object will be a vector, list, hashtable, alist, or
plist.  Its elements will be the JSON null value, the JSON false
value, t, numbers, strings, or further vectors, lists, hashtables,
alists, or plists.  If there are duplicate keys in an object, all
but the last one are ignored.

If the current buffer doesn't contain a valid JSON object, the
function signals an error of type `json-parse-error'.

The arguments ARGS are a list of keyword/argument pairs:

The keyword argument `:object-type' specifies which Lisp type is used
to represent objects; it can be `hash-table', `alist' or `plist'.  It
defaults to `hash-table'.

The keyword argument `:array-type' specifies which Lisp type is used
to represent arrays; it can be `array' (the default) or `list'.

The keyword argument `:null-object' specifies which object to use
to represent a JSON null value.  It defaults to `:null'.

The keyword argument `:false-object' specifies which object to use to
represent a JSON false value.  It defaults to `:false'.
usage: (json-parse-buffer &rest args) */)
(ptrdiff_t nargs, Lisp_Object *args)
{
  specpdl_ref count = SPECPDL_INDEX ();

#ifdef WINDOWSNT
  ensure_json_available ();
#endif

  struct json_configuration conf
    = {json_object_hashtable, json_array_array, QCnull, QCfalse};
  json_parse_args (nargs, args, &conf, true);

  ptrdiff_t point = PT_BYTE;
  struct json_read_buffer_data data = {.point = point};
  json_error_t error;
  json_t *object = json_load_callback (json_read_buffer_callback, &data,
				       JSON_DECODE_ANY | JSON_DISABLE_EOF_CHECK
					 | JSON_ALLOW_NUL,
				       &error);

  if (object == NULL)
    json_parse_error (&error);

  /* Avoid leaking the object in case of further errors.  */
  record_unwind_protect_ptr (json_release_object, object);

  /* Convert and then move point only if everything succeeded.  */
  Lisp_Object lisp = json_to_lisp (object, &conf);

  /* Adjust point by how much we just read.  */
  point += error.position;
  SET_PT_BOTH (BYTE_TO_CHAR (point), point);

  return unbind_to (count, lisp);
}

void
syms_of_json (void)
{
  DEFSYM (QCnull, ":null");
  DEFSYM (QCfalse, ":false");

  DEFSYM (Qstring_without_embedded_nulls_p, "string-without-embedded-nulls-p");
  DEFSYM (Qjson_value_p, "json-value-p");

  DEFSYM (Qjson_error, "json-error");
  DEFSYM (Qjson_out_of_memory, "json-out-of-memory");
  DEFSYM (Qjson_parse_error, "json-parse-error");
  DEFSYM (Qjson_end_of_file, "json-end-of-file");
  DEFSYM (Qjson_trailing_content, "json-trailing-content");
  DEFSYM (Qjson_object_too_deep, "json-object-too-deep");
  DEFSYM (Qjson_unavailable, "json-unavailable");
  define_error (Qjson_error, "generic JSON error", Qerror);
  define_error (Qjson_out_of_memory,
		"not enough memory for creating JSON object", Qjson_error);
  define_error (Qjson_parse_error, "could not parse JSON stream", Qjson_error);
  define_error (Qjson_end_of_file, "end of JSON stream", Qjson_parse_error);
  define_error (Qjson_trailing_content, "trailing content after JSON stream",
		Qjson_parse_error);
  define_error (Qjson_object_too_deep,
		"object cyclic or Lisp evaluation too deep", Qjson_error);

  DEFSYM (Qpure, "pure");
  DEFSYM (Qside_effect_free, "side-effect-free");

  DEFSYM (Qjson_serialize, "json-serialize");
  DEFSYM (Qjson_parse_string, "json-parse-string");
  DEFSYM (Qjson_rpc, "json-rpc");
  DEFSYM (Qjson_rpc_connection, "json-rpc-connection");
  DEFSYM (Qjson_rpc_shutdown, "json-rpc-shutdown");
  DEFSYM (Qjson_rpc_send, "json-rpc-send");
  DEFSYM (Qjson_rpc_pid, "json-rpc-pid");
  DEFSYM (Qjson_rpc_close, "json-rpc-close");
  DEFSYM (Qjson_rpc_stderr, "json-rpc-stderr");
  Fput (Qjson_serialize, Qpure, Qt);
  Fput (Qjson_serialize, Qside_effect_free, Qt);
  Fput (Qjson_parse_string, Qpure, Qt);
  Fput (Qjson_parse_string, Qside_effect_free, Qt);

  DEFSYM (QCobject_type, ":object-type");
  DEFSYM (QCarray_type, ":array-type");
  DEFSYM (QCnull_object, ":null-object");
  DEFSYM (QCfalse_object, ":false-object");
  DEFSYM (Qalist, "alist");
  DEFSYM (Qplist, "plist");
  DEFSYM (Qarray, "array");

  defsubr (&Sjson__available_p);
  defsubr (&Sjson_serialize);
  defsubr (&Sjson_insert);
  defsubr (&Sjson_parse_string);
  defsubr (&Sjson_rpc);
  defsubr (&Sjson_rpc_connection);
  defsubr (&Sjson_rpc_send);
  defsubr (&Sjson_rpc_shutdown);
  defsubr (&Sjson_rpc_pid);
  defsubr (&Sjson_rpc_stderr);
  defsubr (&Sjson_parse_buffer);
}
