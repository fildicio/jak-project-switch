/*
 * @file dk_state.cpp
 * deko3d migration Phase 2 (AI-assisted, T16): bridge from the deko3d-free
 * PipelineTable (T14/T15) to real deko3d render-state structs. Switch-only.
 *
 * Every deko3d name below was checked against docs/deko3d-ref/include/deko3d.h
 * (deko3d 0.5.0, same version as the build image). Each switch lists every
 * enum value with no `default:` so the compiler warns on a future gap.
 *
 * The 1:1 mapping is:
 *   Compare  -> DkCompareOp_{Always,Gequal,Greater,Never}
 *   Blend    -> DkBlendOp (colorBlendOp/alphaBlendOp) + DkBlendFactor factors,
 *                with the GL equation taken from pipeline_table.md / PipelineTable.h.
 *   Wrap     -> DkWrapMode_{Repeat,ClampToEdge} (DkSampler::wrapMode[0/1])
 *   Filter   -> DkFilter_{Nearest,Linear} (DkSampler::minFilter/magFilter)
 *   mipmap   -> DkMipFilter_{None,Nearest,Linear} (DkSampler::mipFilter, split out
 *                from the 3-valued JSON minFilter by the T14 `filt`+`mipmap` axes).
 */

#if defined(__SWITCH__)

#include "game/graphics/deko3d_renderer/dk_state.h"
#include "game/graphics/deko3d_renderer/dk_state_internal.h"  // T26: apply_blend_const decl
#include "game/graphics/deko3d_renderer/PipelineTable.inc"  // T35b: kPipelines/kSamplers (static per TU)

// switch_run_logf lives in run_log.h, which cannot be included next to <deko3d.h>
// (pulls in common/common_types.h -> the u128 collision with deko3d.h). Same hand
// declaration as dk_device.cpp (L41-43).
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace dkpipe {

// Map Blend -> colorBlendOp/alphaBlendOp + 4 factors (GL-equation faithful).
// All DkBlendOp_* and DkBlendFactor_* names checked in deko3d.h.
static void to_blend(const Blend b, DkBlendState* out) {
  switch (b) {
    case Blend::Off:  // glDisable(GL_BLEND): leave default; cs.blendEnableMask stays 0.
      dkBlendStateDefaults(out);
      break;
    case Blend::StdAlpha:  // Add; src=SrcAlpha dst=InvSrcAlpha srcA=One dstA=Zero
      dkBlendStateDefaults(out);
      out->colorBlendOp = DkBlendOp_Add;
      out->srcColorBlendFactor = DkBlendFactor_SrcAlpha;
      out->dstColorBlendFactor = DkBlendFactor_InvSrcAlpha;
      out->alphaBlendOp = DkBlendOp_Add;
      out->srcAlphaBlendFactor = DkBlendFactor_One;
      out->dstAlphaBlendFactor = DkBlendFactor_Zero;
      break;
    case Blend::PremulAdd:  // Add; src=SrcAlpha dst=One srcA=One dstA=Zero
      dkBlendStateDefaults(out);
      out->colorBlendOp = DkBlendOp_Add;
      out->srcColorBlendFactor = DkBlendFactor_SrcAlpha;
      out->dstColorBlendFactor = DkBlendFactor_One;
      out->alphaBlendOp = DkBlendOp_Add;
      out->srcAlphaBlendFactor = DkBlendFactor_One;
      out->dstAlphaBlendFactor = DkBlendFactor_Zero;
      break;
    case Blend::Fix128Add:  // Add; src=One dst=One srcA=One dstA=Zero
      dkBlendStateDefaults(out);
      out->colorBlendOp = DkBlendOp_Add;
      out->srcColorBlendFactor = DkBlendFactor_One;
      out->dstColorBlendFactor = DkBlendFactor_One;
      out->alphaBlendOp = DkBlendOp_Add;
      out->srcAlphaBlendFactor = DkBlendFactor_One;
      out->dstAlphaBlendFactor = DkBlendFactor_Zero;
      break;
    case Blend::Fix64Sub:  // Add; src=ConstColor dst=ConstColor srcA=One dstA=Zero; glBlendColor(0.5,...)
      dkBlendStateDefaults(out);
      out->colorBlendOp = DkBlendOp_Add;
      out->srcColorBlendFactor = DkBlendFactor_ConstColor;
      out->dstColorBlendFactor = DkBlendFactor_ConstColor;
      out->alphaBlendOp = DkBlendOp_Add;
      out->srcAlphaBlendFactor = DkBlendFactor_One;
      out->dstAlphaBlendFactor = DkBlendFactor_Zero;
      // NOTE (reviewer-confirmed): DkBlendState has no blend-color field. The GL path
      // calls glBlendColor(0.5,0.5,0.5,0.5); in deko3d the constant is a command-buffer
      // setting. It is emitted at bind time by dkpipe::apply_blend_const() (see
      // dk_state_internal.h / the T26 implementation below), which calls
      // dkCmdBufSetBlendConst(cb, 0.5f, 0.5f, 0.5f, 0.5f) (deko3d.h:1293) when a Fix64Sub
      // pipeline is bound. Inert until P3.2 binds pipelines.
      break;
    case Blend::SrcAlphaSub:  // RevSub; src=SrcAlpha dst=One srcA=One dstA=Zero
      dkBlendStateDefaults(out);
      out->colorBlendOp = DkBlendOp_RevSub;
      out->srcColorBlendFactor = DkBlendFactor_SrcAlpha;
      out->dstColorBlendFactor = DkBlendFactor_One;
      out->alphaBlendOp = DkBlendOp_RevSub;
      out->srcAlphaBlendFactor = DkBlendFactor_One;
      out->dstAlphaBlendFactor = DkBlendFactor_Zero;
      break;
    case Blend::DstAlphaAdd:  // Add; src=DstAlpha dst=One srcA=DstAlpha dstA=One
      dkBlendStateDefaults(out);
      out->colorBlendOp = DkBlendOp_Add;
      out->srcColorBlendFactor = DkBlendFactor_DstAlpha;
      out->dstColorBlendFactor = DkBlendFactor_One;
      out->alphaBlendOp = DkBlendOp_Add;
      out->srcAlphaBlendFactor = DkBlendFactor_DstAlpha;
      out->dstAlphaBlendFactor = DkBlendFactor_One;
      break;
  }
}


void to_deko3d(const PipelineDesc& p, DkStates* out) {
  // Each starts from its dk*Defaults() (checked in deko3d.h) so all fields
  // other than the ones we set are in the known good state.
  dkDepthStencilStateDefaults(&out->ds);
  dkColorStateDefaults(&out->cs);
  dkBlendStateDefaults(&out->bs);

  // Depth test + compare + write (fields: depthTestEnable/depthWriteEnable/depthCompareOp).
  out->ds.depthTestEnable = p.depth_test ? 1u : 0u;
  out->ds.depthWriteEnable = p.depth_write ? 1u : 0u;
  switch (p.depth_op) {
    case Compare::Always:
      out->ds.depthCompareOp = DkCompareOp_Always;
      break;
    case Compare::Gequal:
      out->ds.depthCompareOp = DkCompareOp_Gequal;
      break;
    case Compare::Greater:
      out->ds.depthCompareOp = DkCompareOp_Greater;
      break;
    case Compare::Never:
      out->ds.depthCompareOp = DkCompareOp_Never;
      break;
  }

  // Blend: enable blending for all RGBA output components in DkColorState,
  // then fill DkBlendState. blendEnableMask is per color component
  // (DkColorMask_R=1<<0 .. DkColorMask_A=1<<3 in deko3d.h), so DkColorMask_RGBA
  // turns blending on for R,G,B and A.
  if (p.blend != Blend::Off) {
    out->cs.blendEnableMask = DkColorMask_RGBA;
  }
  to_blend(p.blend, &out->bs);
}

void to_deko3d(const SamplerDesc& s, DkSampler* out) {
  // Start from defaults so unbound fields (lod*, borderColor, anisotropy,
  // compareEnable, reductionMode) are in the known good state.
  dkSamplerDefaults(out);

  // Wrap modes: wrapMode[0] is S/U, wrapMode[1] is T/V (DkSampler in deko3d.h).
  switch (s.wrap_u) {
    case Wrap::Repeat:
      out->wrapMode[0] = DkWrapMode_Repeat;
      break;
    case Wrap::ClampToEdge:
      out->wrapMode[0] = DkWrapMode_ClampToEdge;
      break;
  }
  switch (s.wrap_v) {
    case Wrap::Repeat:
      out->wrapMode[1] = DkWrapMode_Repeat;
      break;
    case Wrap::ClampToEdge:
      out->wrapMode[1] = DkWrapMode_ClampToEdge;
      break;
  }

  // Min / mag filter (DkFilter, 2-valued).
  switch (s.min_f) {
    case Filter::Nearest:
      out->minFilter = DkFilter_Nearest;
      break;
    case Filter::Linear:
      out->minFilter = DkFilter_Linear;
      break;
  }
  switch (s.mag_f) {
    case Filter::Nearest:
      out->magFilter = DkFilter_Nearest;
      break;
    case Filter::Linear:
      out->magFilter = DkFilter_Linear;
      break;
  }

  // Mip filter: reconstruct the 3-valued JSON minFilter (Nearest|Linear|
  // LinearMipmapLinear) from the two T14 axes (filt + mipmap):
  //   mipmap=false                  -> DkMipFilter_None     (no mip: GL_NEAREST / GL_LINEAR)
  //   mipmap=true, min_f=Nearest   -> DkMipFilter_Nearest  (GL_NEAREST_MIPMAP_NEAREST)
  //   mipmap=true, min_f=Linear    -> DkMipFilter_Linear   (GL_LINEAR_MIPMAP_LINEAR)
  switch (s.min_f) {
    case Filter::Nearest:
      out->mipFilter = s.mipmap ? DkMipFilter_Nearest : DkMipFilter_None;
      break;
    case Filter::Linear:
      out->mipFilter = s.mipmap ? DkMipFilter_Linear : DkMipFilter_None;
      break;
  }
}

// T26: emit the FIX64_SUB blend constant (the GL glBlendColor(0.5,...) equivalent)
// onto the command buffer when a Fix64Sub pipeline is bound. DkBlendState has no
// blend-color field, so this is the only way to supply the ConstantColor factor a
// Fix64Sub pipeline uses (see to_blend). All other Blend values need no constant and
// are a no-op here. Exhaustive, no default: a future Blend value fails to compile.
void apply_blend_const(DkCmdBuf cb, Blend b) {
  switch (b) {
    case Blend::Off:        break;
    case Blend::StdAlpha:   break;
    case Blend::PremulAdd:  break;
    case Blend::Fix128Add:  break;
    case Blend::Fix64Sub:   dkCmdBufSetBlendConst(cb, 0.5f, 0.5f, 0.5f, 0.5f); break;
    case Blend::SrcAlphaSub: break;
    case Blend::DstAlphaAdd: break;
  }
}

// T35a: emit the complete fixed-function state for one pipeline table entry.
// Order mirrors record_splash_3d in dk_device.cpp (the reference 3D path).
// Cull mode is always DkFace_None for parity with the splash/tie/tfrag 3D paths
// (glDisable(GL_CULL_FACE)); every other state comes from the DkStates trio via
// to_deko3d (T16) plus the T26 blend const.
void record_graphics_state(DkCmdBuf cb, const PipelineDesc& p) {
  if (cb == nullptr) {
    switch_run_logf("[dk] state null cb\n");
    return;
  }
  DkStates s;
  to_deko3d(p, &s);                          // (T16) fills cs / bs / ds
  dkCmdBufBindDepthStencilState(cb, &s.ds);
  dkCmdBufBindColorState(cb, &s.cs);
  dkCmdBufBindBlendStates(cb, 0, &s.bs, 1);
  apply_blend_const(cb, p.blend);               // (T26)
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  dkCmdBufBindRasterizerState(cb, &rs);
  DkColorWriteState cw;
  dkColorWriteStateDefaults(&cw);
  dkCmdBufBindColorWriteState(cb, &cw);
  DkMultisampleState ms;
  dkMultisampleStateDefaults(&ms);
  dkCmdBufBindMultisampleState(cb, &ms);
}

// T35b: pure lookup over the T15 table. Map the already-derived draw axes to a
// PipelineDesc entry + a kSamplers index. No bit derivation, no GPU calls, no
// allocation. The axes are derived by the caller (T38) from the GOAL draw-mode
// word per docs/deko3d/pipeline_table.md L27 table.
const PipelineDesc* find_for_draw_mode(const DrawModeAxes& ax,
                                       uint8_t* sampler_idx) {
  Wrap wu = ax.clamp_s ? Wrap::ClampToEdge : Wrap::Repeat;
  Wrap wv = ax.clamp_t ? Wrap::ClampToEdge : Wrap::Repeat;
  Filter ft = ax.filt ? Filter::Linear : Filter::Nearest;
  for (const PipelineDesc& p : kPipelines) {
    if (p.depth_test == ax.depth_test && p.depth_op == ax.depth_op &&
        p.depth_write == ax.depth_write && p.blend == ax.blend) {
      if (sampler_idx) {
        for (uint32_t i = 0; i < sizeof(kSamplers) / sizeof(kSamplers[0]); ++i) {
          const SamplerDesc& s = kSamplers[i];
          if (s.wrap_u == wu && s.wrap_v == wv && s.min_f == ft &&
              s.mag_f == ft && s.mipmap == ax.mipmap) {
            *sampler_idx = static_cast<uint8_t>(i);
            break;
          }
        }
      }
      return &p;
    }
  }
  return nullptr;
}

// T95 (AI-assisted): sampler-only lookup (see dk_state.h). Same matching rules
// as the sampler half of find_for_draw_mode.
bool find_sampler(bool clamp_s, bool clamp_t, bool linear, bool mipmap, uint8_t* idx) {
  Wrap wu = clamp_s ? Wrap::ClampToEdge : Wrap::Repeat;
  Wrap wv = clamp_t ? Wrap::ClampToEdge : Wrap::Repeat;
  Filter ft = linear ? Filter::Linear : Filter::Nearest;
  for (uint32_t i = 0; i < sizeof(kSamplers) / sizeof(kSamplers[0]); ++i) {
    const SamplerDesc& s = kSamplers[i];
    if (s.wrap_u == wu && s.wrap_v == wv && s.min_f == ft && s.mag_f == ft &&
        s.mipmap == mipmap) {
      *idx = static_cast<uint8_t>(i);
      return true;
    }
  }
  return false;
}

}  // namespace dkpipe

#endif  // __SWITCH__
