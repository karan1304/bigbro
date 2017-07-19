#!/usr/bin/python3

from __future__ import print_function
import glob, os, sys, platform, subprocess

if 'SKIPSPARSE' in os.environ:
    print("# skipping sparse as requested")
    exit(0)

myplatform = sys.platform
if myplatform == 'linux2':
    myplatform = 'linux'

if myplatform != 'linux':
    print('# cannot use sparse on platform', myplatform)
    exit(0)

try:
    ver = subprocess.check_output(['sparse', '--version'],
                                  stderr=subprocess.STDOUT)
except:
    print('# there is no sparse present')
    exit(0)

myver = b'v0.5.0-'
if ver[:len(myver)] == myver:
    print('# looking good!')
else:
    print('# I am not confident with sparse version', ver)

for f in sorted(glob.glob('*.c')):
    if f != 'bigbro-windows.c':
        print('| sparse -Ibigbro -Wsparse-error %s > %s.sparse' % (f, f[:-2]))
        print('< syscalls/linux.h')
        print('> %s.sparse' % (f[:-2]))
