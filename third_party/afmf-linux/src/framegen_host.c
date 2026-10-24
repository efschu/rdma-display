/* rdma-display addition (not part of upstream afmf-linux; same MIT license, see ../LICENSE).
 *
 * afmf_framegen_record() takes the new frame from a swapchain image and copies the interpolated
 * frame into one. rdma-display's frames live in host memory (pinned memfd slots that gamescope
 * composited into on another GPU, imported here with VK_EXT_external_memory_host), so this
 * variant copies the new frame from a VkBuffer straight into the colour ring and the
 * interpolated frame from fg->output straight into a VkBuffer: two whole-frame image copies
 * fewer than going through intermediate images. Everything between is afmf's own recording
 * (afmf_framegen_record_flow), unchanged. Needs fg created without the direct paths. */

#include "framegen_host.h"
#include "framegen_internal.h"

static void copy_region(VkBufferImageCopy *r, VkDeviceSize offset, uint32_t row_pixels, VkExtent2D e)
{
    memset(r, 0, sizeof *r);
    r->bufferOffset = offset;
    r->bufferRowLength = row_pixels;
    r->bufferImageHeight = e.height;
    r->imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r->imageExtent = (VkExtent3D){e.width, e.height, 1};
}

bool afmf_framegen_record_host(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd,
                               uint32_t slot, VkBuffer src, VkDeviceSize src_offset, uint32_t src_row_pixels,
                               VkBuffer dst, VkDeviceSize dst_offset, uint32_t dst_row_pixels)
{
    if (fg->direct || fg->ingest_direct || fg->output.image == VK_NULL_HANDLE)
        return false;
    const struct afmf_framegen_pipelines *pl = dev->framegen_pipelines;
    uint32_t levels = fg->levels;
    uint32_t p = fg->frame_index & 1u;
    uint32_t w = fg->of_extent.width, h = fg->of_extent.height;
    bool companion = dst != VK_NULL_HANDLE;

    if (!fg->initialized)
        afmf_framegen_initialize(dev, fg, cmd);
    afmf_framegen_write_constants(fg, slot, levels);
    afmf_sync(dev, cmd);
    afmf_framegen_profiler_begin(dev, fg, cmd, slot);

    /* What another device or the host wrote into the source buffer becomes visible to the copy. */
    VkMemoryBarrier host_in = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
    };
    dev->fns.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &host_in,
                                  0, NULL, 0, NULL);
    VkBufferImageCopy in;
    copy_region(&in, src_offset, src_row_pixels, fg->extent);
    vkCmdCopyBufferToImage(cmd, src, fg->color[p].image, VK_IMAGE_LAYOUT_GENERAL, 1, &in);
    afmf_sync(dev, cmd);
    if (fg->flow_scale > 1) {
        struct downsample_push push = {
            .size = {(int32_t)w, (int32_t)h},
            .source = {(int32_t)fg->extent.width, (int32_t)fg->extent.height},
        };
        dev->fns.cmd_push_constants(cmd, pl->layouts[PASS_DOWNSAMPLE], VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                    (uint32_t)sizeof push, &push);
        afmf_dispatch(dev, cmd, pl->downsample[fg->half], pl->layouts[PASS_DOWNSAMPLE],
                      fg->sets[p][SET_DOWNSAMPLE], NULL, 0, (w + 7) / 8, (h + 7) / 8, 1);
    }
    afmf_framegen_profiler_mark(dev, fg, cmd, slot, STAGE_INGEST);

    afmf_framegen_record_flow(dev, fg, cmd, slot, p, levels, companion);

    if (companion) {
        afmf_sync(dev, cmd); /* the copy engine reads what the interpolator wrote */
        VkBufferImageCopy out;
        copy_region(&out, dst_offset, dst_row_pixels, fg->extent);
        dev->fns.cmd_copy_image_to_buffer(cmd, fg->output.image, VK_IMAGE_LAYOUT_GENERAL, dst, 1, &out);
        afmf_framegen_profiler_mark(dev, fg, cmd, slot, STAGE_OUTPUT);
        VkMemoryBarrier host_out = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        dev->fns.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                                      &host_out, 0, NULL, 0, NULL);
    }
    fg->frame_index++;
    return true;
}

uint32_t afmf_framegen_flow_scale(const struct afmf_framegen *fg) { return fg->flow_scale; }
uint32_t afmf_framegen_levels(const struct afmf_framegen *fg) { return fg->levels; }
