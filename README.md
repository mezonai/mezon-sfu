# mezon-sfu
A high-performance optimized for HD meetings and large-scale deployment.

The SFU core is built around a lock-free room execution model. Each room is processed by a dedicated isolated thread, ensuring deterministic packet routing without shared-state contention. Media packets are forwarded through a zero-copy pipeline powered by io_uring, enabling efficient fan-out to thousands of subscribers with minimal CPU overhead.

# diagram
<img width="1440" height="840" alt="image" src="https://github.com/user-attachments/assets/f3772f59-b4b9-4086-9a6e-a80346da1bef" />

<img width="1440" height="720" alt="image" src="https://github.com/user-attachments/assets/3c4832a0-ff90-4357-a3b9-5b329fd6ee0d" />

<img width="2720" height="1320" alt="mezon_sfu_core_architecture" src="https://github.com/user-attachments/assets/67892729-529a-4bcf-9490-1598d6526a5a" />


 # topology this wires up
 
    [NIC] -> [dispatcher core: multishot recvmsg, SSRC/4-tuple hash]
                   |  SPSC ring per worker
                   v
    [worker core 0..N: pop inbox, forward via send_zc, reap completions]

 Core 0 is reserved for the dispatcher; cores 1..N-1 are workers. This is a placeholder policy -- production topology should account for NUMA (Non-Uniform Memory Access) nodes and leave a core free for the kernel's network softirq handling, but the mapping itself is what matters for now: one dispatcher, N workers, no shared mutable state between them beyond the SPSC rings.

# build
`sudo apt install libmimalloc-dev`

build liburing
```
git clone https://github.com/axboe/liburing.git
cd liburing

./configure
make -j$(nproc)
sudo make install
```

build boringSSL
```
git clone https://boringssl.googlesource.com/boringssl
cd boringssl
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
sudo make install
sudo ldconfig
```
```
cd boringssl
sudo mkdir /usr/local/include/boringssl
sudo cp -rf include/* /usr/local/include/boringssl/
sudo cp -rf build/bssl /usr/local/bin/
sudo mkdir /usr/local/lib/boringssl
sudo cp -rf build/lib* /usr/local/lib/boringssl/
```

build  libsrtp
```
git clone https://github.com/cisco/libsrtp.git
git checkout 24b3bf8

cd libsrtp
./configure --enable-openssl \
  crypto_CFLAGS="-I/usr/local/include/boringssl/" \
  crypto_LIBS="-L/usr/local/lib/boringssl/ -lcrypto"
make
```

build mezon sfu
```
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
ctest --output-on-failure
```
