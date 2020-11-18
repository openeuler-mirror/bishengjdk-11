### Tips for the compilation of dependencies

All X11 libraries can be compiled with shell scripts from https://www.x.org/wiki/Building_the_X_Window_System. But if you're stuck in it(blocked by the firewall or slow proxy), then you can refer to the following stupid tips: download all the source code and compile them one by one.

#### Prepare

```shell
$ export PATH=/path/to/riscv/toolchain/bin:$PATH
$ export sysroot=/path/to/riscv/toolchain/sysroot
$ export prefix=$sysroot/usr
```

Note that the compilation of some of the following libraries may depend on others, so it's a wise choice to keep a recommended sequence:
xproto/xtrans/xextproto/renderproto/libpthread-stubs/xcb-proto/fixesproto/kbproto/recordproto/inputproto/xorgproto
libxml2/libffi/libuuid/zlib/alsa-lib/libpng/freetype2/libICE/libSM/libXau/libXcb/libX11/libXt/libXfixes/libXrender
libXext/libXi/libXtst/libXrandr/cups/fontconfig

#### xproto/xextproto/renderproto/libpthread-stubs/xcb-proto/fixesproto/kbproto/inputproto/xorgproto/libffi/libuuid/libpng

```shell
$ cd /path/to/source
$ ./configure --host=riscv64-unknown-linux-gnu --prefix=$prefix
$ make && make install
```

#### xtrans/recordproto/libICE/libSM/libXau/libX11/libXt/libXfixes/libXrender/libXext/libXi/libXtst/libXrandr

```shell
$ cd /path/to/source
$ ./configure --host=riscv64-unknown-linux-gnu --disable-malloc0returnsnull --prefix=$prefix
$ make && make install
```

#### libxml2

```shell
$ ./configure --host=riscv64-unknown-linux-gnu --prefix=$prefix --with-python=no
$ make && make install
```

#### zlib

```shell
$ CHOST=riscv64 CC=riscv64-unknown-linux-gnu-gcc AR=riscv64-unknown-linux-gnu-ar RANLIB=riscv64-unknown-linux-gnu-ranlib ./configure  --prefix=$prefix
$ make && make install
```

#### alsa-lib

```shell
$ ./configure --host=riscv64-unknown-linux-gnu --disable-python --prefix=$prefix
$ make && make install
```

#### freetype2

```shell
$ ./configure --host=riscv64-unknown-linux-gnu --prefix=$prefix --with-brotli=no --with-harfbuzz=no --with-bzip2=no PKG_CONFIG_PATH=$prefix/lib/pkgconfig
$ make && make install
```

#### libXcb

```shell
$ ./configure --host=riscv64-unknown-linux-gnu --disable-malloc0returnsnull --prefix=$prefix PKG_CONFIG_PATH=$prefix/lib/pkgconfig
$ make && make install
```

#### cups

```shell
$ ./configure --host=riscv64-unknown-linux-gnu --disable-ssl --disable-gssapi --disable-avahi --disable-libsub --disable-dbus --disable-systemd
$ make && make install DSTROOT=$sysroot
```

#### fontconfig

```shell
$ ./configure --host=riscv64-unknown-linux-gnu --prefix=$prefix --enable-libxml2 LIBXML2_LIBS="-L$prefix/lib -lxml2" \
LIBXML2_CFLAGS="-I$prefix/include/libxml2" CPPFLAGS="-I$prefix/include" LDFLAGS="-L$prefix/lib"
FREETYPE_CFLAGS="-I$prefix/include/freetype2" FREETYPE_LIBS="-L$prefix/lib -lfreetype"
$ make && make install
```
