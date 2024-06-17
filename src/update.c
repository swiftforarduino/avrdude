/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2000-2005 Brian S. Dean <bsd@bdmicro.com>
 * Copyright (C) 2007 Joerg Wunsch
 * Copyright (C) 2022- Stefan Rueger <stefan.rueger@urclocks.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* $Id$ */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <ac_cfg.h>

#include "avrdude.h"
#include "libavrdude.h"


/*
 * Parsing of [<memory>:<op>:<file>[:<fmt>] | <file>[:<fmt>]]
 *
 * As memory names don't contain colons and the r/w/v operation <op> is
 * a single character, check whether the first two colons sandwich one
 * character. If not, treat the argument as a filename (defaulting to
 * flash write). This allows colons in filenames other than those for
 * enclosing <op> and separating <fmt>, eg, C:/some/file.hex
 */
UPDATE *parse_op(const char *s) {
  // Assume -U <file>[:<fmt>] first
  UPDATE *upd = (UPDATE *) mmt_malloc(sizeof *upd);
  upd->memstr = NULL;           // Defaults to flash or application
  upd->op = DEVICE_WRITE;
  const char *fn = s;

  // Check for <memory>:c: start in which case override defaults
  const char *fc = strchr(s, ':');
  if(fc && fc[1] && fc[2] == ':') {
    if(!strchr("rwv", fc[1])) {
      pmsg_error("invalid I/O mode :%c: in -U %s\n", fc[1], s);
      imsg_error("I/O mode can be r, w or v for read, write or verify device\n");
      mmt_free(upd->memstr);
      mmt_free(upd);
      return NULL;
    }

    upd->memstr = memcpy(mmt_malloc(fc-s+1), s, fc-s);
    upd->op =
      fc[1]=='r'? DEVICE_READ:
      fc[1]=='w'? DEVICE_WRITE: DEVICE_VERIFY;
    fn = fc+3;
  }

  // Default to AUTO for write and verify, and to raw binary for read
  upd->format = upd->op == DEVICE_READ? FMT_RBIN: FMT_AUTO;

  // Filename: last char is format if the penultimate char is a colon
  size_t len = strlen(fn);
  if(len > 2 && fn[len-2] == ':') { // Assume format specified
    upd->format = fileio_format(fn[len-1]);
    if(upd->format == FMT_ERROR) {
      pmsg_error("invalid file format :%c in -U %s; known formats are\n", fn[len-1], s);
      for(int f, c, i=0; i<62; i++) {
        c = i<10? '0'+i: (i&1? 'A': 'a') + (i-10)/2;
        f = fileio_format(c);
        if(f != FMT_ERROR)
          imsg_error("  :%c %s\n", c, fileio_fmtstr(f));
      }
      mmt_free(upd->memstr);
      mmt_free(upd);
      return NULL;
    }
    len -= 2;
  }

  upd->filename = memcpy(mmt_malloc(len+1), fn, len);

  return upd;
}


UPDATE *dup_update(const UPDATE *upd) {
  UPDATE *u = (UPDATE *) mmt_malloc(sizeof *u);
  memcpy(u, upd, sizeof*u);
  u->memstr = upd->memstr? mmt_strdup(upd->memstr): NULL;
  u->filename = mmt_strdup(upd->filename);

  return u;
}

UPDATE *new_update(int op, const char *memstr, int filefmt, const char *fname) {
  UPDATE *u = (UPDATE *) mmt_malloc(sizeof *u);
  u->memstr = mmt_strdup(memstr);
  u->filename = mmt_strdup(fname);
  u->op = op;
  u->format = filefmt;

  return u;
}

UPDATE *cmd_update(const char *cmd) {
  UPDATE *u = (UPDATE *) mmt_malloc(sizeof *u);
  u->cmdline = cmd;

  return u;
}

void free_update(UPDATE *u) {
  if(u) {
    mmt_free(u->memstr);
    mmt_free(u->filename);
    memset(u, 0, sizeof *u);
    mmt_free(u);
  }
}

char *update_str(const UPDATE *upd) {
  if(upd->cmdline)
    return mmt_sprintf("-%c %s",
      str_eq("interactive terminal", upd->cmdline)? 't': 'T', upd->cmdline);
  return mmt_sprintf("-U %s:%c:%s:%c",
    upd->memstr,
    upd->op == DEVICE_READ? 'r': upd->op == DEVICE_WRITE? 'w': 'v',
    upd->filename,
    fileio_fmtchr(upd->format));
}

// Memory statistics considering holes after a file read returned size bytes
int memstats(const AVRPART *p, const char *memstr, int size, Filestats *fsp) {
  AVRMEM *mem = avr_locate_mem(p, memstr);

  if(!mem) {
    pmsg_error("%s %s undefined\n", p->desc, memstr);
    return LIBAVRDUDE_GENERAL_FAILURE;
  }

  return memstats_mem(p, mem, size, fsp);
}

// Memory statistics considering holes after a file read returned size bytes
int memstats_mem(const AVRPART *p, const AVRMEM *mem, int size, Filestats *fsp) {
  Filestats ret = { 0 };

  if(!mem->buf || !mem->tags) {
    pmsg_error("%s %s is not set\n", p->desc, mem->desc);
    return LIBAVRDUDE_GENERAL_FAILURE;
  }

  int pgsize = mem->page_size;
  if(pgsize < 1)
    pgsize = 1;

  if(size < 0 || size > mem->size) {
    pmsg_error("size %d at odds with %s %s size %d\n", size, p->desc, mem->desc, mem->size);
    return LIBAVRDUDE_GENERAL_FAILURE;
  }

  ret.lastaddr = -1;
  int firstset = 0, insection = 0;
  // Scan all memory
  for(int addr = 0; addr < mem->size; ) {
    int pageset = 0;
    // Go page by page
    for(int pgi = 0; pgi < pgsize; pgi++, addr++) {
      if(mem->tags[addr] & TAG_ALLOCATED) {
        if(!firstset) {
          firstset = 1;
          ret.firstaddr = addr;
        }
        ret.lastaddr = addr;
        // size can be smaller than tags suggest owing to flash trailing-0xff
        if(addr < size) {
          ret.nbytes++;
          if(!pageset) {
            pageset = 1;
            ret.nfill += pgi;
            ret.npages++;
          }
          if(!insection) {
            insection = 1;
            ret.nsections++;
          }
        } else {                // Now beyond size returned by input file read
          ret.ntrailing++;
          if(pageset)
            ret.nfill++;
        }
      } else {                  // In a hole or beyond input file
        insection = 0;
        if(pageset)
          ret.nfill++;
      }
    }
  }

  if(fsp)
    *fsp = ret;

  return LIBAVRDUDE_SUCCESS;
}


// Helper functions for dry run to determine file access

int update_is_okfile(const char *fn) {
  struct stat info;

  // File exists and is a regular file or a character file, eg, /dev/urandom
  return fn && *fn && stat(fn, &info) == 0 && !!(info.st_mode & (S_IFREG | S_IFCHR));
}

int update_is_writeable(const char *fn) {
  if(!fn || !*fn)
    return 0;

  // Assume writing to stdout will be OK
  if(str_eq(fn, "-"))
    return 1;

  // File exists? If so return whether it's readable and an OK file type
  if(access(fn, F_OK) == 0)
    return access(fn, W_OK) == 0 && update_is_okfile(fn);

  // File does not exist: try to create it
  FILE *test = fopen(fn, "w");
  if(test) {
    unlink(fn);
    fclose(test);
  }
  return !!test;
}

int update_is_readable(const char *fn) {
  if(!fn || !*fn)
    return 0;

  // Assume reading from stdin will be OK
  if(str_eq(fn, "-"))
    return 1;

  // File exists, is readable by the process and an OK file type?
  return access(fn, R_OK) == 0 && update_is_okfile(fn);
}


static void ioerror(const char *iotype, const UPDATE *upd) {
  int errnocp = errno;

  pmsg_ext_error("file %s is not %s: ", str_outname(upd->filename), iotype);
  if(errnocp)
    msg_ext_error("%s", strerror(errnocp));
  else if(upd->filename && *upd->filename)
    msg_ext_error("(not a regular or character file?)");
  msg_ext_error("\n");
}

// Basic checks to reveal serious failure before programming (and on autodetect set format)
int update_dryrun(const AVRPART *p, UPDATE *upd) {
  int known, format_detect, ret = LIBAVRDUDE_SUCCESS;

  if(upd->cmdline) {            // Todo: parse terminal command line?
    cx->upd_termcmds = mmt_realloc(cx->upd_termcmds, sizeof(*cx->upd_termcmds) * (cx->upd_nterms+1));
    cx->upd_termcmds[cx->upd_nterms++] = upd->cmdline;
    return 0;
  }

  /*
   * Allow memory name to be a list. Reject an update if memory name is not
   * known amongst any part (suspect a typo) but accept when the specific part
   * does not have it (allow unifying i/faces); also accept pseudo memory all
   */
  char *umstr = upd->memstr, *dstr = mmt_strdup(umstr), *s = dstr, *e;
  for(e = strchr(s, ','); 1; e = strchr(s, ',')) {
    if(e)
      *e = 0;
    s = str_trim(s);
    if(*s && !avr_mem_might_be_known(s) && !str_eq(s, "all")) {
      pmsg_error("unknown memory %s in -U %s:...\n", s, umstr);
      ret = LIBAVRDUDE_GENERAL_FAILURE;
      break;
    } else if(*s && !avr_locate_mem(p, s))
      ret = LIBAVRDUDE_SOFTFAIL;
    if(!e)
      break;
    s = e+1;
  }
  mmt_free(dstr);

  known = 0;
  // Necessary to check whether the file is readable?
  if(upd->op == DEVICE_VERIFY || upd->op == DEVICE_WRITE || upd->format == FMT_AUTO) {
    if(upd->format != FMT_IMM) {
      // Need to read the file: was it written before, so will be known?
      for(int i = 0; i < cx->upd_nfwritten; i++)
        if(!cx->upd_wrote || (upd->filename && str_eq(cx->upd_wrote[i], upd->filename)))
          known = 1;
      // Could a -T terminal command have created the file?
      for(int i = 0; i < cx->upd_nterms; i++)
        if(!cx->upd_termcmds || (upd->filename && str_contains(cx->upd_termcmds[i], upd->filename)))
          known = 1;
      // Any -t interactive terminal could have created it
      for(int i = 0; i < cx->upd_nterms; i++)
        if(!cx->upd_termcmds || str_eq(cx->upd_termcmds[i], "interactive terminal"))
          known = 1;

      errno = 0;
      if(!known && !update_is_readable(upd->filename)) {
        ioerror("readable", upd);
        ret = LIBAVRDUDE_SOFTFAIL; // Even so it might still be there later on
        known = 1;              // Pretend we know it, so no auto detect needed
      }
    }
  }

  if(!known && upd->format == FMT_AUTO) {
    if(str_eq(upd->filename, "-")) {
      pmsg_error("cannot auto detect file format for stdin/out, specify explicitly\n");
      ret = LIBAVRDUDE_GENERAL_FAILURE;
    } else if((format_detect = fileio_fmt_autodetect(upd->filename)) < 0) {
      pmsg_warning("cannot determine file format for %s, specify explicitly\n", upd->filename);
      ret = LIBAVRDUDE_SOFTFAIL;
    } else {
      // Set format now (but might be wrong in edge cases, where user needs to specify explicity)
      upd->format = format_detect;
      if(quell_progress < 2)
        pmsg_notice("%s file %s auto detected as %s\n",
          upd->op == DEVICE_READ? "output": "input", upd->filename,
          fileio_fmtstr(upd->format));
    }
  }

  switch(upd->op) {
  case DEVICE_READ:
    if(upd->format == FMT_IMM) {
      pmsg_error("invalid file format 'immediate' for output\n");
      ret = LIBAVRDUDE_GENERAL_FAILURE;
    } else {
      errno = 0;
      if(!update_is_writeable(upd->filename)) {
        ioerror("writeable", upd);
        ret = LIBAVRDUDE_SOFTFAIL;
      } else if(upd->filename) { // Record filename (other than stdout) is available for future reads
        if(!str_eq(upd->filename, "-") &&
          (cx->upd_wrote = mmt_realloc(cx->upd_wrote, sizeof(*cx->upd_wrote) * (cx->upd_nfwritten+1))))
          cx->upd_wrote[cx->upd_nfwritten++] = upd->filename;
      }
    }
    break;

  case DEVICE_VERIFY:           // Already checked that file is readable
  case DEVICE_WRITE:
    break;

  default:
    pmsg_error("invalid update operation (%d) requested\n", upd->op);
    ret = LIBAVRDUDE_GENERAL_FAILURE;
  }

  return ret;
}

// Whether a memory should be backup-ed: exclude sub-memories
static int is_backup_mem(const AVRPART *p, const AVRMEM *mem) {
  return mem_is_in_flash(mem)? mem_is_flash(mem):
    mem_is_in_sigrow(mem)? mem_is_sigrow(mem):
    mem_is_in_fuses(mem)? mem_is_fuses(mem) || !avr_locate_fuses(p):
    mem_is_io(mem)? 0:
    !mem_is_sram(mem);
}


int do_op(const PROGRAMMER *pgm, const AVRPART *p, const UPDATE *upd, enum updateflags flags) {
  int retval = LIBAVRDUDE_GENERAL_FAILURE;
  AVRPART *v;
  AVRMEM *mem, **umemlist = NULL, *m;
  Segment *seglist = NULL;
  Filestats fs, fs_patched;
  char *tofree;
  const char *umstr = upd->memstr;

  lmsg_info("\n");              // Ensure an empty line for visual separation of operations
  pmsg_info("processing %s\n", tofree = update_str(upd));
  mmt_free(tofree);

  if(upd->cmdline) {
    if(!str_eq(upd->cmdline, "interactive terminal"))
      return terminal_line(pgm, p, upd->cmdline);
    // Interactive terminal shell
    clearerr(stdin);
    return terminal_mode(pgm, p);
  }

  int size, len, maxmemstrlen = 0, ns = 0;
  // Compute list of multiple memories if umstr indicates so
  if(str_eq(umstr, "all") || strchr(umstr, ',')) {
    ns = (lsize(p->mem) + 1) * ((int) str_numc(umstr, ',') + 1); // Upper limit of memories
    umemlist = mmt_malloc(ns*sizeof*umemlist);
    ns = 0;                     // Now count how many there really are mentioned

    char *dstr = mmt_strdup(umstr), *s = dstr, *e;
    for(e = strchr(s, ','); 1; e = strchr(s, ',')) {
      if(e)
        *e = 0;
      s = str_trim(s);
      if(str_eq(s, "all")) {
        for(LNODEID lm = lfirst(p->mem); lm; lm = lnext(lm))
          if(is_backup_mem(p, (m = ldata(lm))))
            umemlist[ns++] = m;
      } else if(!*s) {          // Ignore empty list elements
      } else {
        if(!(m = avr_locate_mem(p, s)))
          pmsg_warning("skipping unknown memory %s in list -U %s:...\n", s, umstr);
        else
          umemlist[ns++] = m;
      }
      if(!e)
        break;
      s = e+1;
    }
    mmt_free(dstr);
    // De-duplicate list, keeping order
    for(int i=0; i < ns; i++) {
      m = umemlist[i];
      // Move down remaining list whenever same memory detected
      for(int j = i+1; j < ns; j++)
         for(; j < ns && m == umemlist[j]; ns--)
           memmove(umemlist+j, umemlist+j+1, (ns-j-1)*sizeof*umemlist);
    }

    if(!ns) {
      pmsg_warning("skipping -U %s:... as no memory in part %s available\n", umstr, p->desc);
      mmt_free(umemlist);
      return LIBAVRDUDE_SOFTFAIL;
    }
    // Maximum length of memory name for to-be-read memories
    for(int i=0; i<ns; i++)
      if((len = strlen(avr_mem_name(p, umemlist[i]))) > maxmemstrlen)
        maxmemstrlen = len;
    seglist = mmt_malloc(ns*sizeof*seglist);
  }

  mem = umemlist? avr_new_memory("multi", ANY_MEM_SIZE): avr_locate_mem(p, umstr);
  if (mem == NULL) {
    pmsg_warning("skipping -U %s:... as memory not defined for part %s\n", umstr, p->desc);
    return LIBAVRDUDE_SOFTFAIL;
  }

  const char *mem_desc = !umemlist? avr_mem_name(p, mem):
    ns==1? avr_mem_name(p, umemlist[0]): "multiple memories";
  int rc = 0;
  switch (upd->op) {
  case DEVICE_READ:
    // Read out the specified device memory and write it to a file
    if (upd->format == FMT_IMM) {
      pmsg_error("invalid file format 'immediate' for output\n");
      goto error;
    }
    if(umemlist) {
      /*
       * Writing to all memories or a list of memories. It is crucial not to
       * skip empty flash memory: otherwise the output file cannot distinguish
       * between flash having been deliberately dropped by the user or it
       * having been empty. Therefore the code switches temporarily off
       * trailing 0xff optimisation. In theory, the output file could only
       * store those pages that are non-empty for a paged memory, and if it was
       * all empty, store only the first empty page to indicate the memory was
       * selected. However, file space on a PC is cheap and fast; the main use
       * case for saving "all" memory is a backup, and AVRDUDE does not want to
       * rely on the uploader to know that the backup file requires the paged
       * memories to be erased first, so the code goes the full hog.
       */
      int dffo = cx->avr_disableffopt;
      cx->avr_disableffopt = 1;
      pmsg_info("reading %s ...\n", mem_desc);
      int nn = 0;
      for(int ii = 0; ii < ns; ii++) {
        m = umemlist[ii];
        const char *m_name = avr_mem_name(p, m);
        const char *caption = str_ccprintf("Reading %-*s", maxmemstrlen, m_name);
        report_progress(0, 1, caption);
        int ret = avr_read_mem(pgm, p, m, NULL);
        report_progress(1, 1, NULL);
        if(ret < 0) {
          pmsg_warning("unable to read %s (ret = %d), skipping...\n", m_name, ret);
          continue;
        }
        unsigned off = fileio_mem_offset(p, m);
        if(off == -1U) {
          pmsg_warning("cannot map %s to flat address space, skipping ...\n", m_name);
          continue;
        }
        if(ret > 0) {
          // Copy individual memory into multi memory
          memcpy(mem->buf+off, m->buf, ret);
          seglist[nn].addr = off;
          seglist[nn].len = ret;
          nn++;
        }
      }

      if(nn)
        rc = fileio_segments(FIO_WRITE, upd->filename, upd->format, p, mem, nn, seglist);
      else
        pmsg_notice("empty memory, resulting file has no contents\n");
      cx->avr_disableffopt = dffo;
    } else {                    // Regular file
      pmsg_info("reading %s memory ...\n", mem_desc);
      if(mem->size > 32 || verbose > 1)
        report_progress(0, 1, "Reading");

      rc = avr_read(pgm, p, umstr, 0);
      report_progress(1, 1, NULL);
      if (rc < 0) {
        pmsg_error("unable to read all of %s, rc=%d\n", mem_desc, rc);
        goto error;
      }
      if (rc == 0)
        pmsg_notice("empty memory, resulting file has no contents\n");
      pmsg_info("writing output file %s\n", str_outname(upd->filename));
      rc = fileio_mem(FIO_WRITE, upd->filename, upd->format, p, mem, rc);
    }

    if (rc < 0) {
      pmsg_error("write to file %s failed\n", str_outname(upd->filename));
      goto error;
    }

    break;

  case DEVICE_WRITE:
    // Write the selected device memory/ies using data from a file

    pmsg_info("reading input file %s for %s\n", str_inname(upd->filename), mem_desc);
    rc = fileio_mem(FIO_READ, upd->filename, upd->format, p, mem, -1);
    if (rc < 0) {
      pmsg_error("read from file %s failed\n", str_inname(upd->filename));
      goto error;
    }
    if(memstats_mem(p, mem, rc, &fs) < 0)
      goto error;

    imsg_info("with %d byte%s in %d section%s within %s\n",
      fs.nbytes, str_plural(fs.nbytes),
      fs.nsections, str_plural(fs.nsections),
      str_ccinterval(fs.firstaddr, fs.lastaddr));
    if(mem->page_size > 1) {
      imsg_info("using %d page%s and %d pad byte%s",
        fs.npages, str_plural(fs.npages),
        fs.nfill, str_plural(fs.nfill));
      if(fs.ntrailing)
        msg_info(", cutting off %d trailing 0xff byte%s",
          fs.ntrailing, str_plural(fs.ntrailing));
      msg_info("\n");
    }



    // Patch flash input, eg, for vector bootloaders
    if(pgm->flash_readhook) {
      AVRMEM *mem = avr_locate_mem(p, umstr);
      if(mem && mem_is_flash(mem)) {
        rc = pgm->flash_readhook(pgm, p, mem, upd->filename, rc);
        if (rc < 0) {
          pmsg_notice("readhook for file %s failed\n", str_inname(upd->filename));
          goto error;
        }
        if(memstats(p, umstr, rc, &fs_patched) < 0)
          goto error;
        if(memcmp(&fs_patched, &fs, sizeof fs)) {
          pmsg_info("preparing flash input for device%s\n",
            pgm->prog_modes & PM_SPM? " bootloader": "");
            imsg_notice2("with %d byte%s in %d section%s within %s\n",
              fs_patched.nbytes, str_plural(fs_patched.nbytes),
              fs_patched.nsections, str_plural(fs_patched.nsections),
              str_ccinterval(fs_patched.firstaddr, fs_patched.lastaddr));
            if(mem->page_size > 1) {
              imsg_notice2("using %d page%s and %d pad byte%s",
                fs_patched.npages, str_plural(fs_patched.npages),
                fs_patched.nfill, str_plural(fs_patched.nfill));
              if(fs_patched.ntrailing)
                msg_notice2(", and %d trailing 0xff byte%s",
                  fs_patched.ntrailing, str_plural(fs_patched.ntrailing));
              msg_notice2("\n");
            }
        }
      }
    }
    size = rc;

    // Write the buffer contents to the selected memory
    pmsg_info("writing %d byte%s to %s ...\n", fs.nbytes,
      str_plural(fs.nbytes), mem_desc);

    if (!(flags & UF_NOWRITE)) {
      if(mem->size > 32 || verbose > 1)
        report_progress(0, 1, "Writing");
      rc = avr_write(pgm, p, umstr, size, (flags & UF_AUTO_ERASE) != 0);
      report_progress(1, 1, NULL);
    } else {
      // Test mode: write to stdout in intel hex rather than to the chip
      rc = fileio(FIO_WRITE, "-", FMT_IHEX, p, umstr, size);
    }

    if (rc < 0) {
      pmsg_error("unable to write %s, rc=%d\n", mem_desc, rc);
      goto error;
    }

    pmsg_info("%d byte%s of %s written\n", fs.nbytes, str_plural(fs.nbytes), mem_desc);

    if (!(flags & UF_VERIFY))   // Fall through for auto verify unless
      break;
    // Fall through

  case DEVICE_VERIFY:
    // Verify that the in memory file is the same as what is on the chip
    led_set(pgm, LED_VFY);

    int userverify = upd->op == DEVICE_VERIFY; // Explicit -U :v by user

    pmsg_info("verifying %s against %s\n", mem_desc, str_inname(upd->filename));

    // No need to read file when fallen through from DEVICE_WRITE
    if (userverify) {
      pmsg_notice("load %s data from input file %s\n", mem_desc, str_inname(upd->filename));

      rc = fileio(FIO_READ_FOR_VERIFY, upd->filename, upd->format, p, umstr, -1);

      if (rc < 0) {
        pmsg_error("read from file %s failed\n", str_inname(upd->filename));
        led_set(pgm, LED_ERR);
        led_clr(pgm, LED_VFY);
        goto error;
      }
      size = rc;

      if(memstats(p, umstr, size, &fs) < 0) {
        led_set(pgm, LED_ERR);
        led_clr(pgm, LED_VFY);
        goto error;
      }
    } else {
      // Correct size of last read to include potentially cut off, trailing 0xff (flash)
      size = fs.lastaddr+1;
    }

    v = avr_dup_part(p);

    if (quell_progress < 2) {
      if (userverify)
        pmsg_notice("input file %s contains %d byte%s\n",
          str_inname(upd->filename), fs.nbytes, str_plural(fs.nbytes));
      pmsg_notice2("reading on-chip %s data ...\n", mem_desc);
    }

    if(mem->size > 32 || verbose > 1)
      report_progress (0,1,"Reading");
    rc = avr_read(pgm, p, umstr, v);
    report_progress (1,1,NULL);
    if (rc < 0) {
      pmsg_error("unable to read all of %s, rc = %d\n", mem_desc, rc);
      led_set(pgm, LED_ERR);
      led_clr(pgm, LED_VFY);
      avr_free_part(v);
      goto error;
    }

    if (quell_progress < 2)
      pmsg_notice2("verifying ...\n");

    rc = avr_verify(pgm, p, v, umstr, size);
    if (rc < 0) {
      pmsg_error("verification mismatch\n");
      led_set(pgm, LED_ERR);
      led_clr(pgm, LED_VFY);
      avr_free_part(v);
      goto error;
    }

    int verified = fs.nbytes+fs.ntrailing;
    pmsg_info("%d byte%s of %s verified\n", verified, str_plural(verified), mem_desc);

    led_clr(pgm, LED_VFY);
    avr_free_part(v);
    break;

  default:
    pmsg_error("invalid update operation (%d) requested\n", upd->op);
    goto error;
  }

  retval = LIBAVRDUDE_SUCCESS;

error:
  if(umemlist) {
    avr_free_mem(mem);
    mmt_free(umemlist);
    mmt_free(seglist);
  }
  return retval;
}
