#pragma once

/*
 * @file dk_device_internal.h
 * deko3d migration Phase 2.3 (T22a, AI-assisted): dk-TU internals pass-throughs.
 *
 * Unlike dk_device.h (the public, deko3d-free interface that dk_renderer.cpp talks
 * to), this file is deko3d-INTERNAL: it is only ever included from code already
 * inside #if defined(__SWITCH__), so it is explicitly ALLOWED to include
 * <deko3d.h> and to surface the raw deko3d handles (DkDevice / DkQueue /
 * DkCmdBuf / DkCmdList / DkGpuAddr) plus the upload staging pointers and the
 * upload command-region offsets. T22b/T22c (the P2.3 dk_texture_gpu store) build
 * on these; nothing outside __SWITCH__-guarded code should include this file.
 */

#if defined(__SWITCH__)

#include <deko3d.h>  // DkDevice/Queue/CmdBuf/CmdList/GpuAddr; uint8_t/uint32_t come in via <stdint.h>

namespace dkdev {

//! device/queue/cmd-buf handles, mirrored 1:1 from dk_device.cpp module state
//! (s_dev / s_queue / s_cmd) so a P2.3 consumer can touch the GPU directly.
DkDevice device_handle();
DkQueue queue_handle();
DkCmdBuf cmd_handle();

//! CPU + GPU pointers to the upload staging area (kOffStaging into s_dataMem),
//! the same region copy_to_gpu() fills before each chunk.
uint8_t* staging_cpu();
DkGpuAddr staging_gpu();

//! CPU pointer to the start of the data memblock (s_dataMem).
uint8_t* data_cpu();

//! The dedicated upload command region: its byte offset (kOffUpload) and size
//! (kCmdUpload) into the command memory — the same region copy_to_gpu() re-records
//! per chunk, kept distinct from the per-frame cmd slices that present_frame owns.
uint32_t upload_cmd_off();
uint32_t upload_cmd_size();

//! Submit `list`, signal the upload fence (flushed) and wait on it -- exactly the
//! T18 tail of copy_to_gpu(): dkQueueSubmitCommands + dkQueueSignalFence
//! (s_uploadFence, true) + dkFenceWait. Render-thread only.
//! T51: waits at most 1 s; returns false on timeout/error (then every later call returns
//! false without submitting - the queue is treated as wedged).
bool submit_upload_cmds(DkCmdList list);

// T22b: P2.3 texture-pool accessors. desc_off()/desc_slot_stride() expose the
// descriptor-region base (kOffDesc) and the per-slot stride (DK_IMAGE_DESCRIPTOR_ALIGNMENT)
// so dk_texture_gpu.cpp places each live texture's DkImageDescriptor at
// (desc_off() + 0x100 + i * desc_slot_stride()) without hard-coding the offset.
// data_mem() exposes the data DkMemBlock as a handle (AddMemory needs the memblock, not
// just a CPU pointer) for the buffer->image upload.
uint32_t desc_off();
uint32_t desc_slot_stride();
DkMemBlock data_mem();

// T33b: tfrag3 shader pair loaded by T33a (valid only when
// tfrag3_shaders_loaded() returns true -- copies, not references).
DkShader tfrag3_vsh();
DkShader tfrag3_fsh();
bool tfrag3_shaders_loaded();
// T72 (AI-assisted): SHRUB shader pair, loaded right after tfrag3 in the same code memblock.
DkShader shrub_vsh();
DkShader shrub_fsh();
bool shrub_shaders_loaded();
// T77 (AI-assisted): ETIE pair (tie envmap shine), loaded right after shrub.
DkShader etie_vsh();
DkShader etie_fsh();
bool etie_shaders_loaded();
// T78: merc2 pair (characters).
DkShader merc_vsh();
DkShader merc_fsh();
bool merc_shaders_loaded();
// T79: sprite3 pair (particles, HUD sprites).
DkShader sprite_vsh();
DkShader sprite_fsh();
bool sprite_shaders_loaded();
// T80: direct pair (menus, HUD text, subtitles).
DkShader direct_vsh();
DkShader direct_fsh();
bool direct_shaders_loaded();

// T34b: sampler descriptor set written at init by T34a.
// GPU addr = data-pool base + sampler_set_off(); samplerId for
// dkMakeTextureHandle indexes [0, sampler_set_count()).
uint32_t sampler_set_off();
uint32_t sampler_set_count();

}  // namespace dkdev

#endif  // __SWITCH__