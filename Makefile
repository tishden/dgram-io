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
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude
LDLIBS   ?= -lrt -lpthread
CLANG    ?= clang
BIN      := bin

HAVE_URING := $(shell pkg-config --exists liburing 2>/dev/null && echo 1)
ifeq ($(HAVE_URING),1)
URING_CXXFLAGS := -DHAVE_URING $(shell pkg-config --cflags liburing)
URING_LDLIBS   := $(shell pkg-config --libs liburing)
endif

HAVE_XDP := $(shell pkg-config --exists libxdp 2>/dev/null && echo 1)
ifeq ($(HAVE_XDP),1)
XDP_CXXFLAGS := -DHAVE_XDP $(shell pkg-config --cflags libxdp)
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
LWIP_OBJS := $(patsubst $(LWIP_DIR)/src/%.c,$(BIN)/lwip/%.o,$(LWIP_SRCS)) \
             $(BIN)/lwip_port.o $(BIN)/lwip_tcp.o
LWIP_CPPFLAGS := -DHAVE_LWIP -I$(LWIP_DIR)/src/include -Isrc/lwip
# lwIP is C89-flavoured C with its own opinions about unused parameters and
# sign conversion. It is a vendored dependency, not our code: -w on purpose.
LWIP_CFLAGS := -std=gnu99 -O2 -w $(LWIP_CPPFLAGS)
endif

HDRS := $(wildcard include/dgram_io/*.h)
# What was detected, as a file whose timestamp moves only when the detection
# does. Every object depends on it: without this, fetching lwIP (or installing
# liburing) after a first build leaves the old objects in place, `make config`
# says yes, and the backend still answers "built without".
FLAGS := $(BIN)/.detected
DEPS  := $(HDRS) $(FLAGS)
OBJS := $(BIN)/factory.o $(BIN)/udp_backend.o $(BIN)/uring_backend.o \
        $(BIN)/xdp_backend.o \
        $(BIN)/dpdk_backend.o $(BIN)/tcp_backend.o $(BIN)/tcp_dpdk_backend.o \
        $(BIN)/tcp_xdp_backend.o $(LWIP_OBJS)

.PHONY: all lib test example clean config FORCE

all: lib $(if $(HAVE_XDP),$(BIN)/xdp_filter.bpf.o $(BIN)/xdp_tcp_filter.bpf.o)

config:
	@echo "uring: $(if $(HAVE_URING),yes,no  (pkg-config liburing))"
	@echo "xdp:   $(if $(HAVE_XDP),yes,no  (pkg-config libxdp))"
	@echo "dpdk:  $(if $(HAVE_DPDK),yes,no  (pkg-config libdpdk))"
	@echo "lwip:  $(if $(HAVE_LWIP),yes,no  (run scripts/get_lwip.sh))"

lib: $(BIN)/libdgram_io.a

$(BIN)/libdgram_io.a: $(OBJS)
	ar rcs $@ $^

$(BIN):
	mkdir -p $(BIN)

$(FLAGS): FORCE | $(BIN)
	@echo 'uring=$(HAVE_URING) xdp=$(HAVE_XDP) dpdk=$(HAVE_DPDK) lwip=$(HAVE_LWIP)' | \
	  cmp -s - $@ 2>/dev/null || \
	  echo 'uring=$(HAVE_URING) xdp=$(HAVE_XDP) dpdk=$(HAVE_DPDK) lwip=$(HAVE_LWIP)' > $@

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

# Vendored lwIP, compiled once into bin/lwip/ mirroring the source layout.
$(BIN)/lwip/%.o: $(LWIP_DIR)/src/%.c src/lwip/lwipopts.h $(FLAGS) | $(BIN)
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BIN)/lwip_port.o: src/lwip/lwip_port.c src/lwip/lwipopts.h $(FLAGS) | $(BIN)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BIN)/lwip_tcp.o: src/lwip_tcp.cpp $(DEPS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(LWIP_CPPFLAGS) -c $< -o $@

# XDP filter programs, loaded at runtime from next to the binary.
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

example: $(BIN)/echo $(BIN)/loadgen
	@echo "run two of these: ./$(BIN)/echo --io udp --role server / --role client"

$(BIN)/echo $(BIN)/loadgen: $(BIN)/%: examples/%.cpp $(BIN)/libdgram_io.a $(HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(XDP_CXXFLAGS) $(DPDK_CXXFLAGS) $(LWIP_CPPFLAGS) \
	  $< $(BIN)/libdgram_io.a -o $@ $(LDLIBS) $(URING_LDLIBS) $(XDP_LDLIBS) \
	  $(DPDK_LDLIBS)

clean:
	rm -rf $(BIN)
