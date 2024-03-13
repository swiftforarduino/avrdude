#!/usr/bin/env python
#
# Simple test program for testing the SWIG interface.
#
# Best call this as "python -i src/swigtest.py",
# then run the interactive interpreter.

# Example:
# getavr("m128")

import sys
import os
import pathlib

builddir = None
if os.name == 'posix':
    # Linux, *BSD, MacOS
    sysname = os.uname()[0].lower()
    builddir = f'build_{sysname}/src'
elif os.name == 'nt':
    # Windows
    for candidate in ['build_msvc/src', 'build_mingw64/src']:
        if os.path.exists(candidate):
            builddir = candidate
            os.add_dll_directory(os.path.realpath(candidate))
            break

if builddir == None:
    print("Cannot determine build directory, module loading might fail.", file=sys.stderr)
else:
    sys.path.append(builddir)

import swig_avrdude as ad

ad.init_config()

found = False
for d in [builddir, "/etc", "/usr/local/etc"]:
    p = pathlib.Path(d + "/avrdude.conf")
    if p.is_file():
        print(f"Found avrdude.conf in {d}")
        ad.read_config(d + "/avrdude.conf")
        found = True
        break

if not found:
    print("Sorry, no avrdude.conf could be found.")
    sys.exit(1)

def avrpart_to_dict(avrpart):

    if str(type(avrpart)).find('AVRPART') < 0:
        raise Exception(f"wrong argument: {type(avrpart)}, expecting swig_avrdude.AVRPART")

    d = {}
    d['desc'] = avrpart.desc
    d['id'] = avrpart.id
    d['family_id'] = avrpart.family_id
    d['config_file'] = avrpart.config_file
    d['lineno'] = avrpart.lineno
    d['mem'] = avrpart.mem

    return d

def avrmem_to_dict(mem):

    if str(type(mem)).find('AVRMEM') < 0:
        raise Exception(f"wrong argument: {type(mem)}, expecting swig_avrdude.AVRMEM")

    d = {}
    d['desc'] = mem.desc
    d['size'] = mem.size
    d['paged'] = mem.paged
    d['page_size'] = mem.page_size
    d['num_pages'] = mem.num_pages

    return d

def avrpart_to_mem(avrpart):

    if str(type(avrpart)).find('AVRPART') < 0:
        raise Exception(f"wrong argument: {type(avrpart)}, expecting swig_avrdude.AVRPART")

    res = []
    m = ad.lfirst(avrpart.mem)
    while m:
        mm = ad.ldata_avrmem((m))
        res.append(avrmem_to_dict(mm))
        m = ad.lnext(m)

    return res

def getavr(name: str):

    p = ad.locate_part(ad.cvar.part_list, name)
    if not p:
        print(f"No part named {name} found")
        return
    pp = avrpart_to_dict(p)
    mm = avrpart_to_mem(p)
    print(f"AVR part {name} found as {pp['desc']}, or {pp['id']}")
    print(f"Definition in {pp['config_file']}, line {pp['lineno']}")
    print("Memory overview:")
    print( "Name        size   paged   page_size num_pages")
    for m in mm:
        print(f"{m['desc']:11s} {m['size']:6d}  {str(m['paged']):5s}   {m['page_size']:4d}      {m['num_pages']:3d}")
    print("")
