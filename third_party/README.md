# third_party/

Not vendored in this repo (prebuilt binaries don't belong in source
control, and a static lib built on one machine's toolchain/libc isn't
safely portable to another's).

## boringssl/

If you build with `-DSFU_USE_BORINGSSL=ON` (see top-level CMakeLists.txt),
point `SFU_BORINGSSL_ROOT` at a built BoringSSL tree:

```
cmake .. -DSFU_USE_BORINGSSL=ON -DSFU_BORINGSSL_ROOT=/path/to/boringssl
```

Expected layout under that root (this is exactly what you get from a
standard BoringSSL checkout + build, no rearranging needed):

```
<root>/include/openssl/*.h
<root>/build/libssl.a
<root>/build/libcrypto.a
```

To produce that from scratch:

```
git clone https://github.com/google/boringssl.git
cd boringssl
mkdir build && cd build
cmake -GNinja -DCMAKE_BUILD_TYPE=Release ..
ninja ssl crypto
```

Requires Go and Ninja in addition to the usual C/C++ toolchain (Go is
used for a handful of code-generation steps at build time, not at
runtime -- the resulting libssl.a/libcrypto.a have no Go dependency).

Without `-DSFU_USE_BORINGSSL=ON`, the build uses system OpenSSL via
`find_package(OpenSSL)` as before -- both are supported; see the note
in transport/dtls/dtls.c about the one API difference that mattered
(EVP_EC_gen is OpenSSL-3.0-only, so cert generation uses the older,
portable EC_KEY sequence that works on both).

## libsrtp/
Should rebuild to use boringSSL as well.

## libjuice/, xxhash/

Not yet needed as vendored sources -- libsrtp2 is currently pulled from
the system package (libsrtp2-dev) via pkg-config in CMakeLists.txt.
libjuice and xxhash aren't wired in yet (see the ICE/hashing notes in
docs/ or the project's architecture discussion).
