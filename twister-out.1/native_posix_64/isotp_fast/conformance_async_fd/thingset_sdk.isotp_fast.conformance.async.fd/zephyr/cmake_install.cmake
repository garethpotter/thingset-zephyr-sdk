# Install script for directory: /home/gaz/thingset-zephyr-sdk/zephyr

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/zephyr/arch/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/zephyr/lib/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/zephyr/soc/posix/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/zephyr/boards/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/zephyr/subsys/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/zephyr/drivers/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/modules/ThingSet SDK/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/modules/thingset-node-c/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/modules/cmsis/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/modules/hal_espressif/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/modules/hal_nordic/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/modules/st/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/modules/stm32/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/modules/loramac-node/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/modules/mbedtls/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/modules/picolibc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/modules/tinycrypt/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/modules/zcbor/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/zephyr/kernel/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/zephyr/cmake/flash/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/zephyr/cmake/usage/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/gaz/thingset-zephyr-sdk/thingset-zephyr-sdk.git/twister-out/native_posix_64/isotp_fast/conformance_async_fd/thingset_sdk.isotp_fast.conformance.async.fd/zephyr/cmake/reports/cmake_install.cmake")
endif()

