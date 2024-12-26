#!/bin/bash

set -x

# Color codes
C_RED="\033[31m"
C_GREEN="\033[32m"
C_END="\033[0m"

CMAKE_MIN_VERSION="3.18"
TAR_MIN_VERSION="1.26"

BUILD_DIR="build"

CLEAN_BUILD="false"
ARGS=()

# Parse command-line arguments
for i in "$@"; do
  case $i in
    -c*|--clean*)
      CLEAN_BUILD="true"
      ;;
    -*|--*)
      echo "Unknown option $i"
      exit 1
      ;;
    *)
      ARGS=("${ARGS[@]}" $i)
      ;;
  esac
done

# Determine the number of CPU cores
if [ ! -f "/proc/cpuinfo" ]; then
  CPU_CORE=$(sysctl -n hw.ncpu)
else
  CPU_CORE=$(grep -c "processor" /proc/cpuinfo)
fi
if [ ${CPU_CORE} -eq 0 ]; then
  CPU_CORE=1
fi

echo "CPU cores: ${CPU_CORE}"

# Clean build if specified
if [[ "${CLEAN_BUILD}" = "true" ]]; then
  rm -rf "${BUILD_DIR}" buildtrees deps pkg
fi

if [[ "${ARGS[0]}" = "clean" ]]; then
  rm -rf "${BUILD_DIR}" buildtrees deps pkg
  exit 0
fi

if [[ "${ARGS[0]}" = "tools" ]]; then
  echo "Building tools..."
  if [[ ! -d ${BUILD_DIR} ]]; then
    mkdir ${BUILD_DIR}
  fi
  cd ${BUILD_DIR}
  cmake -DCMAKE_C_COMPILER=/usr/local/bin/gcc \
        -DCMAKE_CXX_COMPILER=/usr/local/bin/g++ \
        -DUSE_PIKA_TOOLS=ON \
        ..

  if [ $? -ne 0 ]; then
    echo -e "${C_RED}Error:${C_END} cmake configuration for tools failed."
    exit 1
  fi

  make -j ${CPU_CORE} CC=/usr/local/bin/gcc CXX=/usr/local/bin/g++

  if [ $? -eq 0 ]; then
    echo -e "${C_GREEN}Tools build complete.${C_END} Output files are in ${C_GREEN}${BUILD_DIR}${C_END}."
  else
    echo -e "${C_RED}Error:${C_END} Tools build failed."
    exit 1
  fi
  exit 0
fi

# Check for required programs
function check_program() {
  if ! command -v $1 >/dev/null 2>&1; then
    echo -e "${C_RED}Error:${C_END} Program ${C_GREEN}$1${C_END} not found. Please install it."
    exit 1
  fi
}

# Check versions
function version_compare() {
  if [[ "$1" == "$2" ]]; then
    return 0
  fi
  if [[ "$(printf '%s\n' "$1" "$2" | sort -rV | head -n1)" == "$1" ]]; then
    echo -e "${C_RED}Error:${C_END} $3 version $2 is less than the minimum required version $1."
    exit 1
  fi
}

# Check for required tools
check_program cmake
check_program tar

# Get local cmake version
LOCAL_CMAKE_VERSION=$(cmake --version | grep version | grep -o '[0-9.]\+')
version_compare ${CMAKE_MIN_VERSION} ${LOCAL_CMAKE_VERSION} "cmake"

# Get local tar version
LOCAL_TAR_VERSION=$(tar --version | head -n 1 | grep -o '[0-9.]\+')
version_compare ${TAR_MIN_VERSION} ${LOCAL_TAR_VERSION} "tar"

# Create build directory if it doesn't exist
if [ ! -d ${BUILD_DIR} ]; then
  mkdir ${BUILD_DIR}
fi

# Move to build directory
cd ${BUILD_DIR}

# Configure the project
cmake -DCMAKE_C_COMPILER=/usr/local/bin/gcc \
      -DCMAKE_CXX_COMPILER=/usr/local/bin/g++ \
      ..

if [ $? -ne 0 ]; then
  echo -e "${C_RED}Error:${C_END} cmake configuration failed."
  exit 1
fi

# Build the project
make -j ${CPU_CORE} CC=/usr/local/bin/gcc CXX=/usr/local/bin/g++

if [ $? -eq 0 ]; then
  echo -e "${C_GREEN}Build complete.${C_END} Output files are in ${C_GREEN}${BUILD_DIR}${C_END}."
else
  echo -e "${C_RED}Error:${C_END} Build failed."
  exit 1
fi
