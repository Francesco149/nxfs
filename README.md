[![Build Status](https://travis-ci.org/Francesco149/nxfs.svg?branch=master)](https://travis-ci.org/Francesco149/nxfs)

read-only fuse filesystem for the
[nx file format](http://nxformat.github.io/), based on
[tinynx](https://github.com/Francesco149/tinynx)

![](https://media.giphy.com/media/l1J9RfALTYXacQSDm/giphy.gif)

![](https://media.giphy.com/media/3ohhwJTUQUFproE59u/giphy.gif)

![](https://media.giphy.com/media/3ohhwlI8GR9Qux9cCk/giphy.gif)

[video demonstration](https://streamable.com/7s4wl)

# usage
you need to have fuse installed. search it in your distro's
package manager, it's usually named ```fuse``` or ```fuse2```.

download binaries from the releases section or build with
```./build``` (you must have libfuse installed, it's usually named
```libfuse-dev``` or ```fuse2``` or ```fuse```).

run ```./nxfs``` with no parameters to see a list of options
available

install the nxfs executable wherever you prefer (ideally somewhere
within your ```PATH``` like ```/usr/bin``` on linux).

```sh
cd

# mount
mkdir nxmnt
nxfs /path/to/file.nx nxmnt
cd nxmnt

# do stuff
ls

# unmount
cd
fusermount -u nxmnt
```
