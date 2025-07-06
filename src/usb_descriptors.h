/**
 * @file usb_descriptors.h
 * @brief Custom USB descriptors to ensure Windows COM detection
 */

#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#include <zephyr/kernel.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

// Define Microsoft OS compatibility ID to help Windows find drivers
// This helps Windows identify the device as a standard CDC ACM device
#define MS_OS_DESCRIPTOR_VERSION 0x0100
#define MS_OS_DESCRIPTOR_VENDOR_CODE 0x20
#define MS_OS_DESCRIPTOR_STRING_INDEX 0xEE

/**
 * @brief Register custom USB descriptors for Windows compatibility
 * 
 * This function registers handlers for Microsoft OS descriptors
 * which help Windows identify and install drivers for the device.
 * 
 * @return 0 on success, negative error code on failure
 */
int register_windows_descriptors(void);

/**
 * @brief Release USB port to prevent locking issues
 * 
 * This function helps prevent Windows ACCESS DENIED issues
 * by properly releasing the USB port when not in use.
 */
void release_usb_port(void);

#endif /* USB_DESCRIPTORS_H */
