
## Installation of LLVM/Clang (C2 version)
C2 is based on LLVM 3.6 and some parts of Clang 3.6.
To install C2, follow the steps below. The example shows
how to install in **$HOME/llvm-c2**, but any other dir should work.

To build:
```
git clone git://github.com/llvm-mirror/llvm.git
cd llvm/
git checkout -b release_36 origin/release_36
cd projects
git clone git://github.com/llvm-mirror/compiler-rt.git
cd compiler-rt
git checkout -b release_36 origin/release_36
cd ../../tools
git clone git://github.com/c2lang/clang.git
cd clang
git checkout -b c2master_36 origin/c2master_36
cd ../../..
mkdir llvm_build
cd llvm_build
export CC=gcc (optional)
export CXX=g++ (optional)
# on linux:
../llvm/configure --enable-optimized --prefix=$HOME/llvm-c2/ --with-python=/usr/bin/python2
# on OS X:
../llvm/configure --enable-optimized --prefix=$HOME/llvm-c2/
make -j4
make install
```

Voila! When adding **$HOME/llvm-c2/bin** to the your $PATH, you should be able
to build C2.
```
export PATH=$HOME/llvm-c2/bin:$PATH
```

## Installation of C2C
To build C2: (llvm-c2/bin must be in PATH)
```
git clone git://github.com/c2lang/c2compiler.git
cd c2compiler/c2c
mkdir build
cd build
cmake . ..
make -j4
```
If all goes well, the **c2c** executable should appear in the build directory.

If you get an error with some Clang/C2 errors, try updating your clang C2 archive.

## Getting and Building C2C
To run the unit tests:
```
cd tools/tester
make
cd ../../c2c/build
make test
```

