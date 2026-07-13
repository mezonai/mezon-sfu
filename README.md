# mezon-sfu
A high-performance optimized for HD meetings and large-scale deployment.

The SFU core is built around a lock-free room execution model. Each room is processed by a dedicated isolated thread, ensuring deterministic packet routing without shared-state contention. Media packets are forwarded through a zero-copy pipeline powered by io_uring, enabling efficient fan-out to thousands of subscribers with minimal CPU overhead.

# diagram
<img width="1440" height="840" alt="image" src="https://github.com/user-attachments/assets/f3772f59-b4b9-4086-9a6e-a80346da1bef" />

<img width="1440" height="720" alt="image" src="https://github.com/user-attachments/assets/3c4832a0-ff90-4357-a3b9-5b329fd6ee0d" />

<img width="2720" height="1320" alt="mezon_sfu_core_architecture" src="https://github.com/user-attachments/assets/67892729-529a-4bcf-9490-1598d6526a5a" />


# build
`sudo apt install libsrtp2-1 libsrtp2-dev`

build liburing
```
git clone https://github.com/axboe/liburing.git
cd liburing

./configure
make -j$(nproc)
sudo make install
```

build mezon sfu
```
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
ctest --output-on-failure
```
