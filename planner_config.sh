#!/bin/bash
./configure \
	 --prefix="/home/gpadmin/local/gpdb" \
	  --with-perl --with-python --with-libxml --without-zstd \
	   --enable-debug --enable-cassert
sed -i 's/-O3/-O0/g' src/Makefile.global
