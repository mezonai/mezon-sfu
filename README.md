# mezon-sfu
A high-performance optimized for HD meetings and large-scale deployment. 


# build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
ctest --output-on-failure
