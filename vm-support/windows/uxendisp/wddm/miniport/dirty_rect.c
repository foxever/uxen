#include <uxenvmlib.h>
#include <uxenv4vlib.h>
#include <uxendisp-common.h>
#include <Windef.h>
#include "dirty_rect.h"

#define DR_CTX_TAG 'TCRD'
#define DR_TIMEOUT_MS 10
#define DR_ONE_MS_IN_HNS 10000
#define DR_USHRT_MAX 0xffff

struct dr_context
{
    void *dev;
    disable_tracking_ptr disable_tracking;
    v4v_addr_t peer;
    v4v_addr_t alt_peer;
    uxen_v4v_ring_handle_t *ring;
    uxen_v4v_ring_handle_t *alt_ring;
    BOOLEAN enabled;
    BOOLEAN alt_ring_active;
};

static void dr_v4v_dpc(uxen_v4v_ring_handle_t *ring, void *ctx1, void *ctx2)
{
    struct dr_context *ctx = (struct dr_context *)ctx1;
    ssize_t len;
    struct update_msg dummy;

    UNREFERENCED_PARAMETER(ctx2);
    ASSERT(KeGetCurrentIrql() == DISPATCH_LEVEL);

    len = uxen_v4v_copy_out(ring, NULL, NULL, &dummy, sizeof(dummy), 0);
    if (len > 0) {
        uxen_v4v_copy_out(ring, NULL, NULL, NULL, 0, 1);
        if (ctx->disable_tracking)
        {
            ctx->disable_tracking(ctx->dev);
            ctx->disable_tracking = NULL;
            ctx->enabled = TRUE;
        }
        if ((ctx->alt_ring_active == FALSE) && (ctx->alt_ring == ring))
        {
            ctx->alt_ring_active = TRUE;
        }
    }

    uxen_v4v_notify();
}

dr_ctx_t dr_init(void *dev, disable_tracking_ptr fn)
{
    struct dr_context *ctx = NULL;

    ctx = (struct dr_context *)ExAllocatePoolWithTag(NonPagedPool,
                                                     sizeof(*ctx),
                                                     DR_CTX_TAG);
    if (ctx == NULL)
    {
        return NULL;
    }

    RtlZeroMemory(ctx, sizeof(*ctx));

    ctx->dev = dev;
    ctx->disable_tracking = fn;

    ctx->peer.port = UXENDISP_PORT;
    ctx->peer.domain = V4V_DOMID_DM;
    ctx->ring = uxen_v4v_ring_bind(UXENDISP_PORT, V4V_DOMID_DM,
                                   UXENDISP_RING_SIZE,
                                   dr_v4v_dpc, ctx, NULL);
    if (!ctx->ring)
    {
        ExFreePoolWithTag(ctx, DR_CTX_TAG);
        return NULL;
    }

    ctx->alt_peer.port = UXENDISP_ALT_PORT;
    ctx->alt_peer.domain = V4V_DOMID_DM;
    ctx->alt_ring = uxen_v4v_ring_bind(UXENDISP_ALT_PORT, V4V_DOMID_DM,
                                       UXENDISP_RING_SIZE,
                                       dr_v4v_dpc, ctx, NULL);
    if (!ctx->alt_ring)
    {
        uxen_v4v_ring_free(ctx->ring);
        ExFreePoolWithTag(ctx, DR_CTX_TAG);
        return NULL;
    }

    return ctx;
}

void dr_send(dr_ctx_t context, ULONG d_num, RECT *dirty_rect)
{
    struct dr_context *ctx = (struct dr_context *)context;
    ULONG idx;
    struct dirty_rect_msg rect = { 0 };

    if (!ctx->enabled)
        return;

    rect.left = DR_USHRT_MAX;
    rect.top = DR_USHRT_MAX;
    rect.right = 0;
    rect.bottom = 0;

    for (idx = 0; idx < d_num; ++idx)
    {
        rect.left = min(rect.left, dirty_rect[idx].left);
        rect.top = min(rect.top, dirty_rect[idx].top);
        rect.right = max(rect.right, dirty_rect[idx].right);
        rect.bottom = max(rect.bottom, dirty_rect[idx].bottom);
    }

    if ((rect.right > 0) && (rect.bottom > 0)) {
        uxen_v4v_send_from_ring(ctx->ring, &ctx->peer, &rect, sizeof(rect),
                                V4V_PROTO_DGRAM);
        if (ctx->alt_ring_active == TRUE) {
           uxen_v4v_send_from_ring(ctx->alt_ring, &ctx->alt_peer, &rect,
                                   sizeof(rect), V4V_PROTO_DGRAM);
        }
    }
}

void dr_deinit(dr_ctx_t context)
{
    struct dr_context *ctx = (struct dr_context *)context;

    if (ctx)
    {
        uxen_v4v_ring_free(ctx->alt_ring);
        uxen_v4v_ring_free(ctx->ring);
        ExFreePoolWithTag(ctx, DR_CTX_TAG);
    }
}
