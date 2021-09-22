# Dockerfile derived from easy::jit's .travis.yml

FROM ubuntu:18.04

LABEL manteiner Juan Manuel Martinez Caamaño jmartinezcaamao@gmail.com

ENV http_proxy http://child-prc.intel.com:913
ENV https_proxy http://child-prc.intel.com:913
ARG DEBIAN_FRONTEND=noninteractive

ARG branch=master

# add sources
RUN apt-get update
RUN apt-get install -y software-properties-common
RUN apt-add-repository -y "ppa:ubuntu-toolchain-r/test" 
RUN deb http://apt.llvm.org/trusty/ llvm-toolchain-trusty-6.0 main | tee -a /etc/apt/sources.list > /dev/null 
 
# install apt packages, base first, then travis
RUN apt-get update 
RUN apt-get upgrade -y 
RUN apt-get install -y build-essential python3 python3-pip git wget unzip cmake && \
    apt-get install -y ninja-build g++-6 llvm-6.0-dev llvm-6.0-tools clang-6.0 libopencv-dev

# checkout
RUN git clone --depth=50 --branch=${branch} https://github.com/luo-cheng2021/easy-just-in-time.git easy-just-in-time && cd easy-just-in-time

# install other deps
RUN cd /easy-just-in-time && pip3 install --user --upgrade pip
RUN cd /easy-just-in-time && pip3 install --user lit 

# compile and test!
RUN cd easy-just-in-time && \
  mkdir _build && cd _build && \ 
  git clone --depth=1 https://github.com/google/benchmark.git && git clone --depth=1 https://github.com/google/googletest.git benchmark/googletest && mkdir benchmark/_build && cd benchmark/_build && cmake .. -GNinja -DCMAKE_INSTALL_PREFIX=`pwd`/../_install && ninja && ninja install && cd ../.. && \ 
  cmake -DLLVM_DIR=/usr/lib/llvm-6.0/cmake -DCMAKE_CXX_COMPILER=clang++-6.0 -DCMAKE_C_COMPILER=clang-6.0 -DEASY_JIT_EXAMPLE=ON -DEASY_JIT_BENCHMARK=ON -DBENCHMARK_DIR=`pwd`/benchmark/_install -DCMAKE_INSTALL_PREFIX=`pwd`/../_install .. -G Ninja && \ 
  ninja && \ 
  ninja install && \ 
  ninja check && \ 
  echo ok!
