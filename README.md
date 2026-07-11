# mezon-sfu
A high-performance optimized for HD meetings and large-scale deployment.

The SFU core is built around a lock-free room execution model. Each room is processed by a dedicated isolated thread, ensuring deterministic packet routing without shared-state contention. Media packets are forwarded through a zero-copy pipeline powered by io_uring, enabling efficient fan-out to thousands of subscribers with minimal CPU overhead.

# diagram
<img width="2720" height="1320" alt="mezon_sfu_core_architecture" src="https://github.com/user-attachments/assets/67892729-529a-4bcf-9490-1598d6526a5a" />


# build
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
