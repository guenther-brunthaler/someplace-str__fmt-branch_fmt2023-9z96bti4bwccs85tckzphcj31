/* fmt2023 - yet another sprintf() replacement idea.
 *
 * I have had so many of those ideas, one worse than the other, that I started
 * naming them by the year when the next brainstorm hit me.
 *
 * Version 2023.3.1
 * Copyright (c) 2023 Guenther Brunthaler. All rights reserved.
 *
 * This source file is free software.
 * Distribution is permitted under the terms of the LGPLv3. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

struct insertion_sequence {
   size_t name_bytecount, expansion_bytecount;
   struct insertion_sequence *older;
   char const *name, *expansion;
};

struct fmt2023_ctx {
   int no_write, next_size_known, cmd_introducer;
   size_t next_size;
   char const *stop;
   char *outpos;
   struct insertion_sequence *insertions;
   va_list *args;
};

static void fmt2023_expand(struct fmt2023_ctx *o, char const *format) {
   int trigger, no_write= o->no_write;
   struct insertion_sequence *insertions= o->insertions;
   /* Consider the terminating null character to be part of the format string.
    * This automatically terminates the current output string once all of the
    * format string's bytes have been processed. */
   size_t remaining= strlen(format) + 1;
   if (!(trigger= o->cmd_introducer) && insertions) {
      trigger= o->cmd_introducer= *insertions->name;
   }
   while (remaining) {
      /* Format string not yet exhausted. */
      char const *source;
      size_t bytes;
      if (trigger) {
         if (*format == trigger) {
            /* Possibly at the introducing name character of an insertion
             * sequence. */
            struct insertion_sequence *i;
            for (i= insertions; i; i= i->older) {
               /* Try to locate insertion sequence with matching name. */
               if (
                  remaining >= i->name_bytecount
                  && !memcmp(format, i->name, i->name_bytecount)
               ) {
                  /* Found. */
                  source= i->expansion; bytes= i->expansion_bytecount;
                  format+= i->name_bytecount; remaining-= i->name_bytecount;
                  goto append_bytes;
               }
            }
            /* No matching name. Consider current character as a literal one
             * rather than being part of an insertion sequence. */
            if (
               remaining > 1
               && (source= memchr(format + 1, trigger, remaining - 1))
            ) {
               goto append_literally_until_source;
            }
         } else if (source= memchr(format, trigger, remaining)) {
            /* A possible insertion sequence starts later into the format
             * string. Append the text before there as literal text. */
            assert(source > format);
            /* Append prefix of unprocessed part of the format string up to
             * the next possible insertion sequence at <source>. */
            append_literally_until_source:
            bytes= source - format; source= format;
            format+= bytes; remaining-= bytes;
            goto append_bytes;
         }
         /* No further possible insertion sequence found. */
      }
      /* No further insertion sequence can possibly exist in the format
       * string.
       *
       * Append the rest of the format string literally. */
      source= format; bytes= remaining; remaining= 0;
      /* Append bytes to the result string (only virtually in case of
       * 'no_write' mode). */
      append_bytes:
      if (o->outpos + bytes > o->stop) o->no_write= no_write= 1;
      if (!no_write) (void)memcpy(o->outpos, source, bytes);
      o->outpos+= bytes;
   }
}

static int fmt2023_worker(
   struct fmt2023_ctx *o, struct insertion_sequence *nsq
) {
   for (;;) {
      char const *cmd;
      if (cmd= va_arg(*o->args, char const *)) {
         switch (*cmd) {
            case '\v':
               o->next_size= va_arg(*o->args, size_t); o->next_size_known= 1;
               break;
            case '\r':
               {
                  void **saveloc= va_arg(*o->args, void **);
                  *saveloc= o->outpos;
               }
               break;
            case '\n':
               nsq->name= va_arg(*o->args, char const *);
               nsq->expansion= o->outpos;
               nsq->expansion_bytecount= 0;
               goto register_insertion;
            case '\f':
               cmd= va_arg(*o->args, char const *);
               fmt2023_expand(o, cmd);
               o->insertions->expansion_bytecount=
                  o->outpos - 1 - o->insertions->expansion
               ;
               break;
            default:
               nsq->name= cmd;
               nsq->expansion= va_arg(*o->args, void *);
               nsq->expansion_bytecount=
                  o->next_size_known
                  ? (o->next_size_known= 0 , o->next_size)
                  : strlen(nsq->expansion)
               ;
               register_insertion:
               nsq->name_bytecount= strlen(nsq->name);
               nsq->older= o->insertions;
               o->insertions= nsq;
               return 1;
         }
      } else {
         cmd= va_arg(*o->args, char const *);
         fmt2023_expand(o, cmd);
         return 0;
      }
   }
}

/* Recursion helper, using minimal stack frame. */
static void fmt2023_recurser(struct fmt2023_ctx *o) {
   struct insertion_sequence i;
   if (fmt2023_worker(o, &i)) fmt2023_recurser(o);
}

/* Expands one or more format strings into a given memory buffer. All
 * formatted strings will be null-terminated. There is no additional padding
 * between the formatted output strings.
 *
 * The function never fails. If the buffer is too small, it automatically
 * switches to "no write" mode and stops writing to the buffer. In this case,
 * there is no guarantee how much if anything has been written. This mode
 * can also be enforced by passing a null pointer as <buffer>.
 *
 * In any case, the function returns the number of bytes which *would* have
 * been written if the buffer was large enough. This number includes the
 * trailing null byte. Think of "strlen(buffer) + 1" in case of success.
 *
 * If the return value is less than <buffer_size>, then everything has been
 * written to the buffer and the formatting operation has thus been successful.
 *
 * The variable arguments control the formatting operation. All arguments
 * come as pairs:
 *
 * NULL, <final_format>: Stop processing the argument list and use
 * <final_format> as the last format string to be processed.
 *
 * "\v", <bytes>: Set size_t value <bytes> as an override for the next
 * insertion string definition. This means that insertion sequence does not
 * need to be a null terminated string, but can be anyting binary. Think of
 * "void *" as a mnemonic.
 *
 * "\n", <name>, "\f", <format>: Create a new insertion sequence <name> by
 * expanding format string <format>. The result will *then* be appended at the
 * current output position as a null-terminated string. If <name> should
 * already be referenced during the expansion of <format>, it will expand to
 * an empty string. Use of "\f" in any other constellation than explained here
 * will result in undefined behavior.
 *
 * "\r", <addr>: Write the current output address to a pointer variable with
 * address <addr>. This allows to store the starting addresses of the next
 * string to be formatted, avoiding the need to search for null bytes in the
 * buffer as string separators within the formatted result. "\r" is frequently
 * used after a series of "\f" instructions in order to communicate the
 * address of the final output string to the caller. In case of "no write"
 * mode, the written pointer values have no significance and must be ignored.
 * Think of "report" as a mnemonic.
 *
 * <name>, <expansion>: The normal way to specify an insertion sequence.
 * <name> must be a null-terminated string identifying the sequence. Actually
 * it is more than just a name; it is a substring to search within a format
 * string, replacing all ocurrences of the substring with the string at
 * <expansion>. The latter one is normally a null-terminated string, but a
 * preceding "\v" can override this, so anything binary including null bytes
 * can be expanded as well. There is no restriction what <name> can be other
 * that it cannot start with one of the special-function characters. However,
 * for efficiency purposes, all insertion names must start with the same
 * characters. Thus, one could use "%1", "%2", "%(10)" etc. as names, or
 * rather "<name1>", "<name2>", but both styles cannot be mixed within the
 * same function invocation. There is no escape mechanism by default. But it
 * is easy to provide one. One could define an insertion sequence "%%" which
 * expands to a single "%". Then one could write "The expression '%1 %% of %2'
 * means %1 percent of the quantity %2.".
 *
 * Text after the special-function characters like "\v" is allowed but will be
 * ignored. It could be used to pass hints to a human translator, for
 * instance.
 *
 * The format string is expanded by scanning for the first character of an
 * insertion sequence name (which must all be the same). Then all insertion
 * sequence names defined so far are checked whether one matches the prefix
 * of the yet-unprocessed rest of the format string. If so, its associated
 * expansion string contents are appended to the output buffer. Otherwise, the
 * format introduction character loses its special meaning and is copied to
 * the output buffer literally.
 *
 * No sophisticated search algorithm is used to locate the named insertion
 * sequences. A simple linear search backwards is done instead. This means
 * that names defined later will be found faster. And in case of duplicate
 * names, only the last instance of the name defined so far will be found.
 * This can even be exploited for "redefining" names if multiple format
 * strings are used. */
static size_t vfmt2023(char *buffer, size_t buffer_size, va_list args) {
   struct fmt2023_ctx o;
   o.args= &args;
   o.no_write= !(o.outpos= buffer);
   o.next_size_known= 0;
   o.insertions= 0;
   o.cmd_introducer= '\0';
   o.stop= buffer + buffer_size;
   fmt2023_recurser(&o);
   assert(o.outpos >= buffer);
   return (size_t)(o.outpos - buffer);
}

static size_t sfmt2023(char *buffer, size_t buffer_size, ...) {
   size_t result;
   va_list args;
   va_start(args, buffer_size);
   result= vfmt2023(buffer, buffer_size, args);
   va_end(args);
   return result;
}

int main(void) {
   char initial_static_buffer[512];
   char *buffer= initial_static_buffer;
   size_t needed, bsz= sizeof initial_static_buffer;
   char const *error= 0;
   int t;
   char pstr[]= {"PASCAL uses length-prefixed strings"};
   (void)memmove(pstr + 1, pstr, sizeof pstr - 1); *pstr= sizeof pstr - 1;
   for (t= 8; t; --t) {
      char const *result;
      while (
         (
            result= buffer
            , needed= t == 8 ? sfmt2023(
               buffer, bsz, NULL, "Hello, world!\n"
            ) : t == 7 ? sfmt2023(
               buffer, bsz, "&", "world", NULL, "Ho-ho-ho, hello, &!\n"
            ) : t == 6 ? sfmt2023(
               buffer, bsz
               ,  "${func}", "fmt2023"
               ,  "\v", *pstr, "${string}", pstr + 1
               ,  NULL, "${func}() expands '${string}' as a PASCAL string!\n"
            ) : t == 5 ? sfmt2023(
                  buffer, bsz
               ,  "%1", "2000-04-01"
               ,  "%2", "Mr. April Fool"
               ,  "%3", "I always know everything"
               ,  NULL, "On %1, %2 said '%3'.\n"
            ) : t == 4 ? sfmt2023(
                  buffer, bsz
               ,  "%%", "%"
               ,  "%1", "25"
               ,  "%2", "1000"
               ,  NULL
               ,  "The expression '%1 %% of %2'"
               " means %1 percent of the quantity %2.\n"
            ) : t == 3 ? sfmt2023(
                  buffer, bsz
               ,  "{day}", "24"
               ,  "{month}", "12"
               ,  "{year}", "2000"
               ,  "\n", "{date}", "\f", "{year}-{month}-{day}"
               ,  "\r", &result
               ,  "{who}", "Santa Claus"
               ,  "{msg}", "Ho, ho, ho!"
               ,  NULL, "On {date}, {who} said '{msg}'.\n"
            ) : t == 2 ? sfmt2023(
               buffer, bsz
               , "IDIOTS", "Developers"
               , "Infamous U*X-hater mendaciously"
               , "big OpenSource friend honestly"
               , NULL
               ,  "'IDIOTS, IDIOTS, IDIOTS!'"
                  ", Steve the Infamous U*X-hater mendaciously cheered.\n"
            ) : (
               /* Special case: Can we handle to output nothing at all? */
               t= 1 , sfmt2023(buffer, bsz, NULL, "")
            )
         ) >= bsz
      ) {
         {
            size_t pfib, fib;
            pfib= fib= 1;
            while (fib < needed) {
               size_t t= fib; fib+= pfib; pfib= t;
            }
            needed= fib;
         }
         {
            void *nbuf;
            if (
               !(
                  nbuf= buffer == initial_static_buffer
                  ? malloc(needed)
                  : realloc(buffer, needed)
               )
            ) {
               error= "Out of memory!";
               goto failure;
            }
            buffer= nbuf; bsz= needed;
         }
      }
      assert(needed >= 1);
      --needed;
      assert(buffer[needed] == '\0');
      assert(result >= buffer);
      assert(result <= buffer + needed);
      needed= buffer + needed - result;
      if (fwrite(result, 1, needed, stdout) != needed) {
         write_error:
         error= "Write error!";
         failure:
         (void)fputs(error, stderr);
         (void)fputc('\n', stderr);
      }
   }
   if (buffer != initial_static_buffer) free(buffer);
   if (fflush(0)) goto write_error;
   return error ? EXIT_FAILURE : EXIT_SUCCESS;
}
