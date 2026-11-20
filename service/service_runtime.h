/* SPDX-License-Identifier: GPL-2.0-or-later
 * ps4mac_port: the graphics service's side of the runtime (service_runtime.c). */
#ifndef PS4MAC_SERVICE_RUNTIME_H
#define PS4MAC_SERVICE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

typedef void (*ServiceGpuRange)(uintptr_t address, uint64_t size);

/* The bridge's mapping messages, applied to the service's table (and the video core's hooks). */
void service_memory_mapped(uintptr_t address, uint64_t size, int prot);
void service_memory_unmapped(uintptr_t address, uint64_t size);
void service_memory_protected(uintptr_t address, uint64_t size, int prot);
size_t service_memory_range_count(void);
void service_set_clock_start(uint64_t ns);
void service_install_fault_handler(void);
void runtime_report(void); /* also called by the video core */
typedef void (*ServiceProtect)(uintptr_t address, uint64_t size, int read, int write);
void service_set_protect_sender(ServiceProtect sender); /* the guest applies page protection */

#endif
