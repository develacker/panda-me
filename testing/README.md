# *crapr* testing framework

## Testing

### PANDA record
```bash
$ cd tests
$ python runner.py guest:cat guest:/etc/passwd
```

Then `replays` directory is created and the log and snapshot files are saved in the path as `<name>` (in this case, the <name> is `cat`).

### PANDA replay
```bash
$ cd tests
$ ../build/i386-softmmu/qemu-system-i386 -replay ./replays/cat
```

The recording can be replayed by executing qemu commands with `-replay` option and the argument should be given as `/path/to/replays/<name>` format.

### Crashing example
```bash
$ python runner.py ./samples/hof ./samples/exploit --stdin
...
[run_guest.py] Running command inside guest.

console cmd: [/home/add/git/crapr/tests/replays/hof/cdrom/hof < /home/add/git/crapr/tests/replays/hof/cdrom/exploit]
monitor cmd: [begin_record "/home/add/git/crapr/tests/replays/hof/hof"]
(qemu)
None
writing snapshot:	/home/add/git/crapr/tests/replays/hof/hof-rr-snp
 begin_record "/home/add/git/crapr/tests/replays/hof/hof"
(qemu) begin_record "/home/add/git/crapr/tests/replays/hof/hof"
(qemu)

root@debian-i386:~#
None
opening nondet log for write :	/home/add/git/crapr/tests/replays/hof/hof-rr-nondet.log
git/crapr/tests/replays/hof/cdrom/exploitrom/hof < /home/add/ 
Input b: Input a: Segmentation fault
git/crapr/tests/replays/hof/cdrom/exploitests/replays/hof/cdrom/hof < /home/add/ 
Input b: Input a: Segmentation fault
root@debian-i386:~#
...
```