/*
 * Copyright (c) 2012, 2013, Christian Limpach <Christian.Limpach@gmail.com>
 * 
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _PLATFORM_H_
#define _PLATFORM_H_

typedef struct _DRIVER_CONTEXT {
    void *v;
} DRIVER_CONTEXT, *PDRIVER_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DRIVER_CONTEXT, get_driver_context)

struct _FDO_DATA;
typedef struct _FILE_CONTEXT {
    struct _FDO_DATA *fdo_data;
    struct shared_info *user_shared_info;
    MDL *user_shared_info_mdl;
} FILE_CONTEXT, *PFILE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE(FILE_CONTEXT)

typedef struct _FDO_DATA
{
    ULONG signature;
    WDFDEVICE wdf_device;
    WDFINTERRUPT wdf_interrupt;
    PHYSICAL_ADDRESS ctl_mmio_phys;
    struct ctl_mmio *ctl_mmio;
    PHYSICAL_ADDRESS state_bar_phys;
    struct uxp_state_bar *state_bar;
    uint32_t pending_events;
    uint32_t processing_events;
    uint32_t balloon_min, balloon_max;
    WDFQUEUE ioctl_queue;
    WDFQUEUE pending_ioctl_queue;

    KEVENT *time_update_event;
    KEVENT *balloon_update_event;

    struct shared_info *shared_info;
    unsigned int shared_info_gpfn;

    PHYSICAL_ADDRESS bus_conf_phys;
    uint8_t *bus_conf;
} FDO_DATA, *PFDO_DATA;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FDO_DATA, get_fdo_data)

EVT_WDF_OBJECT_CONTEXT_CLEANUP uxp_ev_driver_context_cleanup;

EVT_WDF_DRIVER_DEVICE_ADD uxp_ev_driver_device_add;

EVT_WDF_DEVICE_CONTEXT_CLEANUP uxp_ev_device_context_cleanup;

EVT_WDF_DEVICE_PREPARE_HARDWARE uxp_ev_device_prepare_hardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE uxp_ev_device_release_hardware;

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL uxp_ev_device_io_device_control;

EVT_WDF_FILE_CLEANUP uxp_ev_file_cleanup;

#endif  /* _PLATFORM_H_ */
