os-project by Dominic Holschen
See the repo on github: https://github.com/domHolschen/os-project

To Compile:
Use the `make` command to invoke the Makefile and create the executables `user` and `oss`.

user:
Tool that intakes 1 argument. That argument is the amount of iterations that the process will run through.
The argument is an integer and has a minumum value of 1. Invalid inputs will be defaulted to 1.

oss:
Tool with 4 different options. It invokes `user` processes.
Usage: oss [-h] [-n proc] [-s simul] [-t iter]
-h : Print options for the oss tool and exits
-n : Total number of processes to run (default: 1)
-s : Maximum number of simultaneously running processes (disabled by default)
-t : Amount of iterations each child process will run through before terminating (default: 1) This is the argument that `user` will be passed.

Known issues:
Once oss spins up the maximum amount of child processes, it will wait until ALL children are exited before forking new ones.
This currently should not have a functional effect as each call to `user` has the same argument passed in for one particular call of `oss`.
So the children processes will finish at (roughly, if you're counting by the millisecond) the same time.