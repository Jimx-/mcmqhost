# MCMQHost

## Prerequisites
MCMQHost requires libboost and protobuf compiler to build. On Ubuntu, these can be installed with
```sh
sudo apt-get install libboost-all-dev protobuf-compiler
```

## Build & Run
To build MCMQHost, run 
```sh
mkdir build
cd build
conan install ..
cmake ..
make -j
```
This will build the frontend driver under `bin/mcmqhost`. To run the frontend driver, the SSD configuration and the workload description are required. The example configuration can be found in `ssdconfig.yaml` and `workload.yaml`. You can start the frontend driver with the default configuration with
```sh
bin/mcmqhost -c ../ssdconfig.yaml -w ../workload.yaml
```
More configuration can be found in the `configs` and the `workloads` folders. After starting the frontend driver, it will wait for the simulated SSD to connect and submit the I/O traces to it and produce the result file `result.json`.
