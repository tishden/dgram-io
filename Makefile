# SPDX-License-Identifier: Apache-2.0
#
# Every backend is optional. The build detects what the machine has and
# compiles those in; the rest still link, and fail at runtime with a message
# naming the missing dependency rather than failing to build.
#
#   udp, tcp        always (kernel sockets)
#   uring           needs liburing         (pkg-config liburing)
#   xdp, tcp-xdp    need libxdp + libbpf   (pkg-config libxdp)
#   dpdk, tcp-dpdk  need DPDK              (pkg-config libdpdk)
#   tcp-*           additionally need lwIP (scripts/get_lwip.sh)
#
CXX      ?= g++
CC       ?= gcc
CLANG    ?= clang
OBJCOPY  ?= objcopy
# Yours to override (make CXXFLAGS=-O3 ...). What the build cannot do without
# is appended below regardless.
CXXFLAGS ?= -O2 -Wall -Wextra
CFLAGS   ?= -O2
LDLIBS   ?= -lrt -lpthread
# -fPIC: the static library can then be linked into a shared object too.
override CXXFLAGS += -std=c++17 -fPIC -Iinclude
BIN      := bin

# Install layout. Defined up here because the XDP backends are compiled with
# BPFDIR baked in: it is where they look for their filter when it is not next
# to the running binary.
PREFIX  ?= /usr/local
LIBDIR  ?= $(PREFIX)/lib
INCDIR  ?= $(PREFIX)/include
BPFDIR  ?= $(LIBDIR)/dgram_io
PCDIR   ?= $(LIBDIR)/pkgconfig
VERSION := 0.1.0

HAVE_URING := $(shell pkg-config --exists liburing 2>/dev/null && echo 1)
ifeq ($(HAVE_URING),1)
URING_CXXFLAGS := -DHAVE_URING $(shell pkg-config --cflags liburing)
URING_LDLIBS   := $(shell pkg-config --libs liburing)
endif

HAVE_XDP := $(shell pkg-config --exists libxdp 2>/dev/null && echo 1)
ifeq ($(HAVE_XDP),1)
XDP_CXXFLAGS := -DHAVE_XDP -DDGRAM_IO_BPFDIR='"$(BPFDIR)"' $(shell pkg-config --cflags libxdp)
XDP_LDLIBS   := $(shell pkg-config --libs libxdp) -lbpf
endif

HAVE_DPDK := $(shell pkg-config --exists libdpdk 2>/dev/null && echo 1)
ifeq ($(HAVE_DPDK),1)
DPDK_CXXFLAGS := -DHAVE_DPDK $(shell pkg-config --cflags libdpdk)
DPDK_LDLIBS   := $(shell pkg-config --libs libdpdk)
endif

# lwIP is not a distro package; scripts/get_lwip.sh clones a pinned release.
LWIP_DIR  ?= third_party/lwip
HAVE_LWIP := $(shell test -f $(LWIP_DIR)/src/core/tcp.c && echo 1)
ifeq ($(HAVE_LWIP),1)
LWIP_SRCS := $(wildcard $(LWIP_DIR)/src/core/*.c) \
             $(wildcard $(LWIP_DIR)/src/core/ipv4/*.c) \
             $(LWIP_DIR)/src/netif/ethernet.c
LWIP_PARTS := $(patsubst $(LWIP_DIR)/src/%.c,$(BIN)/lwip/%.o,$(LWIP_SRCS)) \
              $(BIN)/lwip_port.o $(BIN)/lwip_tcp.o
# What goes into the library: the same objects with lwIP's symbols renamed
# (see the rule for $(LWIP_STAMP) below).
LWIP_STAMP := $(BIN)/lwip_r/.stamp
LWIP_OBJS := $(addprefix $(BIN)/lwip_r/,$(notdir $(LWIP_PARTS)))
LWIP_CPPFLAGS := -DHAVE_LWIP -I$(LWIP_DIR)/src/include -Isrc/lwip
# lwIP is C89-flavoured C with its own opinions about unused parameters and
# sign conversion. It is a fetched dependency, not our code: -w on purpose.
LWIP_CFLAGS := $(CFLAGS) -std=gnu99 -fPIC -w $(LWIP_CPPFLAGS)
endif

HDRS := $(wildcard include/dgram_io/*.h)
# What was detected, as a file whose timestamp moves only when the detection
# does. Every object depends on it: without this, fetching lwIP (or installing
# liburing) after a first build leaves the old objects in place, `make config`
# says yes, and the backend still answers "built without".
FLAGS := $(BIN)/.detected
DEPS  := $(HDRS) $(wildcard src/*.h) $(FLAGS)
OBJS := $(BIN)/factory.o $(BIN)/udp_backend.o $(BIN)/uring_backend.o \
        $(BIN)/xdp_backend.o \
        $(BIN)/dpdk_backend.o $(BIN)/tcp_backend.o $(BIN)/tcp_dpdk_backend.o \
        $(BIN)/tcp_xdp_backend.o $(LWIP_OBJS)

.PHONY: all lib test example clean config install uninstall FORCE

all: lib $(if $(HAVE_XDP),$(BIN)/xdp_filter.bpf.o $(BIN)/xdp_tcp_filter.bpf.o)

config:
	@echo "uring: $(if $(HAVE_URING),yes,no  (pkg-config liburing))"
	@echo "xdp:   $(if $(HAVE_XDP),yes,no  (pkg-config libxdp))"
	@echo "dpdk:  $(if $(HAVE_DPDK),yes,no  (pkg-config libdpdk))"
	@echo "lwip:  $(if $(HAVE_LWIP),yes,no  (run scripts/get_lwip.sh))"

lib: $(BIN)/libdgram_io.a

# Rebuilt from scratch: `ar r` only ever adds, so an object that left OBJS
# would otherwise stay in the archive.
$(BIN)/libdgram_io.a: $(OBJS)
	@rm -f $@
	ar rcs $@ $^

$(BIN):
	mkdir -p $(BIN)

# BPFDIR is in the stamp too: the XDP objects have it compiled in.
DETECTED := uring=$(HAVE_URING) xdp=$(HAVE_XDP) dpdk=$(HAVE_DPDK) lwip=$(HAVE_LWIP) bpfdir=$(BPFDIR)
$(FLAGS): FORCE | $(BIN)
	@echo '$(DETECTED)' | cmp -s - $@ 2>/dev/null || echo '$(DETECTED)' > $@

$(BIN)/factory.o: src/factory.cpp $(DEPS) | $(BIN)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BIN)/udp_backend.o: src/udp_backend.cpp src/udp_socket.h $(DEPS) | $(BIN)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BIN)/uring_backend.o: src/uring_backend.cpp src/udp_socket.h $(DEPS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(URING_CXXFLAGS) -c $< -o $@
$(BIN)/tcp_backend.o: src/tcp_backend.cpp $(DEPS) | $(BIN)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BIN)/xdp_backend.o: src/xdp_backend.cpp $(DEPS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(XDP_CXXFLAGS) -c $< -o $@
$(BIN)/dpdk_backend.o: src/dpdk_backend.cpp $(DEPS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(DPDK_CXXFLAGS) -c $< -o $@
$(BIN)/tcp_xdp_backend.o: src/tcp_xdp_backend.cpp $(DEPS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(XDP_CXXFLAGS) $(LWIP_CPPFLAGS) -c $< -o $@
$(BIN)/tcp_dpdk_backend.o: src/tcp_dpdk_backend.cpp $(DEPS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(DPDK_CXXFLAGS) $(LWIP_CPPFLAGS) -c $< -o $@

# lwIP, compiled once into bin/lwip/ mirroring the source layout.
$(BIN)/lwip/%.o: $(LWIP_DIR)/src/%.c src/lwip/lwipopts.h $(FLAGS) | $(BIN)
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BIN)/lwip_port.o: src/lwip/lwip_port.c src/lwip/lwipopts.h $(FLAGS) | $(BIN)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BIN)/lwip_tcp.o: src/lwip_tcp.cpp src/lwip_tcp.h $(DEPS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LWIP_CPPFLAGS) -c $< -o $@
# Every global symbol lwIP and its C glue define gets a dgram_io_lwip__
# prefix, in their objects and in the C++ glue that calls them. An application
# that links its own lwIP (or has a tcp_write, sys_now, ... of its own) then
# cannot collide with ours.
$(LWIP_STAMP): $(LWIP_PARTS)
	@mkdir -p $(BIN)/lwip_r
	nm -g --defined-only $(filter-out $(BIN)/lwip_tcp.o,$^) | \
	  awk 'NF == 3 && $$3 !~ /^dgram_io_/ {print $$3, "dgram_io_lwip__" $$3}' | \
	  sort -u > $(BIN)/lwip_r/syms.map
	for o in $^; do \
	  $(OBJCOPY) --redefine-syms=$(BIN)/lwip_r/syms.map $$o $(BIN)/lwip_r/$$(basename $$o) || exit 1; \
	done
	@touch $@
$(LWIP_OBJS): $(LWIP_STAMP)

# XDP filter programs, loaded at runtime (Config::bpf_obj, else next to the
# binary, else BPFDIR).
# Debian/Ubuntu keep asm/ under the multiarch dir and -target bpf does not
# search it on its own; empty on el9-style layouts.
BPF_ARCH_INC := $(wildcard /usr/include/$(shell uname -m)-linux-gnu)
$(BIN)/%.bpf.o: src/%.bpf.c | $(BIN)
	$(CLANG) -O2 -g -target bpf $(addprefix -I,$(BPF_ARCH_INC)) -c $< -o $@

# Unit tests: pure logic, no NIC and no root.
test: $(BIN)/test_pktbuild $(BIN)/test_stream $(BIN)/test_rtt
	./$(BIN)/test_pktbuild
	./$(BIN)/test_stream
	./$(BIN)/test_rtt

$(BIN)/test_%: tests/test_%.cpp $(HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@

example: $(BIN)/echo $(BIN)/loadgen $(BIN)/uring_probe
	@echo "run two of these: ./$(BIN)/echo --io udp --role server / --role client"

$(BIN)/uring_probe: examples/uring_probe.cpp $(FLAGS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(URING_CXXFLAGS) $< -o $@ $(URING_LDLIBS)

# The examples see only the public headers, as an installed user would.
$(BIN)/echo $(BIN)/loadgen: $(BIN)/%: examples/%.cpp $(BIN)/libdgram_io.a $(HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) $< $(BIN)/libdgram_io.a -o $@ $(LDLIBS) $(URING_LDLIBS) \
	  $(XDP_LDLIBS) $(DPDK_LDLIBS)

# Installs the static library, the headers, the XDP filter objects and a
# pkg-config file. The library is static, so the .pc lists the libraries of
# every backend this build compiled in. The filters go to BPFDIR, where the
# XDP backends find them on their own; `pkg-config --variable=bpfdir
# dgram-io` prints it.

$(BIN)/dgram-io.pc: $(FLAGS) | $(BIN)
	@printf '%s\n' \
	  'prefix=$(PREFIX)' 'libdir=$(LIBDIR)' 'includedir=$(INCDIR)' 'bpfdir=$(BPFDIR)' '' \
	  'Name: dgram-io' \
	  'Description: one datagram interface over kernel UDP, io_uring, AF_XDP, DPDK and TCP' \
	  'Version: $(VERSION)' \
	  'Cflags: -I$${includedir}' \
	  'Libs: -L$${libdir} -ldgram_io $(strip $(LDLIBS) $(URING_LDLIBS) $(XDP_LDLIBS) $(DPDK_LDLIBS))' > $@

install: all $(BIN)/dgram-io.pc
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCDIR)/dgram_io $(DESTDIR)$(PCDIR)
	install -m 644 $(BIN)/libdgram_io.a $(DESTDIR)$(LIBDIR)/
	install -m 644 $(HDRS) $(DESTDIR)$(INCDIR)/dgram_io/
	install -m 644 $(BIN)/dgram-io.pc $(DESTDIR)$(PCDIR)/
	$(if $(HAVE_XDP),install -d $(DESTDIR)$(BPFDIR) && install -m 644 $(BIN)/*.bpf.o $(DESTDIR)$(BPFDIR)/)

# Removes exactly the files install put there, never a whole directory that
# an overridden INCDIR or BPFDIR might point at.
uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/libdgram_io.a $(DESTDIR)$(PCDIR)/dgram-io.pc
	rm -f $(addprefix $(DESTDIR)$(INCDIR)/dgram_io/,$(notdir $(HDRS)))
	rm -f $(DESTDIR)$(BPFDIR)/xdp_filter.bpf.o $(DESTDIR)$(BPFDIR)/xdp_tcp_filter.bpf.o
	-rmdir $(DESTDIR)$(INCDIR)/dgram_io $(DESTDIR)$(BPFDIR) 2>/dev/null

clean:
	rm -rf $(BIN)
