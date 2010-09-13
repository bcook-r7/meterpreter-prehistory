# PKS, I suck at Makefile's.  Given that this compiles POSIX meterpreter and
# associated stuff (openssl, libpcap, etc) this is going to get very messy,
# very quickly.

all: external/source/meterpreter/source/bionic/compiled/libc.so external/source/meterpreter/source/bionic/compiled/libm.so external/source/meterpreter/source/bionic/compiled/libdl.so external/source/meterpreter/source/bionic/compiled/libcrypto.so external/source/meterpreter/source/bionic/compiled/libssl.so external/source/meterpreter/source/bionic/compiled/libsupport.so external/source/meterpreter/source/bionic/compiled/libmetsrv_main.so external/source/meterpreter/source/bionic/compiled/libpcap.so data/meterpreter/msflinker_linux_x86.bin data/meterpreter/ext_server_stdapi.lso

external/source/meterpreter/source/bionic/compiled/libc.so: external/source/meterpreter/source/bionic/compiled
	(cd external/source/meterpreter/source/bionic/libc && ARCH=x86 TOP=${PWD} jam && cd out/x86/ && sh make.sh && [ -f libbionic.so ] )
	cp external/source/meterpreter/source/bionic/libc/out/x86/libbionic.so external/source/meterpreter/source/bionic/compiled/libc.so

external/source/meterpreter/source/bionic/compiled:
	mkdir external/source/meterpreter/source/bionic/compiled/

external/source/meterpreter/source/bionic/compiled/libm.so:
	(cd external/source/meterpreter/source/bionic/libm && make -f msfMakefile && [ -f libm.so ])
	cp external/source/meterpreter/source/bionic/libm/libm.so external/source/meterpreter/source/bionic/compiled/libm.so

external/source/meterpreter/source/bionic/compiled/libdl.so:
	(cd external/source/meterpreter/source/bionic/libdl && make && [ -f libdl.so ])
	cp external/source/meterpreter/source/bionic/libdl/libdl.so external/source/meterpreter/source/bionic/compiled/libdl.so

external/source/meterpreter/source/bionic/compiled/libcrypto.so: tmp/openssl-0.9.8o/libssl.so
	cp tmp/openssl-0.9.8o/libcrypto.so external/source/meterpreter/source/bionic/compiled/libcrypto.so 

external/source/meterpreter/source/bionic/compiled/libssl.so: tmp/openssl-0.9.8o/libssl.so
	cp tmp/openssl-0.9.8o/libssl.so external/source/meterpreter/source/bionic/compiled/libssl.so

tmp/openssl-0.9.8o/libssl.so:
	[ -d tmp ] || mkdir tmp 
	[ -f tmp/openssl-0.9.8o.tar.gz ] || wget -O tmp/openssl-0.9.8o.tar.gz http://openssl.org/source/openssl-0.9.8o.tar.gz
	tar -C tmp/ -xzf tmp/openssl-0.9.8o.tar.gz
	(cd tmp/openssl-0.9.8o && ./Configure threads no-zlib no-krb5 386 --prefix=/tmp/out linux-elf shared)
	(cd tmp/openssl-0.9.8o && make CC="gcc -I${PWD}/external/source/meterpreter/source/bionic/libc/include -I${PWD}/external/source/meterpreter/source/bionic/libc/kernel/common/linux/ -I${PWD}/external/source/meterpreter/source/bionic/libc/kernel/common/ -I${PWD}/external/source/meterpreter/source/bionic/libc/arch-x86/include/ -I${PWD}/external/source/meterpreter/source/bionic/libc/kernel/arch-x86/  -I${PWD}/external/source/meterpreter/source/bionic/libc/private -fPIC -DPIC -nostdinc -nostdlib -Dwchar_t='char' -fno-builtin -D_SIZE_T_DECLARED -DElf_Size='u_int32_t' -I${PWD}/external/source/meterpreter/source/bionic/libm/include  -L${PWD}/external/source/meterpreter/source/bionic/compiled  -D_BYTE_ORDER=_LITTLE_ENDIAN -lc" depend  clean all ; [ -f libssl.so.0.9.8 -a -f libcrypto.so.0.9.8 ] )

external/source/meterpreter/source/bionic/compiled/libpcap.so: tmp/libpcap-1.1.1/libpcap.so.1.1.1
	cp tmp/libpcap-1.1.1/libpcap.so.1.1.1 external/source/meterpreter/source/bionic/compiled/libpcap.so

tmp/libpcap-1.1.1/libpcap.so.1.1.1:
	[ -d tmp ] || mkdir tmp
	[ -f tmp/libpcap-1.1.1.tar.gz ] || wget -O tmp/libpcap-1.1.1.tar.gz http://www.tcpdump.org/release/libpcap-1.1.1.tar.gz
	tar -C tmp -xzf tmp/libpcap-1.1.1.tar.gz
	(cd tmp/libpcap-1.1.1 && ./configure --disable-bluetooth --without-bluetooth --without-usb --disable-usb --without-can --disable-can --without-usb-linux --disable-usb-linux)
	echo '#undef HAVE_DECL_ETHER_HOSTTON' >> tmp/libpcap-1.1.1/config.h
	echo '#undef HAVE_SYS_BITYPES_H' >> tmp/libpcap-1.1.1/config.h
	echo '#undef PCAP_SUPPORT_CAN' >> tmp/libpcap-1.1.1/config.h
	echo '#undef PCAP_SUPPORT_USB' >> tmp/libpcap-1.1.1/config.h
	echo '#undef HAVE_ETHER_HOSTTON'  >> tmp/libpcap-1.1.1/config.h
	echo '#define _STDLIB_H this_works_around_malloc_definition_in_grammar_dot_c' >> tmp/libpcap-1.1.1/config.h
	(cd tmp/libpcap-1.1.1 && patch --dry-run -p0 < ../../external/source/meterpreter/source/libpcap/pcap_nametoaddr_fix.diff && patch -p0 < ../../external/source/meterpreter/source/libpcap/pcap_nametoaddr_fix.diff)
	sed -i -e s/pcap-usb-linux.c//g -e s/fad-getad.c/fad-gifc.c/g tmp/libpcap-1.1.1/Makefile
	sed -i -e s,"CC = gcc","CC = gcc -fno-stack-protector -nostdinc -nostdlib -fPIC -DPIC -g -Wall -D_UNIX -D__linux__  -I${PWD}/external/source/meterpreter/source/bionic/libc/include -I${PWD}/external/source/meterpreter/source/bionic/libc/kernel/common/linux/ -I${PWD}/external/source/meterpreter/source/bionic/libc/kernel/common/ -I${PWD}/external/source/meterpreter/source/bionic/libc/arch-x86/include/ -I${PWD}/external/source/meterpreter/source/bionic/libc/kernel/arch-x86/ -Dwchar_t="char" -fno-builtin -D_SIZE_T_DECLARED -DElf_Size="u_int32_t" -D_BYTE_ORDER=_LITTLE_ENDIAN -lgcc -L${PWD}/external/source/meterpreter/source/bionic/compiled -gstabs+ -fPIC -Os -lc",g tmp/libpcap-1.1.1/Makefile
	(cd tmp/libpcap-1.1.1 && make)


data/meterpreter/msflinker_linux_x86.bin: external/source/meterpreter/source/server/rtld/msflinker.bin
	cp external/source/meterpreter/source/server/rtld/msflinker.bin data/meterpreter/msflinker_linux_x86.bin

external/source/meterpreter/source/server/rtld/msflinker.bin:
	(cd external/source/meterpreter/source/server/rtld ; make)

external/source/meterpreter/source/bionic/compiled/libmetsrv_main.so:
	(cd external/source/meterpreter/workspace/metsrv && make)
	cp external/source/meterpreter/workspace/metsrv/libmetsrv_main.so external/source/meterpreter/source/bionic/compiled/libmetsrv_main.so 

external/source/meterpreter/source/bionic/compiled/libsupport.so:
	(cd external/source/meterpreter/workspace/common && make) 
	cp external/source/meterpreter/workspace/common/libsupport.so external/source/meterpreter/source/bionic/compiled/libsupport.so

data/meterpreter/ext_server_stdapi.lso:
	(cd external/source/meterpreter/workspace/ext_server_stdapi && make)
	cp external/source/meterpreter/workspace/ext_server_stdapi/ext_server_stdapi.so data/meterpreter/ext_server_stdapi.lso
