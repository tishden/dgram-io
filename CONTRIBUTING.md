# Contributing to dgram-io

New backends, portability fixes, and measurements from hardware I do not have
are all welcome.

## Sign-off (DCO), not a CLA

Contributions are accepted under the
[Developer Certificate of Origin](DCO) — the same mechanism the Linux kernel
uses. There is no CLA to sign and nothing to agree to out of band. Add a
sign-off line to each commit:

```
git commit -s
```

which appends:

```
Signed-off-by: Your Name <your.email@example.com>
```

That line certifies you wrote the patch or otherwise have the right to submit
it under Apache-2.0. Use your real name; anonymous contributions cannot be
signed off.

## Adding a backend

The interesting contributions here are new datapaths. The shape is fixed and
short:

1. One translation unit, `src/<name>_backend.cpp`, implementing
   `dgram_io::Backend`.
2. Guard it on a build-time detection in the `Makefile` and provide the
   compiled-out stub — the `#else` branch that returns `nullptr` with an error
   naming the missing dependency. A backend that fails to *link* when its
   library is absent is a regression; the point of the stub is that
   `--io yours` on a plain machine says why.
3. Register it in `src/factory.cpp`, and in `is_stream_backend()` if it is a
   reliable stream.
4. Say in the header comment what it addresses (L2 frames? a connected
   socket?) and what it needs at runtime (root? hugepages? a bound queue?).

The three contract points in the README are the ones a new backend most often
gets wrong: `queue()` must copy, `rx()` views must survive until the next
`rx()`, and an unattainable `max_datagram` must fail at setup rather than
truncate later.

## Testing

```
make test
```

is pure logic — framing, deframing, RTT estimation — and needs no NIC and no
root, which is why it is the gate. Anything touching frame bytes belongs in
`tests/test_pktbuild.cpp`, and anything touching stream reassembly in
`tests/test_stream.cpp`.

The backends themselves cannot be unit-tested meaningfully without hardware.
If you change one, say in the PR what you ran it against — NIC model, driver,
kernel — because "it compiles" is not evidence about a datapath.

## Numbers

Latency claims want the machine, the rate, the message size and the packet
count. The README's table exists in that form for a reason; a PR that moves a
number should be able to replace it in kind.

## Style

Match the file you are editing. Two-space indent, comments in English that
explain why rather than restate the code, and no abbreviation that costs a
reader a lookup.
