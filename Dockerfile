FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG MIMALLOC_REF=v2.1.7
ARG BORINGSSL_REF=0.20250818.0
ARG LIBSRTP_REF=v2.6.0
ARG NATSC_REF=v3.11.0

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake git golang-go liburing-dev libuv1-dev \
    ninja-build pkg-config python3 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /tmp/deps

RUN git clone --depth 1 --branch "${MIMALLOC_REF}" https://github.com/microsoft/mimalloc.git && \
    cmake -S mimalloc -B mimalloc/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DMI_BUILD_TESTS=OFF -DMI_BUILD_OBJECT=OFF && \
    cmake --build mimalloc/build && cmake --install mimalloc/build

RUN git clone --depth 1 --branch "${BORINGSSL_REF}" https://boringssl.googlesource.com/boringssl && \
    cmake -S boringssl -B boringssl/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF && \
    cmake --build boringssl/build && \
    install -d /usr/local/include/boringssl /usr/local/lib/boringssl && \
    cp -a boringssl/include/. /usr/local/include/boringssl/ && \
    cp boringssl/build/libcrypto.a boringssl/build/libssl.a /usr/local/lib/boringssl/

RUN git clone --depth 1 --branch "${LIBSRTP_REF}" https://github.com/cisco/libsrtp.git && \
    cd libsrtp && \
    ./configure --prefix=/usr/local --enable-openssl --disable-shared \
      crypto_CFLAGS="-I/usr/local/include/boringssl" \
      crypto_LIBS="-L/usr/local/lib/boringssl -lcrypto -lstdc++" && \
    make -j"$(nproc)" && make install

RUN git clone --depth 1 --branch "${NATSC_REF}" https://github.com/nats-io/nats.c.git && \
    cmake -S nats.c -B nats.c/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DNATS_BUILD_STREAMING=OFF -DNATS_BUILD_EXAMPLES=OFF -DNATS_BUILD_TESTS=OFF && \
    cmake --build nats.c/build && cmake --install nats.c/build

WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DSFU_DIAG_LOG=OFF -DSFU_NET_BACKEND=io_uring && \
    cmake --build build --target mezon-sfu

FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libssl3t64 liburing2 libuv1t64 netcat-openbsd && \
    rm -rf /var/lib/apt/lists/* && \
    groupadd --gid 10001 mezon && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin mezon

COPY --from=builder /src/build/mezon-sfu /usr/local/bin/mezon-sfu
COPY --from=builder /usr/local/lib/libmimalloc.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/libnats.so* /usr/local/lib/
COPY docker/config.ini.template /etc/mezon-sfu/config.ini.template
COPY docker/docker-entrypoint.sh docker/healthcheck.sh /usr/local/bin/
RUN chmod 0555 /usr/local/bin/mezon-sfu /usr/local/bin/docker-entrypoint.sh /usr/local/bin/healthcheck.sh && \
    mkdir -p /run/mezon-sfu && chown 10001:10001 /run/mezon-sfu && ldconfig

USER 10001:10001
EXPOSE 7000/udp 8000/tcp
HEALTHCHECK --interval=10s --timeout=3s --start-period=10s --retries=3 CMD ["/usr/local/bin/healthcheck.sh"]
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD ["mezon-sfu"]
