#pragma once
// deko3d PipelineTable (AI-assisted). Generated data: PipelineTable.inc (T15).
//
// Deko3d-free, host-compilable description of every render-state entry emitted
// by scripts/gen-dk-pipeline-table.py from docs/deko3d/pipeline_table.json.
// deko3d.h is intentionally NOT included here so these structs compile on any host
// and can be unit-tested without libnx/deko3d. The .inc produced by T15 fills
// kPipelines[] / kSamplers[] with concrete values from this table.
#include <cstdint>

namespace dkpipe {

// One value per distinct deko3d depthCompareOp present in the table (Always,
// Gequal, Greater, Never). Always is also the sentinel used when depth_test is
// OFF (JSON depthCompareOp == null), matching the .inc generation rule
// "depth_test false -> Compare::Always".
enum class Compare : uint8_t {
	Always,
	Gequal,
	Greater,
	Never,
};

// One value per distinct blend MODE in the table. The comment on each value is
// the GL equation it stands for, taken from docs/deko3d/pipeline_table.md
// "Blend states". (Off is the blend-disabled case.)
enum class Blend : uint8_t {
	Off,            // glDisable(GL_BLEND)
	StdAlpha,       // GL_FUNC_ADD; src=SrcAlpha dst=OneMinusSrcAlpha srcA=One dstA=Zero
	PremulAdd,      // GL_FUNC_ADD; src=SrcAlpha dst=One        srcA=One   dstA=Zero
	Fix128Add,      // GL_FUNC_ADD; src=One       dst=One        srcA=One   dstA=Zero
	Fix64Sub,       // GL_FUNC_ADD; src=ConstColor dst=ConstColor srcA=One dstA=Zero; blendColor 0.5
	SrcAlphaSub,    // GL_FUNC_REVERSE_SUBTRACT; src=SrcAlpha dst=One srcA=One dstA=Zero
	DstAlphaAdd,    // GL_FUNC_ADD; src=DstAlpha dst=One srcA=DstAlpha dstA=One
};

enum class Wrap : uint8_t {
	Repeat,
	ClampToEdge,
};

// 2-valued min/mag filter axis (DkFilter). The third JSON minFilter value
// "LinearMipmapLinear" is carried separately by SamplerDesc::mipmap, which T16
// maps to DkMipFilter_Linear; here min_f/mag_f only hold the DkFilter value
// (Nearest | Linear) from the `filt` axis.
enum class Filter : uint8_t {
	Nearest,
	Linear,
};

struct PipelineDesc {
	const char* id;
	bool depth_test;
	Compare depth_op;
	bool depth_write;
	Blend blend;
};

struct SamplerDesc {
	uint8_t index;
	Wrap wrap_u, wrap_v;
	Filter min_f, mag_f;
	bool mipmap;
};

}  // namespace dkpipe
