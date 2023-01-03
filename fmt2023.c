/* fmt2023 - yet another sprintf() replacement idea.
 *
 * I have had so many of those ideas, one worse than the other, that I started
 * naming them by the year when the next brainstorm hit me.
 *
 * Version 2023.2
 * Copyright (c) 2023 Guenther Brunthaler. All rights reserved.
 *
 * This source file is free software.
 * Distribution is permitted under the terms of the GPLv3. */

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
   struct insertion_sequence *insertions;
   /* Consider the terminating null character to be part of the format string.
    * This automatically terminates the current output string once all of the
    * format string's bytes have been processed. */
   size_t remaining= strlen(format) + 1;
   if (!(trigger= o->cmd_introducer) && (insertions= o->insertions)) {
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
            bytes= format + remaining - source;
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
            case '\f':
               cmd= va_arg(*o->args, char const *);
               fmt2023_expand(o, cmd);
               break;
            case '\n':
               nsq->name= va_arg(*o->args, char const *);
               nsq->expansion= o->outpos;
               goto register_insertion;
            default:
               nsq->name= cmd;
               nsq->expansion= va_arg(*o->args, void *);
               register_insertion:
               nsq->name_bytecount= strlen(nsq->name);
               nsq->expansion_bytecount=
                  o->next_size_known
                  ? (o->next_size_known= 0 , o->next_size)
                  : strlen(nsq->expansion)
               ;
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
   while (fmt2023_worker(o, &i)) fmt2023_recurser(o);
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
 * 0, <final_format>: Stop processing the argument list and use <final_format>
 * as the last format string to be processed.
 *
 * "\v...", <bytes>: Set size_t value <bytes> as an override for the next
 * insertion string definition. This means that insertion sequence does not
 * need to be a null terminated string, but can be anyting binary. Think of
 * "void *" as a mnemonic.
 *
 * "\r...", <addr>: Write the current output address to a pointer variable
 * with address <addr>. This allows to store the starting addresses of the
 * next string to be formatted, avoiding the need to search for null bytes
 * in the buffer as string separators within the formatted result. In case
 * of "no write" mode, the written pointer values have no significance and
 * must be ignored. Think of "report" as a mnemonic.
 *
 * "\n..." <name>: Remember the current output position as the starting address
 * of a new insertion sequence <name>. This has pretty much the same affect as
 * specifying a normal insertion sequence, except that the buffer address is
 * implicitly provided. Can be also be combined with a preceding "\v" sequence.
 * Think of "new" as a mnemonic.
 *
 * "\f...", <format>: Expand the format string <format>, appending the null
 * terminated result at the current output position. Move the new output
 * position one byte after the terminating null byte. Only insertion sequences
 * already defined so far can be referenced from within the format string.
 * Think of "format" as a mnemonic.
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
 * expands to a single "%". Then one could write "The expression '%1 %%' means
 * %1 percent of the quantity %2.".
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
 * format introduction character loses ist special meaning and is copied to
 * the output buffer literally.
 *
 * No sophisticated search algorithm is used to locate the named insertion
 * sequences. A simple linear search backwards is done instead. This means
 * that names defined later will be found faster. And in case of duplicate
 * names, only the last instance of the name definied so far will be used.
 * This can even be exploited for "redefining" names if multiple format
 * strings are used. */
static size_t fmt2023(char *buffer, size_t buffer_size, ...) {
   struct fmt2023_ctx o;
   va_list args;
   o.args= &args;
   o.no_write= !(o.outpos= buffer);
   o.next_size_known= 0;
   o.insertions= 0;
   o.cmd_introducer= '\0';
   o.stop= buffer + buffer_size;
   va_start(args, buffer_size);
   fmt2023_recurser(&o);
   va_end(args);
   assert(o.outpos >= buffer);
   return (size_t)(o.outpos - buffer);
}

int main(void) {
   char initial_static_buffer[512];
   char *buffer= initial_static_buffer;
   size_t needed, bsz= sizeof initial_static_buffer;
   char const *error= 0;
   while (
      (
         needed= fmt2023(
            buffer, bsz, NULL, "Hello, world!\n"
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
   if (fwrite(buffer, 1, needed, stdout) != needed) {
      error= "Write error!";
      failure:
      (void)fputs(error, stderr);
      (void)fputc('\n', stderr);
   }
   if (buffer != initial_static_buffer) free(buffer);
   return error ? EXIT_FAILURE : EXIT_SUCCESS;
}
