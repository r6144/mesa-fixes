#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"

#include "pipe/p_shader_tokens.h"
#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_dump.h"
#include "tgsi/tgsi_util.h"

#include "nvfx_context.h"
#include "nvfx_state.h"

/* TODO (at least...):
 *  1. Indexed consts  + ARL
 *  3. NV_vp11, NV_vp2, NV_vp3 features
 *       - extra arith opcodes
 *       - branching
 *       - texture sampling
 *       - indexed attribs
 *       - indexed results
 *  4. bugs
 */

#include "nv30_vertprog.h"
#include "nv40_vertprog.h"

#define NVFX_VP_INST_DEST_CLIP(n) ((~0 - 6) + (n))

struct nvfx_vpc {
	struct nvfx_vertex_program *vp;

	struct nvfx_vertex_program_exec *vpi;

	unsigned r_temps;
	unsigned r_temps_discard;
	struct nvfx_sreg r_result[PIPE_MAX_SHADER_OUTPUTS];
	struct nvfx_sreg *r_address;
	struct nvfx_sreg *r_temp;

	struct nvfx_sreg *imm;
	unsigned nr_imm;

	unsigned hpos_idx;
};

static struct nvfx_sreg
temp(struct nvfx_vpc *vpc)
{
	int idx = ffs(~vpc->r_temps) - 1;

	if (idx < 0) {
		NOUVEAU_ERR("out of temps!!\n");
		assert(0);
		return nvfx_sr(NVFXSR_TEMP, 0);
	}

	vpc->r_temps |= (1 << idx);
	vpc->r_temps_discard |= (1 << idx);
	return nvfx_sr(NVFXSR_TEMP, idx);
}

static INLINE void
release_temps(struct nvfx_vpc *vpc)
{
	vpc->r_temps &= ~vpc->r_temps_discard;
	vpc->r_temps_discard = 0;
}

static struct nvfx_sreg
constant(struct nvfx_vpc *vpc, int pipe, float x, float y, float z, float w)
{
	struct nvfx_vertex_program *vp = vpc->vp;
	struct nvfx_vertex_program_data *vpd;
	int idx;

	if (pipe >= 0) {
		for (idx = 0; idx < vp->nr_consts; idx++) {
			if (vp->consts[idx].index == pipe)
				return nvfx_sr(NVFXSR_CONST, idx);
		}
	}

	idx = vp->nr_consts++;
	vp->consts = realloc(vp->consts, sizeof(*vpd) * vp->nr_consts);
	vpd = &vp->consts[idx];

	vpd->index = pipe;
	vpd->value[0] = x;
	vpd->value[1] = y;
	vpd->value[2] = z;
	vpd->value[3] = w;
	return nvfx_sr(NVFXSR_CONST, idx);
}

#define arith(cc,s,o,d,m,s0,s1,s2) \
	nvfx_vp_arith(nvfx, (cc), NVFX_VP_INST_SLOT_##s, NVFX_VP_INST_##s##_OP_##o, (d), (m), (s0), (s1), (s2))

static void
emit_src(struct nvfx_context* nvfx, struct nvfx_vpc *vpc, uint32_t *hw, int pos, struct nvfx_sreg src)
{
	struct nvfx_vertex_program *vp = vpc->vp;
	uint32_t sr = 0;

	switch (src.type) {
	case NVFXSR_TEMP:
		sr |= (NVFX_VP(SRC_REG_TYPE_TEMP) << NVFX_VP(SRC_REG_TYPE_SHIFT));
		sr |= (src.index << NVFX_VP(SRC_TEMP_SRC_SHIFT));
		break;
	case NVFXSR_INPUT:
		sr |= (NVFX_VP(SRC_REG_TYPE_INPUT) <<
		       NVFX_VP(SRC_REG_TYPE_SHIFT));
		vp->ir |= (1 << src.index);
		hw[1] |= (src.index << NVFX_VP(INST_INPUT_SRC_SHIFT));
		break;
	case NVFXSR_CONST:
		sr |= (NVFX_VP(SRC_REG_TYPE_CONST) <<
		       NVFX_VP(SRC_REG_TYPE_SHIFT));
		assert(vpc->vpi->const_index == -1 ||
		       vpc->vpi->const_index == src.index);
		vpc->vpi->const_index = src.index;
		break;
	case NVFXSR_NONE:
		sr |= (NVFX_VP(SRC_REG_TYPE_INPUT) <<
		       NVFX_VP(SRC_REG_TYPE_SHIFT));
		break;
	default:
		assert(0);
	}

	if (src.negate)
		sr |= NVFX_VP(SRC_NEGATE);

	if (src.abs)
		hw[0] |= (1 << (21 + pos));

	sr |= ((src.swz[0] << NVFX_VP(SRC_SWZ_X_SHIFT)) |
	       (src.swz[1] << NVFX_VP(SRC_SWZ_Y_SHIFT)) |
	       (src.swz[2] << NVFX_VP(SRC_SWZ_Z_SHIFT)) |
	       (src.swz[3] << NVFX_VP(SRC_SWZ_W_SHIFT)));

	switch (pos) {
	case 0:
		hw[1] |= ((sr & NVFX_VP(SRC0_HIGH_MASK)) >>
			  NVFX_VP(SRC0_HIGH_SHIFT)) << NVFX_VP(INST_SRC0H_SHIFT);
		hw[2] |= (sr & NVFX_VP(SRC0_LOW_MASK)) <<
			  NVFX_VP(INST_SRC0L_SHIFT);
		break;
	case 1:
		hw[2] |= sr << NVFX_VP(INST_SRC1_SHIFT);
		break;
	case 2:
		hw[2] |= ((sr & NVFX_VP(SRC2_HIGH_MASK)) >>
			  NVFX_VP(SRC2_HIGH_SHIFT)) << NVFX_VP(INST_SRC2H_SHIFT);
		hw[3] |= (sr & NVFX_VP(SRC2_LOW_MASK)) <<
			  NVFX_VP(INST_SRC2L_SHIFT);
		break;
	default:
		assert(0);
	}
}

static void
emit_dst(struct nvfx_context* nvfx, struct nvfx_vpc *vpc, uint32_t *hw, int slot, struct nvfx_sreg dst)
{
	struct nvfx_vertex_program *vp = vpc->vp;

	switch (dst.type) {
	case NVFXSR_TEMP:
		if(!nvfx->is_nv4x)
			hw[0] |= (dst.index << NV30_VP_INST_DEST_TEMP_ID_SHIFT);
		else {
			hw[3] |= NV40_VP_INST_DEST_MASK;
			if (slot == 0) {
				hw[0] |= (dst.index <<
					  NV40_VP_INST_VEC_DEST_TEMP_SHIFT);
			} else {
				hw[3] |= (dst.index <<
					  NV40_VP_INST_SCA_DEST_TEMP_SHIFT);
			}
		}
		break;
	case NVFXSR_OUTPUT:
		/* TODO: this may be wrong because on nv30 COL0 and BFC0 are swapped */
		switch (dst.index) {
		case NVFX_VP_INST_DEST_CLIP(0):
			vp->or |= (1 << 6);
			vp->clip_ctrl |= NV34TCL_VP_CLIP_PLANES_ENABLE_PLANE0;
			dst.index = NVFX_VP(INST_DEST_FOGC);
			break;
		case NVFX_VP_INST_DEST_CLIP(1):
			vp->or |= (1 << 7);
			vp->clip_ctrl |= NV34TCL_VP_CLIP_PLANES_ENABLE_PLANE1;
			dst.index = NVFX_VP(INST_DEST_FOGC);
			break;
		case NVFX_VP_INST_DEST_CLIP(2):
			vp->or |= (1 << 8);
			vp->clip_ctrl |= NV34TCL_VP_CLIP_PLANES_ENABLE_PLANE2;
			dst.index = NVFX_VP(INST_DEST_FOGC);
			break;
		case NVFX_VP_INST_DEST_CLIP(3):
			vp->or |= (1 << 9);
			vp->clip_ctrl |= NV34TCL_VP_CLIP_PLANES_ENABLE_PLANE3;
			dst.index = NVFX_VP(INST_DEST_PSZ);
			break;
		case NVFX_VP_INST_DEST_CLIP(4):
			vp->or |= (1 << 10);
			vp->clip_ctrl |= NV34TCL_VP_CLIP_PLANES_ENABLE_PLANE4;
			dst.index = NVFX_VP(INST_DEST_PSZ);
			break;
		case NVFX_VP_INST_DEST_CLIP(5):
			vp->or |= (1 << 11);
			vp->clip_ctrl |= NV34TCL_VP_CLIP_PLANES_ENABLE_PLANE5;
			dst.index = NVFX_VP(INST_DEST_PSZ);
			break;
		default:
			if(!nvfx->is_nv4x) {
				switch (dst.index) {
				case NV30_VP_INST_DEST_COL0 : vp->or |= (1 << 0); break;
				case NV30_VP_INST_DEST_COL1 : vp->or |= (1 << 1); break;
				case NV30_VP_INST_DEST_BFC0 : vp->or |= (1 << 2); break;
				case NV30_VP_INST_DEST_BFC1 : vp->or |= (1 << 3); break;
				case NV30_VP_INST_DEST_FOGC: vp->or |= (1 << 4); break;
				case NV30_VP_INST_DEST_PSZ  : vp->or |= (1 << 5); break;
				case NV30_VP_INST_DEST_TC(0): vp->or |= (1 << 14); break;
				case NV30_VP_INST_DEST_TC(1): vp->or |= (1 << 15); break;
				case NV30_VP_INST_DEST_TC(2): vp->or |= (1 << 16); break;
				case NV30_VP_INST_DEST_TC(3): vp->or |= (1 << 17); break;
				case NV30_VP_INST_DEST_TC(4): vp->or |= (1 << 18); break;
				case NV30_VP_INST_DEST_TC(5): vp->or |= (1 << 19); break;
				case NV30_VP_INST_DEST_TC(6): vp->or |= (1 << 20); break;
				case NV30_VP_INST_DEST_TC(7): vp->or |= (1 << 21); break;
				}
			} else {
				switch (dst.index) {
				case NV40_VP_INST_DEST_COL0 : vp->or |= (1 << 0); break;
				case NV40_VP_INST_DEST_COL1 : vp->or |= (1 << 1); break;
				case NV40_VP_INST_DEST_BFC0 : vp->or |= (1 << 2); break;
				case NV40_VP_INST_DEST_BFC1 : vp->or |= (1 << 3); break;
				case NV40_VP_INST_DEST_FOGC: vp->or |= (1 << 4); break;
				case NV40_VP_INST_DEST_PSZ  : vp->or |= (1 << 5); break;
				case NV40_VP_INST_DEST_TC(0): vp->or |= (1 << 14); break;
				case NV40_VP_INST_DEST_TC(1): vp->or |= (1 << 15); break;
				case NV40_VP_INST_DEST_TC(2): vp->or |= (1 << 16); break;
				case NV40_VP_INST_DEST_TC(3): vp->or |= (1 << 17); break;
				case NV40_VP_INST_DEST_TC(4): vp->or |= (1 << 18); break;
				case NV40_VP_INST_DEST_TC(5): vp->or |= (1 << 19); break;
				case NV40_VP_INST_DEST_TC(6): vp->or |= (1 << 20); break;
				case NV40_VP_INST_DEST_TC(7): vp->or |= (1 << 21); break;
				}
			}
			break;
		}

		if(!nvfx->is_nv4x) {
			hw[3] |= (dst.index << NV30_VP_INST_DEST_SHIFT);
			hw[0] |= NV30_VP_INST_VEC_DEST_TEMP_MASK | (1<<20);

			/*XXX: no way this is entirely correct, someone needs to
			 *     figure out what exactly it is.
			 */
			hw[3] |= 0x800;
		} else {
			hw[3] |= (dst.index << NV40_VP_INST_DEST_SHIFT);
			if (slot == 0) {
				hw[0] |= NV40_VP_INST_VEC_RESULT;
				hw[0] |= NV40_VP_INST_VEC_DEST_TEMP_MASK | (1<<20);
			} else {
				hw[3] |= NV40_VP_INST_SCA_RESULT;
				hw[3] |= NV40_VP_INST_SCA_DEST_TEMP_MASK;
			}
		}
		break;
	default:
		assert(0);
	}
}

static void
nvfx_vp_arith(struct nvfx_context* nvfx, struct nvfx_vpc *vpc, int slot, int op,
	      struct nvfx_sreg dst, int mask,
	      struct nvfx_sreg s0, struct nvfx_sreg s1,
	      struct nvfx_sreg s2)
{
	struct nvfx_vertex_program *vp = vpc->vp;
	uint32_t *hw;

	vp->insns = realloc(vp->insns, ++vp->nr_insns * sizeof(*vpc->vpi));
	vpc->vpi = &vp->insns[vp->nr_insns - 1];
	memset(vpc->vpi, 0, sizeof(*vpc->vpi));
	vpc->vpi->const_index = -1;

	hw = vpc->vpi->data;

	hw[0] |= (NVFX_COND_TR << NVFX_VP(INST_COND_SHIFT));
	hw[0] |= ((0 << NVFX_VP(INST_COND_SWZ_X_SHIFT)) |
		  (1 << NVFX_VP(INST_COND_SWZ_Y_SHIFT)) |
		  (2 << NVFX_VP(INST_COND_SWZ_Z_SHIFT)) |
		  (3 << NVFX_VP(INST_COND_SWZ_W_SHIFT)));

	if(!nvfx->is_nv4x) {
		hw[1] |= (op << NV30_VP_INST_VEC_OPCODE_SHIFT);
//		hw[3] |= NVFX_VP(INST_SCA_DEST_TEMP_MASK);
//		hw[3] |= (mask << NVFX_VP(INST_VEC_WRITEMASK_SHIFT));

		if (dst.type == NVFXSR_OUTPUT) {
			if (slot)
				hw[3] |= (mask << NV30_VP_INST_SDEST_WRITEMASK_SHIFT);
			else
				hw[3] |= (mask << NV30_VP_INST_VDEST_WRITEMASK_SHIFT);
		} else {
			if (slot)
				hw[3] |= (mask << NV30_VP_INST_STEMP_WRITEMASK_SHIFT);
			else
				hw[3] |= (mask << NV30_VP_INST_VTEMP_WRITEMASK_SHIFT);
		}
	 } else {
		if (slot == 0) {
			hw[1] |= (op << NV40_VP_INST_VEC_OPCODE_SHIFT);
			hw[3] |= NV40_VP_INST_SCA_DEST_TEMP_MASK;
			hw[3] |= (mask << NV40_VP_INST_VEC_WRITEMASK_SHIFT);
	    } else {
			hw[1] |= (op << NV40_VP_INST_SCA_OPCODE_SHIFT);
			hw[0] |= (NV40_VP_INST_VEC_DEST_TEMP_MASK | (1 << 20));
			hw[3] |= (mask << NV40_VP_INST_SCA_WRITEMASK_SHIFT);
		}
	}

	emit_dst(nvfx, vpc, hw, slot, dst);
	emit_src(nvfx, vpc, hw, 0, s0);
	emit_src(nvfx, vpc, hw, 1, s1);
	emit_src(nvfx, vpc, hw, 2, s2);
}

static INLINE struct nvfx_sreg
tgsi_src(struct nvfx_vpc *vpc, const struct tgsi_full_src_register *fsrc) {
	struct nvfx_sreg src = { 0 };

	switch (fsrc->Register.File) {
	case TGSI_FILE_INPUT:
		src = nvfx_sr(NVFXSR_INPUT, fsrc->Register.Index);
		break;
	case TGSI_FILE_CONSTANT:
		src = constant(vpc, fsrc->Register.Index, 0, 0, 0, 0);
		break;
	case TGSI_FILE_IMMEDIATE:
		src = vpc->imm[fsrc->Register.Index];
		break;
	case TGSI_FILE_TEMPORARY:
		src = vpc->r_temp[fsrc->Register.Index];
		break;
	default:
		NOUVEAU_ERR("bad src file\n");
		break;
	}

	src.abs = fsrc->Register.Absolute;
	src.negate = fsrc->Register.Negate;
	src.swz[0] = fsrc->Register.SwizzleX;
	src.swz[1] = fsrc->Register.SwizzleY;
	src.swz[2] = fsrc->Register.SwizzleZ;
	src.swz[3] = fsrc->Register.SwizzleW;
	return src;
}

static INLINE struct nvfx_sreg
tgsi_dst(struct nvfx_vpc *vpc, const struct tgsi_full_dst_register *fdst) {
	struct nvfx_sreg dst = { 0 };

	switch (fdst->Register.File) {
	case TGSI_FILE_OUTPUT:
		dst = vpc->r_result[fdst->Register.Index];
		break;
	case TGSI_FILE_TEMPORARY:
		dst = vpc->r_temp[fdst->Register.Index];
		break;
	case TGSI_FILE_ADDRESS:
		dst = vpc->r_address[fdst->Register.Index];
		break;
	default:
		NOUVEAU_ERR("bad dst file\n");
		break;
	}

	return dst;
}

static INLINE int
tgsi_mask(uint tgsi)
{
	int mask = 0;

	if (tgsi & TGSI_WRITEMASK_X) mask |= NVFX_VP_MASK_X;
	if (tgsi & TGSI_WRITEMASK_Y) mask |= NVFX_VP_MASK_Y;
	if (tgsi & TGSI_WRITEMASK_Z) mask |= NVFX_VP_MASK_Z;
	if (tgsi & TGSI_WRITEMASK_W) mask |= NVFX_VP_MASK_W;
	return mask;
}

static boolean
nvfx_vertprog_parse_instruction(struct nvfx_context* nvfx, struct nvfx_vpc *vpc,
				const struct tgsi_full_instruction *finst)
{
	struct nvfx_sreg src[3], dst, tmp;
	struct nvfx_sreg none = nvfx_sr(NVFXSR_NONE, 0);
	int mask;
	int ai = -1, ci = -1, ii = -1;
	int i;

	if (finst->Instruction.Opcode == TGSI_OPCODE_END)
		return TRUE;

	for (i = 0; i < finst->Instruction.NumSrcRegs; i++) {
		const struct tgsi_full_src_register *fsrc;

		fsrc = &finst->Src[i];
		if (fsrc->Register.File == TGSI_FILE_TEMPORARY) {
			src[i] = tgsi_src(vpc, fsrc);
		}
	}

	for (i = 0; i < finst->Instruction.NumSrcRegs; i++) {
		const struct tgsi_full_src_register *fsrc;

		fsrc = &finst->Src[i];

		switch (fsrc->Register.File) {
		case TGSI_FILE_INPUT:
			if (ai == -1 || ai == fsrc->Register.Index) {
				ai = fsrc->Register.Index;
				src[i] = tgsi_src(vpc, fsrc);
			} else {
				src[i] = temp(vpc);
				arith(vpc, VEC, MOV, src[i], NVFX_VP_MASK_ALL,
				      tgsi_src(vpc, fsrc), none, none);
			}
			break;
		case TGSI_FILE_CONSTANT:
			if ((ci == -1 && ii == -1) ||
			    ci == fsrc->Register.Index) {
				ci = fsrc->Register.Index;
				src[i] = tgsi_src(vpc, fsrc);
			} else {
				src[i] = temp(vpc);
				arith(vpc, VEC, MOV, src[i], NVFX_VP_MASK_ALL,
				      tgsi_src(vpc, fsrc), none, none);
			}
			break;
		case TGSI_FILE_IMMEDIATE:
			if ((ci == -1 && ii == -1) ||
			    ii == fsrc->Register.Index) {
				ii = fsrc->Register.Index;
				src[i] = tgsi_src(vpc, fsrc);
			} else {
				src[i] = temp(vpc);
				arith(vpc, VEC, MOV, src[i], NVFX_VP_MASK_ALL,
				      tgsi_src(vpc, fsrc), none, none);
			}
			break;
		case TGSI_FILE_TEMPORARY:
			/* handled above */
			break;
		default:
			NOUVEAU_ERR("bad src file\n");
			return FALSE;
		}
	}

	dst  = tgsi_dst(vpc, &finst->Dst[0]);
	mask = tgsi_mask(finst->Dst[0].Register.WriteMask);

	switch (finst->Instruction.Opcode) {
	case TGSI_OPCODE_ABS:
		arith(vpc, VEC, MOV, dst, mask, abs(src[0]), none, none);
		break;
	case TGSI_OPCODE_ADD:
		arith(vpc, VEC, ADD, dst, mask, src[0], none, src[1]);
		break;
	case TGSI_OPCODE_ARL:
		arith(vpc, VEC, ARL, dst, mask, src[0], none, none);
		break;
	case TGSI_OPCODE_COS:
		arith(vpc, SCA, COS, dst, mask, none, none, src[0]);
		break;
	case TGSI_OPCODE_DP3:
		arith(vpc, VEC, DP3, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_DP4:
		arith(vpc, VEC, DP4, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_DPH:
		arith(vpc, VEC, DPH, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_DST:
		arith(vpc, VEC, DST, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_EX2:
		arith(vpc, SCA, EX2, dst, mask, none, none, src[0]);
		break;
	case TGSI_OPCODE_EXP:
		arith(vpc, SCA, EXP, dst, mask, none, none, src[0]);
		break;
	case TGSI_OPCODE_FLR:
		arith(vpc, VEC, FLR, dst, mask, src[0], none, none);
		break;
	case TGSI_OPCODE_FRC:
		arith(vpc, VEC, FRC, dst, mask, src[0], none, none);
		break;
	case TGSI_OPCODE_LG2:
		arith(vpc, SCA, LG2, dst, mask, none, none, src[0]);
		break;
	case TGSI_OPCODE_LIT:
		arith(vpc, SCA, LIT, dst, mask, none, none, src[0]);
		break;
	case TGSI_OPCODE_LOG:
		arith(vpc, SCA, LOG, dst, mask, none, none, src[0]);
		break;
	case TGSI_OPCODE_LRP:
		tmp = temp(vpc);
		arith(vpc, VEC, MAD, tmp, mask, neg(src[0]), src[2], src[2]);
		arith(vpc, VEC, MAD, dst, mask, src[0], src[1], tmp);
		break;
	case TGSI_OPCODE_MAD:
		arith(vpc, VEC, MAD, dst, mask, src[0], src[1], src[2]);
		break;
	case TGSI_OPCODE_MAX:
		arith(vpc, VEC, MAX, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_MIN:
		arith(vpc, VEC, MIN, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_MOV:
		arith(vpc, VEC, MOV, dst, mask, src[0], none, none);
		break;
	case TGSI_OPCODE_MUL:
		arith(vpc, VEC, MUL, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_POW:
		tmp = temp(vpc);
		arith(vpc, SCA, LG2, tmp, NVFX_VP_MASK_X, none, none,
		      swz(src[0], X, X, X, X));
		arith(vpc, VEC, MUL, tmp, NVFX_VP_MASK_X, swz(tmp, X, X, X, X),
		      swz(src[1], X, X, X, X), none);
		arith(vpc, SCA, EX2, dst, mask, none, none,
		      swz(tmp, X, X, X, X));
		break;
	case TGSI_OPCODE_RCP:
		arith(vpc, SCA, RCP, dst, mask, none, none, src[0]);
		break;
	case TGSI_OPCODE_RET:
		break;
	case TGSI_OPCODE_RSQ:
		arith(vpc, SCA, RSQ, dst, mask, none, none, abs(src[0]));
		break;
	case TGSI_OPCODE_SEQ:
		arith(vpc, VEC, SEQ, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_SFL:
		arith(vpc, VEC, SFL, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_SGE:
		arith(vpc, VEC, SGE, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_SGT:
		arith(vpc, VEC, SGT, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_SIN:
		arith(vpc, SCA, SIN, dst, mask, none, none, src[0]);
		break;
	case TGSI_OPCODE_SLE:
		arith(vpc, VEC, SLE, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_SLT:
		arith(vpc, VEC, SLT, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_SNE:
		arith(vpc, VEC, SNE, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_SSG:
		arith(vpc, VEC, SSG, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_STR:
		arith(vpc, VEC, STR, dst, mask, src[0], src[1], none);
		break;
	case TGSI_OPCODE_SUB:
		arith(vpc, VEC, ADD, dst, mask, src[0], none, neg(src[1]));
		break;
	case TGSI_OPCODE_XPD:
		tmp = temp(vpc);
		arith(vpc, VEC, MUL, tmp, mask,
		      swz(src[0], Z, X, Y, Y), swz(src[1], Y, Z, X, X), none);
		arith(vpc, VEC, MAD, dst, (mask & ~NVFX_VP_MASK_W),
		      swz(src[0], Y, Z, X, X), swz(src[1], Z, X, Y, Y),
		      neg(tmp));
		break;
	default:
		NOUVEAU_ERR("invalid opcode %d\n", finst->Instruction.Opcode);
		return FALSE;
	}

	release_temps(vpc);
	return TRUE;
}

static boolean
nvfx_vertprog_parse_decl_output(struct nvfx_context* nvfx, struct nvfx_vpc *vpc,
				const struct tgsi_full_declaration *fdec)
{
	unsigned idx = fdec->Range.First;
	int hw;

	switch (fdec->Semantic.Name) {
	case TGSI_SEMANTIC_POSITION:
		hw = NVFX_VP(INST_DEST_POS);
		vpc->hpos_idx = idx;
		break;
	case TGSI_SEMANTIC_COLOR:
		if (fdec->Semantic.Index == 0) {
			hw = NVFX_VP(INST_DEST_COL0);
		} else
		if (fdec->Semantic.Index == 1) {
			hw = NVFX_VP(INST_DEST_COL1);
		} else {
			NOUVEAU_ERR("bad colour semantic index\n");
			return FALSE;
		}
		break;
	case TGSI_SEMANTIC_BCOLOR:
		if (fdec->Semantic.Index == 0) {
			hw = NVFX_VP(INST_DEST_BFC0);
		} else
		if (fdec->Semantic.Index == 1) {
			hw = NVFX_VP(INST_DEST_BFC1);
		} else {
			NOUVEAU_ERR("bad bcolour semantic index\n");
			return FALSE;
		}
		break;
	case TGSI_SEMANTIC_FOG:
		hw = NVFX_VP(INST_DEST_FOGC);
		break;
	case TGSI_SEMANTIC_PSIZE:
		hw = NVFX_VP(INST_DEST_PSZ);
		break;
	case TGSI_SEMANTIC_GENERIC:
		if (fdec->Semantic.Index <= 7) {
			hw = NVFX_VP(INST_DEST_TC(fdec->Semantic.Index));
		} else {
			NOUVEAU_ERR("bad generic semantic index\n");
			return FALSE;
		}
		break;
	case TGSI_SEMANTIC_EDGEFLAG:
		/* not really an error just a fallback */
		NOUVEAU_ERR("cannot handle edgeflag output\n");
		return FALSE;
	default:
		NOUVEAU_ERR("bad output semantic\n");
		return FALSE;
	}

	vpc->r_result[idx] = nvfx_sr(NVFXSR_OUTPUT, hw);
	return TRUE;
}

static boolean
nvfx_vertprog_prepare(struct nvfx_context* nvfx, struct nvfx_vpc *vpc)
{
	struct tgsi_parse_context p;
	int high_temp = -1, high_addr = -1, nr_imm = 0, i;

	tgsi_parse_init(&p, vpc->vp->pipe.tokens);
	while (!tgsi_parse_end_of_tokens(&p)) {
		const union tgsi_full_token *tok = &p.FullToken;

		tgsi_parse_token(&p);
		switch(tok->Token.Type) {
		case TGSI_TOKEN_TYPE_IMMEDIATE:
			nr_imm++;
			break;
		case TGSI_TOKEN_TYPE_DECLARATION:
		{
			const struct tgsi_full_declaration *fdec;

			fdec = &p.FullToken.FullDeclaration;
			switch (fdec->Declaration.File) {
			case TGSI_FILE_TEMPORARY:
				if (fdec->Range.Last > high_temp) {
					high_temp =
						fdec->Range.Last;
				}
				break;
#if 0 /* this would be nice.. except gallium doesn't track it */
			case TGSI_FILE_ADDRESS:
				if (fdec->Range.Last > high_addr) {
					high_addr =
						fdec->Range.Last;
				}
				break;
#endif
			case TGSI_FILE_OUTPUT:
				if (!nvfx_vertprog_parse_decl_output(nvfx, vpc, fdec))
					return FALSE;
				break;
			default:
				break;
			}
		}
			break;
#if 1 /* yay, parse instructions looking for address regs instead */
		case TGSI_TOKEN_TYPE_INSTRUCTION:
		{
			const struct tgsi_full_instruction *finst;
			const struct tgsi_full_dst_register *fdst;

			finst = &p.FullToken.FullInstruction;
			fdst = &finst->Dst[0];

			if (fdst->Register.File == TGSI_FILE_ADDRESS) {
				if (fdst->Register.Index > high_addr)
					high_addr = fdst->Register.Index;
			}

		}
			break;
#endif
		default:
			break;
		}
	}
	tgsi_parse_free(&p);

	if (nr_imm) {
		vpc->imm = CALLOC(nr_imm, sizeof(struct nvfx_sreg));
		assert(vpc->imm);
	}

	if (++high_temp) {
		vpc->r_temp = CALLOC(high_temp, sizeof(struct nvfx_sreg));
		for (i = 0; i < high_temp; i++)
			vpc->r_temp[i] = temp(vpc);
	}

	if (++high_addr) {
		vpc->r_address = CALLOC(high_addr, sizeof(struct nvfx_sreg));
		for (i = 0; i < high_addr; i++)
			vpc->r_address[i] = temp(vpc);
	}

	vpc->r_temps_discard = 0;
	return TRUE;
}

static void
nvfx_vertprog_translate(struct nvfx_context *nvfx,
			struct nvfx_vertex_program *vp)
{
	struct tgsi_parse_context parse;
	struct nvfx_vpc *vpc = NULL;
	struct nvfx_sreg none = nvfx_sr(NVFXSR_NONE, 0);
	int i;

	vpc = CALLOC(1, sizeof(struct nvfx_vpc));
	if (!vpc)
		return;
	vpc->vp = vp;

	if (!nvfx_vertprog_prepare(nvfx, vpc)) {
		FREE(vpc);
		return;
	}

	/* Redirect post-transform vertex position to a temp if user clip
	 * planes are enabled.  We need to append code to the vtxprog
	 * to handle clip planes later.
	 */
	if (vp->ucp.nr)  {
		vpc->r_result[vpc->hpos_idx] = temp(vpc);
		vpc->r_temps_discard = 0;
	}

	tgsi_parse_init(&parse, vp->pipe.tokens);

	while (!tgsi_parse_end_of_tokens(&parse)) {
		tgsi_parse_token(&parse);

		switch (parse.FullToken.Token.Type) {
		case TGSI_TOKEN_TYPE_IMMEDIATE:
		{
			const struct tgsi_full_immediate *imm;

			imm = &parse.FullToken.FullImmediate;
			assert(imm->Immediate.DataType == TGSI_IMM_FLOAT32);
			assert(imm->Immediate.NrTokens == 4 + 1);
			vpc->imm[vpc->nr_imm++] =
				constant(vpc, -1,
					 imm->u[0].Float,
					 imm->u[1].Float,
					 imm->u[2].Float,
					 imm->u[3].Float);
		}
			break;
		case TGSI_TOKEN_TYPE_INSTRUCTION:
		{
			const struct tgsi_full_instruction *finst;
			finst = &parse.FullToken.FullInstruction;
			if (!nvfx_vertprog_parse_instruction(nvfx, vpc, finst))
				goto out_err;
		}
			break;
		default:
			break;
		}
	}

	/* Write out HPOS if it was redirected to a temp earlier */
	if (vpc->r_result[vpc->hpos_idx].type != NVFXSR_OUTPUT) {
		struct nvfx_sreg hpos = nvfx_sr(NVFXSR_OUTPUT,
						NVFX_VP(INST_DEST_POS));
		struct nvfx_sreg htmp = vpc->r_result[vpc->hpos_idx];

		arith(vpc, VEC, MOV, hpos, NVFX_VP_MASK_ALL, htmp, none, none);
	}

	/* Insert code to handle user clip planes */
	for (i = 0; i < vp->ucp.nr; i++) {
		struct nvfx_sreg cdst = nvfx_sr(NVFXSR_OUTPUT,
						NVFX_VP_INST_DEST_CLIP(i));
		struct nvfx_sreg ceqn = constant(vpc, -1,
						 nvfx->clip.ucp[i][0],
						 nvfx->clip.ucp[i][1],
						 nvfx->clip.ucp[i][2],
						 nvfx->clip.ucp[i][3]);
		struct nvfx_sreg htmp = vpc->r_result[vpc->hpos_idx];
		unsigned mask;

		switch (i) {
		case 0: case 3: mask = NVFX_VP_MASK_Y; break;
		case 1: case 4: mask = NVFX_VP_MASK_Z; break;
		case 2: case 5: mask = NVFX_VP_MASK_W; break;
		default:
			NOUVEAU_ERR("invalid clip dist #%d\n", i);
			goto out_err;
		}

		arith(vpc, VEC, DP4, cdst, mask, htmp, ceqn, none);
	}

	vp->insns[vp->nr_insns - 1].data[3] |= NVFX_VP_INST_LAST;
	vp->translated = TRUE;
out_err:
	tgsi_parse_free(&parse);
	if (vpc->r_temp)
		FREE(vpc->r_temp);
	if (vpc->r_address)
		FREE(vpc->r_address);
	if (vpc->imm)
		FREE(vpc->imm);
	FREE(vpc);
}

boolean
nvfx_vertprog_validate(struct nvfx_context *nvfx)
{
	struct pipe_context *pipe = &nvfx->pipe;
	struct nvfx_screen *screen = nvfx->screen;
	struct nouveau_channel *chan = screen->base.channel;
	struct nouveau_grobj *eng3d = screen->eng3d;
	struct nvfx_vertex_program *vp;
	struct pipe_resource *constbuf;
	struct pipe_transfer *transfer = NULL;
	boolean upload_code = FALSE, upload_data = FALSE;
	int i;

	if (nvfx->render_mode == HW) {
		vp = nvfx->vertprog;
		constbuf = nvfx->constbuf[PIPE_SHADER_VERTEX];

		// TODO: ouch! can't we just use constant slots for these?!
		if ((nvfx->dirty & NVFX_NEW_UCP) ||
		    memcmp(&nvfx->clip, &vp->ucp, sizeof(vp->ucp))) {
			nvfx_vertprog_destroy(nvfx, vp);
			memcpy(&vp->ucp, &nvfx->clip, sizeof(vp->ucp));
		}
	} else {
		vp = nvfx->swtnl.vertprog;
		constbuf = NULL;
	}

	/* Translate TGSI shader into hw bytecode */
	if (!vp->translated)
	{
		nvfx->fallback_swtnl &= ~NVFX_NEW_VERTPROG;
		nvfx_vertprog_translate(nvfx, vp);
		if (!vp->translated) {
			nvfx->fallback_swtnl |= NVFX_NEW_VERTPROG;
			return FALSE;
		}
	}

	/* Allocate hw vtxprog exec slots */
	if (!vp->exec) {
		struct nouveau_resource *heap = nvfx->screen->vp_exec_heap;
		uint vplen = vp->nr_insns;

		if (nouveau_resource_alloc(heap, vplen, vp, &vp->exec)) {
			while (heap->next && heap->size < vplen) {
				struct nvfx_vertex_program *evict;

				evict = heap->next->priv;
				nouveau_resource_free(&evict->exec);
			}

			if (nouveau_resource_alloc(heap, vplen, vp, &vp->exec))
				assert(0);
		}

		upload_code = TRUE;
	}

	/* Allocate hw vtxprog const slots */
	if (vp->nr_consts && !vp->data) {
		struct nouveau_resource *heap = nvfx->screen->vp_data_heap;

		if (nouveau_resource_alloc(heap, vp->nr_consts, vp, &vp->data)) {
			while (heap->next && heap->size < vp->nr_consts) {
				struct nvfx_vertex_program *evict;

				evict = heap->next->priv;
				nouveau_resource_free(&evict->data);
			}

			if (nouveau_resource_alloc(heap, vp->nr_consts, vp, &vp->data))
				assert(0);
		}

		/*XXX: handle this some day */
		assert(vp->data->start >= vp->data_start_min);

		upload_data = TRUE;
		if (vp->data_start != vp->data->start)
			upload_code = TRUE;
	}

	/* If exec or data segments moved we need to patch the program to
	 * fixup offsets and register IDs.
	 */
	if (vp->exec_start != vp->exec->start) {
		for (i = 0; i < vp->nr_insns; i++) {
			struct nvfx_vertex_program_exec *vpi = &vp->insns[i];

			if (vpi->has_branch_offset) {
				assert(0);
			}
		}

		vp->exec_start = vp->exec->start;
	}

	if (vp->nr_consts && vp->data_start != vp->data->start) {
		for (i = 0; i < vp->nr_insns; i++) {
			struct nvfx_vertex_program_exec *vpi = &vp->insns[i];

			if (vpi->const_index >= 0) {
				vpi->data[1] &= ~NVFX_VP(INST_CONST_SRC_MASK);
				vpi->data[1] |=
					(vpi->const_index + vp->data->start) <<
					NVFX_VP(INST_CONST_SRC_SHIFT);

			}
		}

		vp->data_start = vp->data->start;
	}

	/* Update + Upload constant values */
	if (vp->nr_consts) {
		float *map = NULL;

		if (constbuf) {
			map = pipe_buffer_map(pipe, constbuf,
					      PIPE_TRANSFER_READ,
					      &transfer);
		}

		for (i = 0; i < vp->nr_consts; i++) {
			struct nvfx_vertex_program_data *vpd = &vp->consts[i];

			if (vpd->index >= 0) {
				if (!upload_data &&
				    !memcmp(vpd->value, &map[vpd->index * 4],
					    4 * sizeof(float)))
					continue;
				memcpy(vpd->value, &map[vpd->index * 4],
				       4 * sizeof(float));
			}

			BEGIN_RING(chan, eng3d, NV34TCL_VP_UPLOAD_CONST_ID, 5);
			OUT_RING  (chan, i + vp->data->start);
			OUT_RINGp (chan, (uint32_t *)vpd->value, 4);
		}

		if (constbuf)
			pipe_buffer_unmap(pipe, constbuf, transfer);
	}

	/* Upload vtxprog */
	if (upload_code) {
#if 0
		for (i = 0; i < vp->nr_insns; i++) {
			NOUVEAU_MSG("VP %d: 0x%08x\n", i, vp->insns[i].data[0]);
			NOUVEAU_MSG("VP %d: 0x%08x\n", i, vp->insns[i].data[1]);
			NOUVEAU_MSG("VP %d: 0x%08x\n", i, vp->insns[i].data[2]);
			NOUVEAU_MSG("VP %d: 0x%08x\n", i, vp->insns[i].data[3]);
		}
#endif
		BEGIN_RING(chan, eng3d, NV34TCL_VP_UPLOAD_FROM_ID, 1);
		OUT_RING  (chan, vp->exec->start);
		for (i = 0; i < vp->nr_insns; i++) {
			BEGIN_RING(chan, eng3d, NV34TCL_VP_UPLOAD_INST(0), 4);
			OUT_RINGp (chan, vp->insns[i].data, 4);
		}
	}

	if(nvfx->dirty & (NVFX_NEW_VERTPROG | NVFX_NEW_UCP))
	{
		WAIT_RING(chan, 7);
		OUT_RING(chan, RING_3D(NV34TCL_VP_START_FROM_ID, 1));
		OUT_RING(chan, vp->exec->start);
		if(nvfx->is_nv4x) {
			OUT_RING(chan, RING_3D(NV40TCL_VP_ATTRIB_EN, 2));
			OUT_RING(chan, vp->ir);
			OUT_RING(chan, vp->or);
		}
		OUT_RING(chan, RING_3D(NV34TCL_VP_CLIP_PLANES_ENABLE, 1));
		OUT_RING(chan, vp->clip_ctrl);
	}

	return TRUE;
}

void
nvfx_vertprog_destroy(struct nvfx_context *nvfx, struct nvfx_vertex_program *vp)
{
	vp->translated = FALSE;

	if (vp->nr_insns) {
		FREE(vp->insns);
		vp->insns = NULL;
		vp->nr_insns = 0;
	}

	if (vp->nr_consts) {
		FREE(vp->consts);
		vp->consts = NULL;
		vp->nr_consts = 0;
	}

	nouveau_resource_free(&vp->exec);
	vp->exec_start = 0;
	nouveau_resource_free(&vp->data);
	vp->data_start = 0;
	vp->data_start_min = 0;

	vp->ir = vp->or = vp->clip_ctrl = 0;
}
