#pragma once
/* rdma-display addition (MIT, see ../LICENSE): frame generation between frames in VkBuffers
 * (host memory imported with VK_EXT_external_memory_host) instead of swapchain images. */

#include "framegen.h"

/* Records, into a command buffer on a compute queue of the family set with
 * afmf_framegen_set_family (or any compute family when that was never called):
 *   1. the new frame from `src` (B8G8R8A8 / R8G8B8A8 rows of `src_row_pixels` pixels starting
 *      at `src_offset`) into the colour ring,
 *   2. the luma pyramid, scene change detector and, when `dst` is not VK_NULL_HANDLE, the block
 *      search and the interpolated frame between the previous and the new frame,
 *   3. that frame into `dst` at `dst_offset`, rows of `dst_row_pixels`, made available to the host.
 * Without `dst` the frame only enters the history. False (nothing recorded) when `fg` was created
 * with a direct path. `slot` selects the constants region (fewer than the `slots` it was created
 * with may be in flight). */
bool afmf_framegen_record_host(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd,
                               uint32_t slot, VkBuffer src, VkDeviceSize src_offset, uint32_t src_row_pixels,
                               VkBuffer dst, VkDeviceSize dst_offset, uint32_t dst_row_pixels);

/* 1 = optical flow at full resolution, 2 = at half resolution (performance mode) */
uint32_t afmf_framegen_flow_scale(const struct afmf_framegen *fg);
uint32_t afmf_framegen_levels(const struct afmf_framegen *fg);
