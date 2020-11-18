# Building JDK for RISC-V platforms

## Requirements

BishengJDK RISC-V has been tested only on QEMU-RISCV64 at linux x86 platforms (specifically, on Ubuntu 18.04).

## Toolchains

Get RISC-V Toolchain source:

```shell
$ git clone --recursive https://github.com/riscv/riscv-gnu-toolchain
```

or

```shell
$ git clone https://github.com/riscv/riscv-gnu-toolchain
$ cd riscv-gnu-toolchain
$ git submodule update --init --recursive
```

Note that no extra work is required when building with gcc 9.2.0, and there may be some compilation problems with other compilers.

```shell
$ cd ricsv-gcc
$ git checkout riscv-gcc-9.2.0
```

### Prerequisites

```
$ sudo apt-get install autoconf automake autotools-dev curl python3 libmpc-dev libmpfr-dev libgmp-dev gawk build-essential bison flex texinfo gpref libtool patchutils bc zlib1g-dev libexpat-dev
```

### Build Toolchain (Linux, 64 bit)

```
$ ./configure --prefix=/opt/riscv
$ make linux
```

## External Libraries

To build the whole JDK riscv port, extra libraries of riscv version are required:

- ALSA
- CUPS
- LIBPNG
- Freetype2
- Fontconfig
- zlib
- libuuid
- libxml2
- libpthread-stubs
- libffi
- X11
  - xproto
  - xtrans
  - xextproto
  - renderproto
  - xcb-proto
  - fixesproto
  - kbproto
  - recordproto
  - inputproto
  - xorgproto
  - libICE
  - libSM
  - libXau
  - libXcb
  - libX11
  - libXt
  - libXfixes
  - libXrender
  - libXext
  - libXi
  - libXtst
  - libXrandr

For the infomation of the cross-compilation of dependencies above, please refer to [the compilation tips](./DEPENDENCY_BUILD.md).

## QEMU

We can build QEMU from the source code, and it's highly recommended to use the lasted version:

```
# keep source tree clean, and build in a new directory
$ mkdir build && cd build
# set riscv64 target
$ bash ./configure --prefix=$PWD/riscv-qemu --disable-git-update --target-list=riscv64-linux-user,riscv64-softmmu
$ make && make install
```

## Getting the source code

Getting the source code of BishengJDK riscv:

```shell
$ git clone -b risc-v https://gitee.com/openeuler/bishengjdk-11.git
```

## Build JDK

### Prerequisites

Serveral standard packages are needed to build JDK.

On Ubuntu, executing the following command:

```shell
$ sudo apt-get install pkg-config build-essential zip unzip screen make autoconf libxext-dev libxrender-dev libxtst-dev libxt-dev libcups2-dev libfreetype6-dev mercurial libasound2-dev cmake automake
```

### Boot JDK Requirements

Paradoxically, buidling the JDK requires a pre-existing JDK. This is called the "boot JDK". The boot JDK does not, however, have to be a JDK built directly from the source code available in the OpenJDK community. Make sure that there already exists another JDK for that platform that is usable as boot JDK.

The rule of thumb is that the boot JDK for building JDK major version N should be a JDK of major version N-1, so for building JDK 11, we need JDK 11 or JDK 10. However, the JDK should be able to "build itself", so an up-to-date build of the current JDK source is an acceptable alternative.

### Build JDK

Refer to the following configuration:

```shell
# add riscv toolchain to path
$ export PATH=/path/to/riscv/toolchain/bin:$PATH
# configure && build
$ bash configure \
    --openjdk-target=riscv64-unknown-linux-gnu \
    --disable-warnings-as-errors \
    --with-sysroot=/riscv/toolchain/sysroot
    --x-includes=/riscv/toolchain/sysroot/usr/include \
    --x-libraries=/riscv/toolchain/sysroot/usr/lib
    --with-boot-jdk=/path/to/boot/jdk
$ make images
```
Before running the JDK, `LD_LIBRARY_PATH` may need to be set:

```shell
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/path/to/riscv/toolchain/sysroot/lib:/path/to/riscv/toolchain/sysroot/usr/lib
```

Run java in QEMU user mode:

```
$ /path/to/qemu/bin/qemu-riscv64 /path/to/jdk/riscv64/bin/java -version
```

## Utils

### hsdis

#### Prerequisites

Binutils is required: [Download Binutils](https://ftp.gnu.org/gnu/binutils)

#### Build binutils & hsdis

Note:
`bfd_octets_per_byte()` has 2 args since binutils 2.34, if you build hsdis with binutils >= 2.34, `hsdis.c` may need be modified like that:

```diff
-  dinfo->octets_per_byte = bfd_octets_per_byte (abfd);
+  dinfo->octets_per_byte = bfd_octets_per_byte (abfd, NULL);
```

Build binutils from the source code:

```shell
$ cd build && make linux-riscv64 && cd linux-riscv64
$ export PATH=/path/to/riscv/toolchain/bin:$PATH
$ CFLAGS = '-fPIC -O' ../binutils-version/configure --disable-nls --host=riscv64-unknown-linux-gnu
$ make
```

Build hsdis-riscv64:

```shell
# PWD is src/utils/hsdis
$ make demo BINUTILSDIR=build/binutils-2.35.1 ARCH=riscv64 CC=riscv-unknown-linxu-gnu-gcc
```

Test 'hsdis-riscv64' by 'hisdis-demo' on QEMU user mode:

```shell
# set LD_LIBRARY_PATH and execute hsdis-demo
$ /path/to/riscv/qemu/bin/qemu-riscv64 build/linux-riscv64/hsdis-demo
```

The outputs would like:

```
Hello World!
...And now for something completely different:

Decoding from 0x108de to 0x10a5a...with decode_instructions_virtual
Decoding for CPU 'riscv:rv64'
main:
 0x108de        addi    sp,sp,-64
 0x108e0        sd      ra,56(sp)
 0x108e2        sd      s0,48(sp)
 0x108e4        addi    s0,sp,64
 0x108e6        mv      a5,a0
 0x108e8        sd      a1,-64(s0)
 0x108ec        sw      a5,-52(s0)
 0x108f0        sw      zero,-20(s0)
 0x108f4        li      a5,1
 0x108f6        sw      a5,-24(s0)
 0x108fa        j       0x00000000000109ba
 0x108fc        lw      a5,-24(s0)
 0x10900        slli    a5,a5,0x3
 ```
