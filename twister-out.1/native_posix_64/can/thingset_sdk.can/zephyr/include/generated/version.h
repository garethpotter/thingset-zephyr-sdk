#ifndef _KERNEL_VERSION_H_
#define _KERNEL_VERSION_H_

/*  values come from cmake/version.cmake
 * BUILD_VERSION related  values will be 'git describe',
 * alternatively user defined BUILD_VERSION.
 */

#define ZEPHYR_VERSION_CODE 197632
#define ZEPHYR_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))

#define KERNELVERSION          0x3040000
#define KERNEL_VERSION_NUMBER  0x30400
#define KERNEL_VERSION_MAJOR   3
#define KERNEL_VERSION_MINOR   4
#define KERNEL_PATCHLEVEL      0
#define KERNEL_VERSION_STRING  "3.4.0"

#define BUILD_VERSION zephyr-v2.5.0-31748-gb7e659cdd308

#endif /* _KERNEL_VERSION_H_ */
