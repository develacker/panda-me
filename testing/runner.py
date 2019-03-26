USAGE="""runner.py binary args [option]

Advanced USAGE:

    --rr turns on Mozilla rr record for your command

    --arch specifies another architecture (Default is i386)
	
    --snapshot specifies loading a different snapshot for your vm (default is "root")
        
    --qcow specifies a path to an alternate qcow (otherwise uses/installs a qcow in $(HOME)/.panda)

    --env "PYTHON_DICT" where PYTHON_DICT represents the user environment
                         you would like to enforce on the guest
        eg.  --env "{'CC':'/bin/gcc', 'LD_LIBRARY_PATH':'/usr/bin/gcc'}"

    --stdin args is a input file as STDIN input for target binary
"""

from collections import namedtuple

Arch = namedtuple('Arch', ['dir', 'binary', 'prompt', 'qcow', 'cdrom', 'extra_files', 'extra_args'])
Arch.__new__.__defaults__ = (None,None)

SUPPORTED_ARCHES = {
    'i386': Arch('i386-softmmu', 'qemu-system-i386', "root@debian-i386:~#", "wheezy_panda2.qcow2", "ide1-cd0"),
    'x86_64': Arch('x86_64-softmmu', 'qemu-system-x86_64', "root@debian-amd64:~#", "wheezy_x64.qcow2", "ide1-cd0"),
    'ppc': Arch('ppc-softmmu', 'qemu-system-ppc', "root@debian-powerpc:~#", "ppc_wheezy.qcow", "ide1-cd0"),
    'arm': Arch('arm-softmmu', 'qemu-system-arm', "root@debian-armel:~#", "arm_wheezy.qcow", "scsi0-cd2", 
        extra_files=['vmlinuz-3.2.0-4-versatile', 'initrd.img-3.2.0-4-versatile'],
        extra_args='-M versatilepb -append "root=/dev/sda1" -kernel {DOT_DIR}/vmlinuz-3.2.0-4-versatile -initrd {DOT_DIR}/initrd.img-3.2.0-4-versatile')
}

import os
import shlex
import shutil
import subprocess as sp
import sys
import argparse
import logging

import json
import pipes
import socket
import sys
import time

from colorama import Fore, Style
from errno import EEXIST
from os.path import abspath, join, realpath, basename, dirname
from traceback import print_exception

from expect import Expect
from tempdir import TempDir

home_dir = os.getenv("HOME")
dot_dir = join(home_dir, '.panda')

if not (os.path.exists(dot_dir)):
    os.mkdir(dot_dir)

this_script = os.path.abspath(__file__)
this_script_dir = dirname(this_script)
default_build_dir = join(dirname(dirname(this_script_dir)), 'build')
panda_build_dir = os.getenv("PANDA_BUILD", default_build_dir)

filemap = {}
debug = True

def env_to_list(env):
    if sys.version_info[:2] < (3,3):
        return ["{}='{}'".format(k, v) for k, v in env.iteritems()]
    else:
        return ["{}='{}'".format(k, v) for k, v in env.items()]

def progress(msg):
    print (Fore.GREEN + '[runner.py] ' + Fore.RESET + Style.BRIGHT + msg + Style.RESET_ALL)
    print ("")

class Qemu(object):
    def __init__(self, qemu_path, qcow, snapshot, tempdir, expect_prompt,
                 boot=False, rr=False, perf=False, extra_args=None):
        assert not (perf and rr)
        self.qemu_path = qemu_path
        self.qcow = qcow
        self.snapshot = snapshot
        self.tempdir = tempdir
        self.rr = rr
        self.perf = perf
        self.boot = boot
        self.expect_prompt = expect_prompt
        self.extra_args = extra_args or []

    # types a command into the qemu monitor and waits for it to complete
    def run_monitor(self, cmd):
        if debug:
            print ("monitor cmd: [%s]" % cmd)
        print (Style.BRIGHT + "(qemu)" + Style.RESET_ALL)
        print (self.monitor.sendline(cmd))
        print (self.monitor.expect("(qemu)"))
        print 
        print 

    def type_console(self, cmd):
        assert (not self.boot)
        if debug:
            print ("console cmd: [%s]" % cmd)
        self.console.send(cmd)

    # types a command into the guest os and waits for it to complete
    def run_console(self, cmd=None, timeout=30):
        assert (not self.boot)
        if cmd is not None:
            self.type_console(cmd)
        print (Style.BRIGHT + self.expect_prompt + Style.RESET_ALL)
        print (self.console.sendline())
        print (self.console.expect(self.expect_prompt, timeout=timeout))
        print 
        print

    def __enter__(self):
        monitor_path = join(self.tempdir, 'monitor')
        if not self.boot:
            serial_path = join(self.tempdir, 'serial')

        qemu_args = [self.qemu_path, self.qcow]
#        if not self.boot:
#
        qemu_args.extend(['-monitor', 'unix:{},server,nowait'.format(monitor_path)])
        if self.boot:
            qemu_args.append('-S')
        else:
            qemu_args.extend(['-serial', 'unix:{},server,nowait'.format(serial_path),
                              '-loadvm', self.snapshot])
        qemu_args.extend(['-display', 'none'])
        qemu_args.extend(self.extra_args)
        if self.rr: qemu_args = ['rr', 'record'] + qemu_args
        if self.perf: qemu_args = ['perf', 'record'] + qemu_args

        progress("Running qemu with args:")
        print (sp.list2cmdline(qemu_args))

        self.qemu = sp.Popen(qemu_args) # , stdout=DEVNULL, stderr=DEVNULL)
        while not os.path.exists(monitor_path):
            time.sleep(0.1)
        if not self.boot:
            while not os.path.exists(serial_path):
                time.sleep(0.1)
#        while not all([os.path.exists(p) for p in [monitor_path, serial_path]]):
#            time.sleep(0.1)

        self.monitor_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.monitor_socket.connect(monitor_path)
        self.monitor = Expect(self.monitor_socket)
        if not self.boot:
            self.serial_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.serial_socket.connect(serial_path)
            self.console = Expect(self.serial_socket)

        # Make sure monitor/console are in right state.
        self.monitor.expect("(qemu)")
        print
        if not self.boot:
            self.console.sendline()
            self.console.expect(self.expect_prompt)
        print
        print

        return self

    def __exit__(self, exc_type, exc_value, traceback):
        if traceback:
            print_exception(exc_type, exc_value, traceback)
        else:
            self.monitor.sendline("quit")
            self.monitor_socket.close()
            if not self.boot:
                self.serial_socket.close()

        try:
            self.qemu.wait(timeout=3)
        except sp.TimeoutExpired:
            progress("Qemu stailed. Sending SIGTERM...")
            self.qemu.terminate()

        print

def make_iso(directory, iso_path):
    with open(os.devnull, "w") as DEVNULL:
        if sys.platform.startswith('linux'):
            sp.check_call([
                'genisoimage', '-RJ', '-max-iso9660-filenames', '-o', iso_path, directory
            ], stderr=sp.STDOUT if debug else DEVNULL)
        elif sys.platform == 'darwin':
            sp.check_call([
                'hdiutil', 'makehybrid', '-hfs', '-joliet', '-iso', '-o', iso_path, directory
            ], stderr=sp.STDOUT if debug else DEVNULL)
        else:
            raise NotImplementedError("Unsupported operating system!")

# command as array of args.
# copy_directory gets mounted in the same place on the guest as an iso/CD-ROM.
def create_recording(qemu_path, qcow, snapshot, command, copy_directory,
                     recording_path, expect_prompt, cdrom, isoname=None, rr=False, savevm=False,
                     perf=False, env={}, extra_args=None, stdin=False):
    assert not (rr and perf)

    recording_path = realpath(recording_path)
    if not isoname: isoname = copy_directory + '.iso'

    with TempDir() as tempdir, \
            Qemu(qemu_path, qcow, snapshot, tempdir, rr=rr, perf=perf,
                 expect_prompt=expect_prompt, extra_args=extra_args) as qemu:
        if os.listdir(copy_directory):
            progress("Creating ISO {}...".format(isoname))
            make_iso(copy_directory, isoname)

            progress("Inserting CD...")
            qemu.run_monitor("change {} \"{}\"".format(cdrom, isoname))
            qemu.run_console("mkdir -p {}".format(pipes.quote(copy_directory)))
            # Make sure cdrom didn't automount
            # Make sure guest path mirrors host path
            qemu.run_console("while ! mount /dev/cdrom {}; ".format(pipes.quote(copy_directory)) +
                        "do sleep 0.3; umount /dev/cdrom; done")

        # if there is a setup.sh script in the replay/proc_name/cdrom/ folder
        # then run that setup.sh script first (good for scriptst that need to
        # prep guest environment before script runs
        qemu.run_console("{}/setup.sh &> /dev/null || true".format(pipes.quote(copy_directory)))
        # Important that we type command into console before recording starts and only
        # hit enter once we've started the recording.
        progress("Running command inside guest.")
        if stdin:
            # only support for "[binary] [STDIN_file]"
            assert (len(command) == 2)
            command.insert(1, "<")
        qemu.type_console(sp.list2cmdline(env_to_list(env) + command))

        # start PANDA recording
        qemu.run_monitor("begin_record \"{}\"".format(recording_path))
        qemu.run_console(timeout=1200)

        # end PANDA recording
        progress("Ending recording...")
        qemu.run_monitor("end_record")

def qemu_binary(arch_data):
    return join(panda_build_dir, arch_data.dir, arch_data.binary)

def transform_arg_copy(orig_filename):
    if orig_filename.startswith('guest:'):
        return orig_filename[6:]
    elif os.path.isfile(orig_filename):
        name = basename(orig_filename)
        copy_filename = join(install_dir, name)
        if copy_filename != orig_filename:
            shutil.copy(orig_filename, copy_filename)
        filemap[orig_filename] = copy_filename
        return copy_filename
    else:
        return orig_filename

def EXIT_USAGE():
    print(USAGE)
    sys.exit(1)

def run_and_create_recording():
    global install_dir
    
    parser = argparse.ArgumentParser(usage=USAGE)

    parser.add_argument("--perf", action='store_true')
    parser.add_argument("--rr", action='store_true')
    parser.add_argument("--cmd", action='store')
    parser.add_argument("--env", action='store')
    parser.add_argument("--qemu_args", action='store', default="")
    parser.add_argument("--qcow", action='store', default="")
    parser.add_argument("--snapshot", "-s", action='store', default="root")
    parser.add_argument("--arch", action='store', default='i386', choices=SUPPORTED_ARCHES.keys())
    parser.add_argument("--fileinput", action='store')
    parser.add_argument("--stdin", action='store_true')
    parser.add_argument("--replaybase", action='store')

    args, guest_cmd = parser.parse_known_args()
    if args.cmd:
        guest_cmd = shlex.split(args.cmd)

    if len(sys.argv) < 2:
        EXIT_USAGE()

    arch_data = SUPPORTED_ARCHES[args.arch]

    env = {}
    if args.env:
        try:
            env = eval(args.env)
        except:
            print("Something went wrong parsing the environment string: [{}]".format(env))
            EXIT_USAGE()

    binary = guest_cmd[0]

    if binary.startswith('guest:'): binary = binary[6:]
    binary_basename = basename(binary)

    # Directory structure:
    # + replays
    # +---+ binary1
    #     +---- cdrom
    #     +---- cdrom.iso
    binary_dir = join(os.getcwd(), 'replays', binary_basename)
    if not os.path.exists(binary_dir):
        os.makedirs(binary_dir)

    install_dir = join(binary_dir, 'cdrom')
    # if os.path.exists(install_dir):
        # shutil.rmtree(install_dir)
    if not os.path.exists(install_dir):
        os.mkdir(install_dir)

    if args.qcow:
        qcow = args.qcow
    else:
        qcow = join(dot_dir, arch_data.qcow)

    if not os.path.isfile(qcow):
        print ("\nQcow %s doesn't exist. Downloading from moyix. Thanks moyix!\n" % qcow)
        sp.check_call(["wget", "http://panda.moyix.net/~moyix/" + arch_data.qcow, "-O", qcow])
        for extra_file in arch_data.extra_files or []:
            extra_file_path = join(dot_dir, extra_file)
            sp.check_call(["wget", "http://panda.moyix.net/~moyix/" + extra_file, "-O", extra_file_path])

    # Expand out the dot dir in extra_args if necessary
    if arch_data.extra_args:
        extra_args = arch_data.extra_args.format(**{'DOT_DIR': dot_dir})
        # And split it
        extra_args = shlex.split(extra_args)
    else:
        extra_args = []

    new_guest_cmd = list(map(transform_arg_copy, guest_cmd))
    # exename = basename(new_guest_cmd[0])

    print ("args = ", guest_cmd)
    print ("new_guest_cmd = ", new_guest_cmd)
    print ("env = ", env)

    if args.replaybase is None:
        replay_base = join(binary_dir, binary_basename)
    else:
        replay_base = args.replaybase

    print(qemu_binary(arch_data))

    create_recording(
        qemu_binary(arch_data),
        qcow, args.snapshot, new_guest_cmd,
        install_dir,
        replay_base,
        arch_data.prompt,
        arch_data.cdrom,
        rr=args.rr,
        perf=args.perf,
        env=env,
        extra_args=extra_args + shlex.split(args.qemu_args),
        stdin=args.stdin
    )
    return (replay_base, arch_data, args.stdin, args.fileinput, guest_cmd)

if __name__ == "__main__":
    run_and_create_recording()
    
