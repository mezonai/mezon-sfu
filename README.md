# mezon-sfu
A high-performance optimized for HD meetings and large-scale deployment. 

# diagram
<img width="2720" height="1320" alt="mezon_sfu_core_architecture" src="https://github.com/user-attachments/assets/67892729-529a-4bcf-9490-1598d6526a5a" />


# build
```
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
ctest --output-on-failure
```
