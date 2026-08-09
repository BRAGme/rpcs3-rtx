#include "stdafx.h"
#include "RemixTransforms.h"

#ifdef _WIN32

#include "Emu/RSX/Program/RSXFragmentProgram.h"
#include "Emu/RSX/Program/RSXVertexProgram.h"
#include "Emu/RSX/rsx_methods.h"
#include "Emu/system_config.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace remix_rsx
{
	namespace
	{
		constexpr u32 s_legal_constant_slots = 468;
		constexpr u32 s_no_temp = 0x3f;
		constexpr u32 s_no_output = 0x1f;

		// One decoded VLIW instruction.
		struct decoded_instr
		{
			D0 d0{};
			D1 d1{};
			D2 d2{};
			D3 d3{};
			SRC src[3]{};
		};

		// Which source slots a VEC opcode actually consumes, mirroring the decompiler's
		// substitution templates (VertexProgramDecompiler.cpp:597-650).
		u32 vec_source_mask(u32 opcode)
		{
			switch (opcode)
			{
			case RSX_VEC_OPCODE_NOP:
				return 0;
			case RSX_VEC_OPCODE_MOV:
			case RSX_VEC_OPCODE_ARL:
			case RSX_VEC_OPCODE_FRC:
			case RSX_VEC_OPCODE_FLR:
			case RSX_VEC_OPCODE_SSG:
			case RSX_VEC_OPCODE_TXL:
				return 0b001;
			case RSX_VEC_OPCODE_ADD:
				return 0b101;
			case RSX_VEC_OPCODE_MAD:
				return 0b111;
			default:
				// MUL, DP3, DPH, DP4, DST, MIN, MAX and every set-compare: "$0 op $1".
				return 0b011;
			}
		}

		u32 vec_writemask(const decoded_instr& in)
		{
			return (in.d3.vec_writemask_x ? 1u : 0u)
				| (in.d3.vec_writemask_y ? 2u : 0u)
				| (in.d3.vec_writemask_z ? 4u : 0u)
				| (in.d3.vec_writemask_w ? 8u : 0u);
		}

		u32 sca_writemask(const decoded_instr& in)
		{
			return (in.d3.sca_writemask_x ? 1u : 0u)
				| (in.d3.sca_writemask_y ? 2u : 0u)
				| (in.d3.sca_writemask_z ? 4u : 0u)
				| (in.d3.sca_writemask_w ? 8u : 0u);
		}

		bool vec_writes_output(const decoded_instr& in, u32 reg)
		{
			return in.d1.vec_opcode != RSX_VEC_OPCODE_NOP
				&& in.d0.vec_result
				&& in.d3.dst == reg
				&& in.d3.dst != s_no_output
				&& vec_writemask(in) != 0;
		}

		bool sca_writes_output(const decoded_instr& in, u32 reg)
		{
			return in.d1.sca_opcode != RSX_SCA_OPCODE_NOP
				&& !in.d0.vec_result
				&& in.d3.dst == reg
				&& in.d3.dst != s_no_output
				&& sca_writemask(in) != 0;
		}

		// ARL's destination is the address register a0/a1, not a temp: its encoded dst_tmp field only
		// selects which of the two, which is why resolve_bone_index masks it with 1. Counting it as a
		// temp definition made every "what last wrote r<n>" question answerable with an instruction
		// that never touched r<n>. Two titles, one bug, measured at ae94587:
		//   Haze af06f6d32ec048ee   29:ARL>r0.xyz sits between 24:MAD>r0.xyzw and 30:ADD>o0.xyzw, so
		//                           match_mad_chain's accumulator walk stepped onto the ARL and the
		//                           bone-palette chain died one hop in.
		//   R2   1ebcb483789d38a1   4:ARL>r0.x sits between 1:MUL>r0.xyz(attr0,c18.w) and the DP4s
		//                           that read r0, so match_const_affine saw a partial (0x1) xyz write
		//                           and refused the position decode outright - the quantised attribute
		//                           then goes to Remix undecoded.
		// Excluded here rather than at each call site because this is the single definition of "this
		// instruction defines that temp"; resolve_bone_index and audit_indexing test the opcode
		// directly and are unaffected. slice_position compensates below, so an indexed read still
		// drags its ARL into the diagnostic slice.
		bool vec_writes_temp(const decoded_instr& in, u32 tmp)
		{
			return in.d1.vec_opcode != RSX_VEC_OPCODE_NOP
				&& in.d1.vec_opcode != RSX_VEC_OPCODE_ARL
				&& in.d0.dst_tmp == tmp
				&& in.d0.dst_tmp != s_no_temp
				&& vec_writemask(in) != 0;
		}

		bool sca_writes_temp(const decoded_instr& in, u32 tmp)
		{
			return in.d1.sca_opcode != RSX_SCA_OPCODE_NOP
				&& in.d3.sca_dst_tmp == tmp
				&& in.d3.sca_dst_tmp != s_no_temp
				&& sca_writemask(in) != 0;
		}

		// A target of the backward walk: either an output register or a temp.
		struct walk_target
		{
			bool is_output = false;
			u32 index = 0;

			bool operator==(const walk_target& other) const
			{
				return is_output == other.is_output && index == other.index;
			}
		};

		// The operand a matrix chain multiplies.
		struct chain_source
		{
			u32 reg_type = 0; // RSX_VP_REGISTER_TYPE_*
			u32 index = 0;    // input_src or tmp_src

			bool operator==(const chain_source& other) const
			{
				return reg_type == other.reg_type && index == other.index;
			}
		};

		// One instruction of a candidate chain, and which components of the target it supplies.
		// The two are the same thing for a direct write; they part company when a component was
		// reached through a MOV, in which case 'instr' is the instruction that defined the MOV's
		// source and 'mask' is still the component the MOV landed on.
		struct writer_ref
		{
			u32 instr = 0;
			u32 mask = 0;
		};

		struct chain_result
		{
			bool found = false;
			chain_shape shape = chain_shape::none;
			u32 base = 0;
			chain_source source{};
			u32 instructions = 0;
			// Earliest instruction of the matched chain: the bound for the next search back.
			u32 first_instruction = 0;

			// Set when every constant read of the group went through the address register,
			// i.e. this group is a palette rather than a fixed matrix.
			bool indexed = false;
			u32 addr_reg = 0;
			u32 addr_swz = 0;

			// The chain was only found after resolving a MOV-from-temp writer back to the
			// instruction that defined it (resolve_writers).
			bool indirect = false;

			// How many of the four rows the group actually supplies, the slot step between them,
			// and whether each row only carries xyz. A 4/1/false group is the shape every matcher
			// produced up to ae94587; the other combinations only arise for an indexed group and
			// exist so a 3-row or strided palette can be rebuilt exactly as the ucode reads it.
			u32 rows = 4;
			u32 stride = 1;
			bool xyz_only = false;

			// A constant translation the ucode adds *after* this group, which is how Resistance 2
			// writes its object-to-world step ('r1.xyz = c18.xyz + M[a] * pos'). It belongs to the
			// group because it sits between the group and whatever consumes it, and folding it in
			// is the difference between an object at its own position and one at the world origin.
			bool has_bias = false;
			u32 bias_slot = 0;

			// The translation wrote a different register than the rows did, so the group was only
			// found by following it back one hop (match_indexed_affine).
			bool bias_forwarded = false;

			// For a 3-row group, the constant component the ucode moves into the operand's w. It is
			// read back per draw and required to be 1: a 3x4 matrix only transforms a *point* when
			// the fourth coordinate is one, and a group whose w is a scale factor is a different
			// transform wearing the same instructions. umax when the group supplies all four rows.
			u32 w_slot = umax;
			u32 w_component = 0;

			// HPOS.z was recovered from a w-buffer premultiply (repair_wbuffer_z).
			bool wbuffer_z = false;

			// The group is read once per bone and the reads summed: a weighted blend
			// (match_blend_palette). 'indexed' is set too - it is still one palette through one
			// address register - but every read goes through its own component of that register,
			// which is what the four addr_swz entries record.
			bool blended = false;
			u32 blend_bones = 0;
			u32 blend_weight_attribute = 0;
			u8 blend_weight_component[max_blend_bones] = {};
			u8 blend_addr_swz[max_blend_bones] = {};
		};

		// What the chain walk carries across its hops, including back out to the caller.
		struct chain_context
		{
			bool allow_indexed = false;
			bool allow_indirect = true;
			bool refused = false; // a MOV was reached whose definition could not be pinned down
		};

		// Component of 'src' selected by position 'slot' of its swizzle.
		u32 swizzle_component(const SRC& s, u32 slot)
		{
			switch (slot)
			{
			case 0: return s.swz_x;
			case 1: return s.swz_y;
			case 2: return s.swz_z;
			default: return s.swz_w;
			}
		}

		bool single_component(u32 mask, u32& out_component)
		{
			switch (mask)
			{
			case 1: out_component = 0; return true;
			case 2: out_component = 1; return true;
			case 4: out_component = 2; return true;
			case 8: out_component = 3; return true;
			default: return false;
			}
		}

		bool is_broadcast_swizzle(const SRC& s, u32& out_component)
		{
			if (s.swz_x != s.swz_y || s.swz_y != s.swz_z || s.swz_z != s.swz_w)
			{
				return false;
			}

			out_component = s.swz_x;
			return true;
		}

		class program_walker
		{
		public:
			explicit program_walker(const RSXVertexProgram& vp)
			{
				const usz count = vp.data.size() / 4;
				m_instr.reserve(count);

				for (usz i = 0; i < count; ++i)
				{
					decoded_instr in{};
					in.d0.HEX = vp.data[(i * 4) + 0];
					in.d1.HEX = vp.data[(i * 4) + 1];
					in.d2.HEX = vp.data[(i * 4) + 2];
					in.d3.HEX = vp.data[(i * 4) + 3];

					in.src[0].src0l = in.d2.src0l;
					in.src[0].src0h = in.d1.src0h;
					in.src[1].src1 = in.d2.src1;
					in.src[2].src2l = in.d3.src2l;
					in.src[2].src2h = in.d2.src2h;

					m_instr.push_back(in);
				}
			}

			usz size() const { return m_instr.size(); }
			const decoded_instr& operator[](usz i) const { return m_instr[i]; }

			// Every VEC instruction before 'before' that writes 'target', in program order.
			void collect_vec_writers(const walk_target& target, std::vector<u32>& out, u32 before) const
			{
				out.clear();

				for (u32 i = 0; i < before && i < ::size32(m_instr); ++i)
				{
					const bool hit = target.is_output
						? vec_writes_output(m_instr[i], target.index)
						: vec_writes_temp(m_instr[i], target.index);

					if (hit)
					{
						out.push_back(i);
					}
				}
			}

			bool has_sca_writer(const walk_target& target, u32 before) const
			{
				for (u32 i = 0; i < before && i < ::size32(m_instr); ++i)
				{
					const bool hit = target.is_output
						? sca_writes_output(m_instr[i], target.index)
						: sca_writes_temp(m_instr[i], target.index);

					if (hit)
					{
						return true;
					}
				}

				return false;
			}

			// Last VEC instruction before 'before' that writes temp 'tmp'.
			u32 last_temp_writer(u32 tmp, u32 before) const
			{
				u32 result = umax;

				for (u32 i = 0; i < before && i < ::size32(m_instr); ++i)
				{
					if (vec_writes_temp(m_instr[i], tmp))
					{
						result = i;
					}
				}

				return result;
			}

			// The instruction that last defined one component of temp 'tmp' before 'before'.
			// 'out_sca' says the definition came from the SCA half, which is a scalar op and never
			// a matrix row - the co-issued 'MUL>r0.xyzw/RSQ>r2.z' forms in the dumps are two
			// unrelated results sharing an instruction word.
			u32 last_component_writer(u32 tmp, u32 component, u32 before, bool& out_sca) const
			{
				const u32 bit = 1u << component;

				for (u32 i = std::min(before, ::size32(m_instr)); i-- > 0;)
				{
					if (sca_writes_temp(m_instr[i], tmp) && (sca_writemask(m_instr[i]) & bit))
					{
						out_sca = true;
						return i;
					}

					if (vec_writes_temp(m_instr[i], tmp) && (vec_writemask(m_instr[i]) & bit))
					{
						out_sca = false;
						return i;
					}
				}

				return umax;
			}

		private:
			std::vector<decoded_instr> m_instr;
		};

		// Resolve a chain operand through trivial MOV forwarding so "r1 = MOV v0" does not
		// hide the fact that the matrix multiplies the input position.
		chain_source resolve_source(const program_walker& prog, chain_source src, u32 before)
		{
			for (u32 hop = 0; hop < 4; ++hop)
			{
				if (src.reg_type != RSX_VP_REGISTER_TYPE_TEMP)
				{
					return src;
				}

				const u32 writer = prog.last_temp_writer(src.index, before);
				if (writer == umax)
				{
					return src;
				}

				const decoded_instr& in = prog[writer];
				if (in.d1.vec_opcode != RSX_VEC_OPCODE_MOV || vec_writemask(in) != 0xf)
				{
					return src;
				}

				src.reg_type = in.src[0].reg_type;
				src.index = (src.reg_type == RSX_VP_REGISTER_TYPE_INPUT) ? u32{in.d1.input_src} : u32{in.src[0].tmp_src};
				before = writer;
			}

			return src;
		}

		// 4 x DP4/DPH, one per component of the target, c[base+i] feeding component i.
		bool match_dp4_chain(const program_walker& prog, const std::vector<writer_ref>& writers, chain_result& out)
		{
			if (writers.size() != 4)
			{
				return false;
			}

			u32 consts[4] = { umax, umax, umax, umax };
			chain_source source{};
			bool have_source = false;
			u32 first = umax;

			for (const writer_ref& w : writers)
			{
				const decoded_instr& in = prog[w.instr];

				if (in.d1.vec_opcode != RSX_VEC_OPCODE_DP4 && in.d1.vec_opcode != RSX_VEC_OPCODE_DPH)
				{
					return false;
				}

				if (in.d3.index_const)
				{
					return false;
				}

				// 'component' is the component of the *target* this row lands on, which after
				// resolution need not be the component the instruction itself writes. A matrix row
				// is still one component wide either way.
				u32 component = 0;
				u32 row = 0;
				if (!single_component(w.mask, component) || !single_component(vec_writemask(in), row) || consts[component] != umax)
				{
					return false;
				}

				first = std::min(first, w.instr);

				// Exactly one of the two consumed sources must be a constant.
				u32 const_slots = 0;
				chain_source other{};

				for (u32 s = 0; s < 2; ++s)
				{
					if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
					{
						++const_slots;
					}
					else
					{
						other.reg_type = in.src[s].reg_type;
						other.index = (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_INPUT) ? u32{in.d1.input_src} : u32{in.src[s].tmp_src};
					}
				}

				if (const_slots != 1)
				{
					return false;
				}

				if (!have_source)
				{
					source = other;
					have_source = true;
				}
				else if (!(source == other))
				{
					return false;
				}

				consts[component] = in.d1.const_src;
			}

			for (u32 c = 1; c < 4; ++c)
			{
				if (consts[c] != consts[0] + c)
				{
					return false;
				}
			}

			out.found = true;
			out.shape = chain_shape::dp4;
			out.base = consts[0];
			out.source = source;
			out.instructions = 4;
			out.first_instruction = first;
			return true;
		}

		// 2 x DP4/DPH writing HPOS.x and HPOS.y from one constant each, both reading the same
		// source. A 2D ortho: z and w are a fixed depth written separately, so the program never
		// has the four consecutive slots match_dp4_chain demands and would otherwise be declared
		// screen_space with its transform discarded (RemixTransforms.h, vp_fingerprint::has_ortho2d).
		// The slots need not be adjacent - only x and y are read, and each is read by name.
		bool match_ortho2d(const program_walker& prog, vp_fingerprint& out)
		{
			u32 slots[2] = { umax, umax };
			chain_source source{};
			bool have_source = false;

			for (u32 i = 0; i < static_cast<u32>(prog.size()); ++i)
			{
				const decoded_instr& in = prog[i];

				if (!vec_writes_output(in, 0))
				{
					continue;
				}

				const u32 mask = vec_writemask(in);

				// z/w only - the constant depth. Not part of the 2D transform.
				if ((mask & 0x3) == 0)
				{
					continue;
				}

				// One instruction writing both x and y cannot be two matrix rows.
				u32 component = 0;
				if (!single_component(mask, component) || component > 1)
				{
					return false;
				}

				if (in.d3.index_const || slots[component] != umax)
				{
					return false;
				}

				if (in.d1.vec_opcode != RSX_VEC_OPCODE_DP4 && in.d1.vec_opcode != RSX_VEC_OPCODE_DPH)
				{
					return false;
				}

				u32 const_slots = 0;
				chain_source other{};

				for (u32 s = 0; s < 2; ++s)
				{
					if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
					{
						++const_slots;
					}
					else
					{
						other.reg_type = in.src[s].reg_type;
						other.index = (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_INPUT) ? u32{in.d1.input_src} : u32{in.src[s].tmp_src};
					}
				}

				if (const_slots != 1)
				{
					return false;
				}

				if (!have_source)
				{
					source = other;
					have_source = true;
				}
				else if (!(source == other))
				{
					return false;
				}

				slots[component] = in.d1.const_src;
			}

			if (slots[0] == umax || slots[1] == umax)
			{
				return false;
			}

			out.has_ortho2d = true;
			out.ortho2d_slot_x = slots[0];
			out.ortho2d_slot_y = slots[1];
			return true;
		}

		// MUL + MAD/ADD accumulation through a temp: c[base+k] scaled by pos_k.
		// With 'allow_indexed' the four constant reads may go through the address register, in
		// which case the group is a bone palette; all four must agree on the same register and
		// component, and a partially indexed group is rejected outright.
		//
		// 'relaxed' drops two assumptions that only ever held for the fused world matrices this
		// started on, and which a bone palette has no reason to satisfy:
		//   - the final value writes all four components. A palette row set that only produces xyz
		//     (the w column being an implicit 0,0,0,1) writes 0x7 and was thrown away.
		//   - the four slots are consecutive. A palette laid out with a stride reads
		//     c[base + k*stride], not c[base + k].
		// Both are the same class of defect as the ADD src2 operand scan, the 2D ortho's three
		// constants and the HPOS write parked in a register: a structural assumption about how a
		// transform is written rejecting a legitimate one. Haze's world programs are recorded as
		// three rows of .xyz at stride 2 (c27, c29), which fails on both axes at once and is why
		// build_skinning had never executed on any title.
		//
		// The relaxed pass is only ever reached from the strict one's failure, and its result is
		// only accepted when the group came back indexed - so every non-indexed program in every
		// title matches exactly the slots it matched at ae94587, where world_applied was 1412430
		// of 1412430 submitted draws.
		//
		// Unlike match_indexed_affine below, this half is NOT replayed against a capture: the Haze
		// shape is on record from an earlier session, not from any dump available here, and no
		// Resistance 2 program uses the MAD form for its palette at all (all 36 of its indexed
		// programs read DP4s). So the stride and the 3-step termination are built to the recorded
		// description and the first Haze run is what confirms or refutes them. What bounds the risk
		// is that this population was refused outright before, and that build_skinning still proves
		// every bone it builds is a finite affine transform - the failure mode available here is a
		// rig placed wrongly, not one exploded across the map.
		bool match_mad_chain_pass(const program_walker& prog, const std::vector<writer_ref>& writers, chain_result& out, bool allow_indexed, bool relaxed)
		{
			if (writers.empty())
			{
				return false;
			}

			// The instruction that produces the final value writes every component - or, relaxed,
			// the three that carry a position.
			u32 cursor = umax;
			u32 terminal_mask = 0xf;

			for (auto it = writers.rbegin(); it != writers.rend(); ++it)
			{
				const u32 mask = vec_writemask(prog[it->instr]);

				if (it->mask != mask)
				{
					continue;
				}

				if (mask == 0xf || (relaxed && mask == 0x7))
				{
					cursor = it->instr;
					terminal_mask = mask;
					break;
				}
			}

			if (cursor == umax)
			{
				return false;
			}

			chain_source source{};
			bool have_source = false;

			// (constant slot, position component) for each of the four steps. The compiler is
			// free to emit them in any order, so the pairing is recovered from the broadcast
			// swizzle rather than from the order.
			u32 chain_consts[4] = { umax, umax, umax, umax };
			u32 chain_components[4] = { umax, umax, umax, umax };

			u32 indexed_steps = 0;
			u32 addr_reg = umax;
			u32 addr_swz = umax;
			u32 steps_used = 4;

			for (u32 step = 0; step < 4; ++step)
			{
				const decoded_instr& in = prog[cursor];
				const u32 opcode = in.d1.vec_opcode;

				if (in.d3.index_const)
				{
					if (!allow_indexed)
					{
						return false;
					}

					const u32 reg = u32{in.d0.addr_reg_sel_1};
					const u32 swz = u32{in.d0.addr_swz};

					if (addr_reg == umax)
					{
						addr_reg = reg;
						addr_swz = swz;
					}
					else if (addr_reg != reg || addr_swz != swz)
					{
						// Two different address registers inside one group is not a palette
						// this code understands.
						return false;
					}

					++indexed_steps;
				}

				// The walk runs backwards, so the terminal step is the MUL/MOV that started the
				// accumulation. Strictly that is always the fourth; relaxed, a group with no
				// translation column reaches it on the third, and stopping there is the only way a
				// 3-row palette can be matched at all.
				const bool terminal = (step == 3)
					|| (relaxed && step >= 2 && (opcode == RSX_VEC_OPCODE_MUL || opcode == RSX_VEC_OPCODE_MOV));

				if (terminal)
				{
					if (opcode != RSX_VEC_OPCODE_MUL && opcode != RSX_VEC_OPCODE_MOV)
					{
						return false;
					}

					steps_used = step + 1;
				}
				else if (opcode != RSX_VEC_OPCODE_MAD && opcode != RSX_VEC_OPCODE_ADD)
				{
					return false;
				}

				// Find the constant operand and, for the scaled forms, the position operand.
				// MUL/MAD multiply src0 by src1 and (MAD) accumulate src2, so their two operands
				// are 0 and 1. ADD consumes src0 and src2 - there is no src1 - so its constant is
				// as likely to sit in src2 as in src0, and Haze's world programs write exactly
				// that: 'ADD o0, r3, c[3]'. Scanning only slots 0 and 1 missed the translation
				// step of every one of them and threw the whole matrix away.
				u32 const_slot = umax;
				u32 scale_slot = umax;
				const u32 mask = vec_source_mask(opcode);
				const bool is_add = (opcode == RSX_VEC_OPCODE_ADD);
				const u32 operand_slots[2] = { 0u, is_add ? 2u : 1u };

				for (const u32 s : operand_slots)
				{
					if (!(mask & (1u << s)))
					{
						continue;
					}

					if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
					{
						if (const_slot != umax)
						{
							return false;
						}

						const_slot = s;
					}
					else
					{
						scale_slot = s;
					}
				}

				if (const_slot == umax)
				{
					return false;
				}

				chain_consts[step] = in.d1.const_src;

				// MAD/MUL carry a broadcast position component; ADD implies a component of 1,
				// i.e. the translation column.
				if (opcode == RSX_VEC_OPCODE_MAD || opcode == RSX_VEC_OPCODE_MUL)
				{
					if (scale_slot == umax)
					{
						return false;
					}

					u32 component = 0;
					if (!is_broadcast_swizzle(in.src[scale_slot], component))
					{
						return false;
					}

					chain_components[step] = component;

					chain_source other{};
					other.reg_type = in.src[scale_slot].reg_type;
					other.index = (other.reg_type == RSX_VP_REGISTER_TYPE_INPUT) ? u32{in.d1.input_src} : u32{in.src[scale_slot].tmp_src};

					if (!have_source)
					{
						source = other;
						have_source = true;
					}
					else if (!(source == other))
					{
						return false;
					}
				}
				else
				{
					// The implicit 1 multiplies the w column.
					chain_components[step] = 3;
				}

				if (terminal)
				{
					break;
				}

				// Follow the accumulator into the previous step. It is whichever consumed slot
				// the constant did not take: src2 for MAD, and for ADD the other of {0, 2}.
				const u32 accumulator_slot = is_add ? ((const_slot == 0) ? 2u : 0u) : 2u;

				if (in.src[accumulator_slot].reg_type != RSX_VP_REGISTER_TYPE_TEMP)
				{
					return false;
				}

				const u32 prev = prog.last_temp_writer(in.src[accumulator_slot].tmp_src, cursor);
				if (prev == umax)
				{
					return false;
				}

				cursor = prev;
			}

			if (!have_source)
			{
				return false;
			}

			// The constants must be one group, with c[base + k*stride] scaling position component k.
			// Strictly stride is 1 - which is what every fused world matrix seen so far uses - and
			// relaxed it is recovered from the data and then required to be uniform, so a set of
			// slots that merely happens to be a permutation still fails.
			u32 base = chain_consts[0];
			for (u32 step = 1; step < steps_used; ++step)
			{
				if (chain_consts[step] == umax)
				{
					return false;
				}

				base = std::min(base, chain_consts[step]);
			}

			u32 stride = 1;

			if (relaxed)
			{
				// The step that scales component 1 sits exactly one stride above the base, so it
				// names the stride outright. A group with no such step cannot have its layout
				// proven and is refused rather than assumed contiguous.
				stride = umax;

				for (u32 step = 0; step < steps_used; ++step)
				{
					if (chain_components[step] == 1)
					{
						stride = chain_consts[step] - base;
						break;
					}
				}

				if (stride == umax || stride == 0 || stride > 4)
				{
					return false;
				}
			}

			bool seen[4] = { false, false, false, false };

			for (u32 step = 0; step < steps_used; ++step)
			{
				const u32 component = chain_components[step];

				if (component > 3 || seen[component] || chain_consts[step] != (base + (component * stride)))
				{
					return false;
				}

				seen[component] = true;
			}

			// Components 0..steps_used-1 in order: a group that supplies x and z but not y is not a
			// matrix whichever way its slots are laid out.
			for (u32 k = 0; k < steps_used; ++k)
			{
				if (!seen[k])
				{
					return false;
				}
			}

			// All of them or none. A half-indexed group would mean the rows do not come from one
			// matrix, and guessing there is exactly how meshes end up at the origin.
			if (indexed_steps != 0 && indexed_steps != steps_used)
			{
				return false;
			}

			out.found = true;
			out.shape = chain_shape::mad;
			out.base = base;
			out.source = source;
			out.instructions = steps_used;
			out.first_instruction = cursor;
			out.indexed = (indexed_steps == steps_used);
			out.addr_reg = (addr_reg == umax) ? 0 : addr_reg;
			out.addr_swz = (addr_swz == umax) ? 0 : addr_swz;
			out.rows = steps_used;
			out.stride = stride;
			out.xyz_only = (terminal_mask == 0x7);
			return true;
		}

		// Strict first, then - for an indexed group only - the relaxed layout. Splitting the two
		// passes is what keeps every already-matching program matching the same slots: the relaxed
		// result is discarded unless it came back indexed, and an indexed group is by construction
		// one this file refused to act on before.
		bool match_mad_chain(const program_walker& prog, const std::vector<writer_ref>& writers, chain_result& out, bool allow_indexed)
		{
			if (match_mad_chain_pass(prog, writers, out, allow_indexed, false))
			{
				return true;
			}

			if (!allow_indexed || !indexed_world_enabled())
			{
				out = chain_result{};
				return false;
			}

			out = chain_result{};

			if (match_mad_chain_pass(prog, writers, out, true, true) && out.indexed)
			{
				return true;
			}

			out = chain_result{};
			return false;
		}

		// True when this instruction actually consumes a constant, so d3.index_const means
		// something. The bit is only interpreted for RSX_VP_REGISTER_TYPE_CONSTANT sources
		// (VertexProgramDecompiler.cpp:128-132); on any other instruction it is noise.
		bool reads_constant(const decoded_instr& in)
		{
			const u32 mask = (in.d1.vec_opcode != RSX_VEC_OPCODE_NOP) ? vec_source_mask(in.d1.vec_opcode) : 0;

			for (u32 s = 0; s < 3; ++s)
			{
				// SCA reads src2 (the decompiler's GetSRC(2) path).
				const bool consumed = (mask & (1u << s)) != 0
					|| (s == 2 && in.d1.sca_opcode != RSX_SCA_OPCODE_NOP);

				if (consumed && in.src[s].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
				{
					return true;
				}
			}

			return false;
		}

		// Whole-ucode audit of the indexing the program does. A rig this code can express loads one
		// address register once and reads its palette through the address components the matched
		// group actually accounted for; every other shape is a rig with a contributor nobody has
		// looked at, and submitting it as if it were fully understood is what tears a character
		// apart.
		//
		// 'swz_mask' is the set of address-register components the matched group covers: bit k for
		// a0.<xyzw>[k]. A single-matrix palette passes 1 << addr_swz - one component, the behaviour
		// up to ae94587 - and a four-bone blend passes all four of the components its twelve reads
		// use. A read outside the mask is still foreign, which is what keeps a fifth contributor
		// from being silently dropped.
		struct index_audit
		{
			u32 arl_count = 0;
			u32 indexed_reads = 0;
			u32 foreign_reads = 0; // indexed reads through some other address register/component
		};

		index_audit audit_indexing(const program_walker& prog, u32 addr_reg, u32 swz_mask)
		{
			index_audit out{};

			for (u32 i = 0; i < static_cast<u32>(prog.size()); ++i)
			{
				const decoded_instr& in = prog[i];

				if (in.d1.vec_opcode == RSX_VEC_OPCODE_ARL)
				{
					++out.arl_count;
				}

				if (in.d3.index_const && reads_constant(in))
				{
					++out.indexed_reads;

					if (u32{in.d0.addr_reg_sel_1} != addr_reg || !(swz_mask & (1u << (u32{in.d0.addr_swz} & 3))))
					{
						++out.foreign_reads;
					}
				}
			}

			return out;
		}

		// Reduce one component of a temp to 'factor * attr[a][c]', where 'factor' is an integer the
		// ucode built by repeated addition rather than by multiplying a constant slot. ADD and MOV
		// only, and both operands of an ADD have to bottom out on the *same* attribute component -
		// 'attr1.w + attr1.w' is x2, 'attr1.w + attr2.w' is not a scaling of anything.
		//
		// The one shape that needs it, and the reason it exists: every Resistance 2 (NPEA00431)
		// four-bone blend program opens with
		//   2:ADD>r1.xyzw(attr1.xyzw, attr1.xyzw)      r1 = 2 * attr1
		//   6:ADD>r1.xyzw(attr1.xyzw, r1.xyzw)         r1 = 3 * attr1
		//   8:ARL>a0.xyzw(r1.wzxy)
		// and 3 is the row stride of the c32/c33/c34 palette those programs read - which is the
		// independent confirmation that the three bases really are one three-row group. The generic
		// walk below refuses both ADDs ("bone index ADD has no constant bias") because neither
		// addend is a constant, so without this the index resolves to bone/3.
		bool reduce_integer_multiple(const program_walker& prog, chain_source src, u32 component, u32 before,
			u32 depth, chain_source& out_src, u32& out_component, f32& out_factor)
		{
			if (src.reg_type == RSX_VP_REGISTER_TYPE_INPUT)
			{
				out_src = src;
				out_component = component;
				out_factor = 1.f;
				return true;
			}

			if (src.reg_type != RSX_VP_REGISTER_TYPE_TEMP || depth >= max_bone_index_ops)
			{
				return false;
			}

			// last_component_writer rather than last_temp_writer, because it is the one that also
			// sees the SCA half. A co-issued scalar write landing on this component between the two
			// ADDs would make the multiple something other than what this computes, and a wrong
			// multiple is a wrong palette entry - the same class of bug as the unread ARL modifiers,
			// which drew every Resistance 2 character as a line. Refused rather than assumed away.
			bool from_sca = false;
			const u32 writer = prog.last_component_writer(src.index, component, before, from_sca);

			if (writer == umax || from_sca)
			{
				return false;
			}

			const decoded_instr& in = prog[writer];

			// Same rule the generic walk applies one hop out: an operand modifier this does not
			// replay is refused, never ignored. The thin-diagonal-box bug was exactly the cost of
			// evaluating an index the hardware computes differently.
			if (in.d3.index_const || !(vec_writemask(in) & (1u << component))
				|| in.src[0].neg || in.src[1].neg || in.src[2].neg
				|| in.d0.src0_abs || in.d0.src1_abs || in.d0.src2_abs
				|| in.d0.staturate || (in.d0.cond_test_enable && in.d0.cond != 0x7))
			{
				return false;
			}

			const auto operand = [&](u32 slot) -> chain_source
			{
				chain_source next{};
				next.reg_type = in.src[slot].reg_type;
				next.index = (next.reg_type == RSX_VP_REGISTER_TYPE_INPUT) ? u32{in.d1.input_src} : u32{in.src[slot].tmp_src};
				return next;
			};

			if (in.d1.vec_opcode == RSX_VEC_OPCODE_MOV)
			{
				return reduce_integer_multiple(prog, operand(0), swizzle_component(in.src[0], component),
					writer, depth + 1, out_src, out_component, out_factor);
			}

			if (in.d1.vec_opcode != RSX_VEC_OPCODE_ADD)
			{
				return false;
			}

			// ADD is '$0 + $2' (vec_source_mask(ADD) is 0b101); src1 is not consumed.
			chain_source lhs_src{};
			chain_source rhs_src{};
			u32 lhs_component = 0;
			u32 rhs_component = 0;
			f32 lhs_factor = 0.f;
			f32 rhs_factor = 0.f;

			if (!reduce_integer_multiple(prog, operand(0), swizzle_component(in.src[0], component),
					writer, depth + 1, lhs_src, lhs_component, lhs_factor)
				|| !reduce_integer_multiple(prog, operand(2), swizzle_component(in.src[2], component),
					writer, depth + 1, rhs_src, rhs_component, rhs_factor))
			{
				return false;
			}

			if (!(lhs_src == rhs_src) || lhs_component != rhs_component)
			{
				return false;
			}

			out_src = lhs_src;
			out_component = lhs_component;
			out_factor = lhs_factor + rhs_factor;
			return true;
		}

		// Follows the address register back to the vertex attribute that produced it, recording
		// the affine steps applied on the way. Anything it cannot follow comes back false, and
		// the caller then skips the draw rather than drawing it at identity.
		//
		// 'out_chain', when given, receives the resolved path instead of the fingerprint's own
		// single-bone fields - that is how the four bones of a blend rig are resolved through the
		// one walk rather than through four copies of it. 'out.skin_note' still carries the reason
		// on failure either way, because there is only one of those per program.
		bool resolve_bone_index(const program_walker& prog, u32 addr_reg, u32 addr_swz, u32 before, vp_fingerprint& out,
			bone_index_chain* out_chain = nullptr)
		{
			// The ARL that last loaded this address register. ARL is a VEC opcode writing
			// d0.dst_tmp; only a0/a1 exist, which is why the interpreter masks with 1.
			u32 arl = umax;

			for (u32 i = 0; i < before && i < static_cast<u32>(prog.size()); ++i)
			{
				const decoded_instr& in = prog[i];

				if (in.d1.vec_opcode == RSX_VEC_OPCODE_ARL && (u32{in.d0.dst_tmp} & 1u) == addr_reg)
				{
					arl = i;
				}
			}

			if (arl == umax)
			{
				out.skin_note = "no ARL writes the address register";
				return false;
			}

			// The indexed read picks component 'addr_swz' of the address vector, so the ARL's
			// source swizzle has to be sampled at the same position.
			const decoded_instr& load = prog[arl];

			if (load.d3.index_const)
			{
				out.skin_note = "ARL source is itself indexed";
				return false;
			}

			// The ARL's own source modifiers. These are applied when the operand is *read*, before
			// the address register truncates it (VertexProgramDecompiler.cpp:151-163: abs first,
			// then negate), and reproducing them is not optional - it decides which palette entry
			// the draw reads.
			//
			// Measured on Resistance 2 (NPEA00431), the run after this recogniser first started
			// resolving: skin_submitted=1101254 with characters drawn as a thin diagonal box, and
			// the skinval line said why. vp=73ae1dc4ee69c33c fed raw attr0.w = -1 straight through,
			// giving palette_base + (-1) = c30, whose three rows are
			//   [0 0 0.97707 | 0 0 -0.040698 | 0 0 0.20897 | -4.9981 1 -6.8393]
			// - a basis whose three vectors are all parallel to Z, so every vertex collapses onto a
			// line. The same program's c32 is (0.97707, -0.040698, 0.20897), norm 1.0000, i.e. a
			// rotation row: the index the hardware used was +1, not -1. Corroborated three ways:
			// vp=67c4f95549157dbb reads raw +1 and lands on c32 with a clean rotation; the four
			// observed raw values (-1, 1, 4, -4) have magnitudes 1, 1, 4, 4, every one of them
			// congruent to 1 modulo the 3-row stride while the signed values are not, so bones
			// start at c32, c35, ... ; and 73ae1dc4ee69c33c carries an SSG on the very same
			// component right after the ARL, which is the ucode reading the packed sign separately
			// because the sign is not part of the index.
			const bool index_abs = !!load.d0.src0_abs;
			const bool index_negate = !!load.src[0].neg;

			chain_source source{};
			source.reg_type = load.src[0].reg_type;
			source.index = (source.reg_type == RSX_VP_REGISTER_TYPE_INPUT) ? u32{load.d1.input_src} : u32{load.src[0].tmp_src};

			u32 component = swizzle_component(load.src[0], addr_swz);
			u32 cursor = arl;

			// Collected from the address register inwards, so reversed at the end.
			bone_index_op ops[max_bone_index_ops]{};
			u32 op_count = 0;

			for (u32 hop = 0; hop <= max_bone_index_ops; ++hop)
			{
				if (source.reg_type == RSX_VP_REGISTER_TYPE_INPUT)
				{
					if (out_chain)
					{
						out_chain->attribute = source.index;
						out_chain->component = component;
						out_chain->op_count = op_count;
						out_chain->index_abs = index_abs;
						out_chain->index_negate = index_negate;

						for (u32 i = 0; i < op_count; ++i)
						{
							out_chain->ops[i] = ops[op_count - 1 - i];
						}

						out_chain->resolved = true;
						return true;
					}

					out.bone_attribute = source.index;
					out.bone_component = component;
					out.bone_op_count = op_count;
					out.bone_index_abs = index_abs;
					out.bone_index_negate = index_negate;

					for (u32 i = 0; i < op_count; ++i)
					{
						out.bone_ops[i] = ops[op_count - 1 - i];
					}

					out.bone_resolved = true;
					return true;
				}

				if (source.reg_type != RSX_VP_REGISTER_TYPE_TEMP)
				{
					out.skin_note = "bone index does not come from a temp or an attribute";
					return false;
				}

				// A multiple of the attribute the ucode built by repeated self-addition, folded into
				// one immediate step. Tried before the generic producer walk and only taken when the
				// factor is not 1, so a program that already resolves keeps resolving through exactly
				// the same instructions it did at ae94587.
				if (bone_blend_enabled() && op_count < max_bone_index_ops)
				{
					chain_source scaled_src{};
					u32 scaled_component = 0;
					f32 factor = 1.f;

					if (reduce_integer_multiple(prog, source, component, cursor, 0, scaled_src, scaled_component, factor)
						&& factor != 1.f && std::isfinite(factor))
					{
						bone_index_op op{};
						op.op = bone_index_op::kind::immediate_scale;
						op.immediate = factor;
						ops[op_count++] = op;

						source = scaled_src;
						component = scaled_component;
						continue;
					}
				}

				const u32 writer = prog.last_temp_writer(source.index, cursor);

				if (writer == umax)
				{
					out.skin_note = "bone index temp has no producer";
					return false;
				}

				const decoded_instr& in = prog[writer];

				if (in.d3.index_const || !(vec_writemask(in) & (1u << component)))
				{
					out.skin_note = "bone index producer does not write the component";
					return false;
				}

				if (op_count >= max_bone_index_ops)
				{
					out.skin_note = "bone index chain too long";
					return false;
				}

				// The same modifiers, on an intermediate step. This walk does not reproduce them,
				// and the thin-diagonal-box bug is exactly what happens when it evaluates an index
				// the hardware computes differently - so an unmodelled modifier is refused rather
				// than ignored a second time. No program observed on either title carries one here
				// (every rig that resolves reads the attribute straight into the ARL, bone_op_count
				// zero), so this refuses nothing that works today.
				// (is_conditional is defined further down; cond == lt|gt|eq is "always".)
				if (in.src[0].neg || in.src[1].neg || in.src[2].neg
					|| in.d0.src0_abs || in.d0.src1_abs || in.d0.src2_abs
					|| in.d0.staturate || (in.d0.cond_test_enable && in.d0.cond != 0x7))
				{
					out.skin_note = "bone index step modifies its operand in a way this cannot replay";
					return false;
				}

				const u32 opcode = in.d1.vec_opcode;
				u32 next_slot = umax;

				switch (opcode)
				{
				case RSX_VEC_OPCODE_MOV:
				{
					next_slot = 0;
					break;
				}
				case RSX_VEC_OPCODE_FLR:
				{
					// ARL truncates anyway, but recording the floor keeps a negative index
					// evaluating the way the hardware would.
					bone_index_op op{};
					op.op = bone_index_op::kind::floor;
					ops[op_count++] = op;
					next_slot = 0;
					break;
				}
				case RSX_VEC_OPCODE_MUL:
				case RSX_VEC_OPCODE_MAD:
				case RSX_VEC_OPCODE_ADD:
				{
					const u32 mask = vec_source_mask(opcode);
					u32 const_slot = umax;
					u32 value_slot = umax;

					for (u32 s = 0; s < 2; ++s)
					{
						if (!(mask & (1u << s)))
						{
							continue;
						}

						if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
						{
							if (const_slot != umax)
							{
								out.skin_note = "bone index step multiplies two constants";
								return false;
							}

							const_slot = s;
						}
						else
						{
							value_slot = s;
						}
					}

					bone_index_op op{};

					if (opcode == RSX_VEC_OPCODE_ADD)
					{
						// $0 + $2, and the addend is the constant bias.
						if (in.src[2].reg_type != RSX_VP_REGISTER_TYPE_CONSTANT)
						{
							out.skin_note = "bone index ADD has no constant bias";
							return false;
						}

						op.op = bone_index_op::kind::affine;
						op.mul_slot = umax; // multiplier of 1
						op.add_slot = in.d1.const_src;
						op.add_component = static_cast<u8>(swizzle_component(in.src[2], component));
						value_slot = 0;
					}
					else
					{
						if (const_slot == umax || value_slot == umax)
						{
							out.skin_note = "bone index step has no constant factor";
							return false;
						}

						op.op = bone_index_op::kind::scale;
						op.mul_slot = in.d1.const_src;
						op.mul_component = static_cast<u8>(swizzle_component(in.src[const_slot], component));

						if (opcode == RSX_VEC_OPCODE_MAD)
						{
							// One const_src per instruction, so the bias shares the slot and
							// differs only in its swizzle.
							if (in.src[2].reg_type != RSX_VP_REGISTER_TYPE_CONSTANT)
							{
								out.skin_note = "bone index MAD addend is not a constant";
								return false;
							}

							op.op = bone_index_op::kind::affine;
							op.add_slot = in.d1.const_src;
							op.add_component = static_cast<u8>(swizzle_component(in.src[2], component));
						}
					}

					ops[op_count++] = op;
					next_slot = value_slot;
					break;
				}
				default:
					out.skin_note = "bone index producer opcode is not affine";
					return false;
				}

				const SRC& next = in.src[next_slot];
				component = swizzle_component(next, component);
				source.reg_type = next.reg_type;
				source.index = (next.reg_type == RSX_VP_REGISTER_TYPE_INPUT) ? u32{in.d1.input_src} : u32{next.tmp_src};
				cursor = writer;
			}

			out.skin_note = "bone index chain did not reach an attribute";
			return false;
		}

		struct prescale_result
		{
			bool found = false;
			u32 scale_slot = 0;
			u32 scale_component = 0;
			u32 bias_slot = 0;
		};

		// 'temp.xyz = attribute.xyz * scalar + c[K].xyz', where the scalar itself comes from a
		// constant. Quantised geometry (Minecraft's chunk meshes) is stored this way.
		bool match_prescale(const program_walker& prog, u32 temp, u32 before, prescale_result& out)
		{
			std::vector<u32> writers;
			prog.collect_vec_writers(walk_target{ false, temp }, writers, before);

			for (auto it = writers.rbegin(); it != writers.rend(); ++it)
			{
				const decoded_instr& in = prog[*it];

				if (in.d1.vec_opcode != RSX_VEC_OPCODE_MAD || (vec_writemask(in) & 0x7) != 0x7 || in.d3.index_const)
				{
					continue;
				}

				// MAD is $0 * $1 + $2, so the addend has to be the constant bias.
				if (in.src[2].reg_type != RSX_VP_REGISTER_TYPE_CONSTANT)
				{
					return false;
				}

				u32 attribute_slot = umax;
				u32 scalar_slot = umax;

				for (u32 s = 0; s < 2; ++s)
				{
					if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_INPUT)
					{
						attribute_slot = s;
					}
					else if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_TEMP)
					{
						scalar_slot = s;
					}
				}

				if (attribute_slot == umax || scalar_slot == umax)
				{
					return false;
				}

				u32 scalar_component = 0;
				if (!is_broadcast_swizzle(in.src[scalar_slot], scalar_component))
				{
					return false;
				}

				// Chase the scalar back to the constant it was copied from.
				const u32 scalar_writer = prog.last_temp_writer(in.src[scalar_slot].tmp_src, *it);
				if (scalar_writer == umax)
				{
					return false;
				}

				const decoded_instr& mov = prog[scalar_writer];

				if (mov.d1.vec_opcode != RSX_VEC_OPCODE_MOV
					|| mov.d3.index_const
					|| mov.src[0].reg_type != RSX_VP_REGISTER_TYPE_CONSTANT
					|| !(vec_writemask(mov) & (1u << scalar_component)))
				{
					return false;
				}

				const u32 mov_swizzle[4] = { mov.src[0].swz_x, mov.src[0].swz_y, mov.src[0].swz_z, mov.src[0].swz_w };

				out.found = true;
				out.scale_slot = mov.d1.const_src;
				out.scale_component = mov_swizzle[scalar_component];
				out.bias_slot = in.d1.const_src;
				return true;
			}

			return false;
		}

		struct const_affine_result
		{
			bool found = false;
			bool has_scale = false;
			bool has_bias = false;
			u32 scale_slot = 0;
			u8 scale_component[3] = { 0, 1, 2 };
			u32 bias_slot = 0;
			// Which test refused, for the world-extent census. The matcher has a dozen exits and
			// every one of them reads as 'affine=0' from outside, which is not enough to tell a
			// program whose decode is written in a shape this grammar does not cover from one that
			// has no decode at all - and those want opposite fixes. Static storage duration only
			// (string literals), copied by pointer onto the fingerprint.
			const char* reason = "ok";
		};

		// The xyz source swizzle is the identity, i.e. the operand really is this vertex's own
		// position in its own axis order rather than some permutation of it.
		bool is_identity_xyz_swizzle(const SRC& s)
		{
			return s.swz_x == 0 && s.swz_y == 1 && s.swz_z == 2;
		}

		// 'temp.xyz = attribute0.xyz * c[S].<swz> + c[B].xyz', written as separate MUL and ADD
		// instructions rather than the single MAD match_prescale looks for, and with a scale that
		// may be a different component per axis.
		//
		// Resistance 2 decodes its quantised positions this way. Two shapes appear in its dump
		// (C:\...\r2_dump3.log, 70 programs):
		//   ca526d308f1650bb  0:MUL>r0.xyz(I0.xyzx,C0.xyzx,I0.xyzw)c17i0   pos * c17.xyz
		//   67c4f95549157dbb  1:MUL>r1.xyz(I0.xyzx,C0.wwww,I0.xyzw)c18i0   pos * c18.w
		// and both leave 'r0.w = c[K].z' between the decode and the matrix, which is why this walks
		// the writer list the way match_prescale does instead of looking only at the last write.
		//
		// Everything it cannot prove is a single scale-and-bias on ATTR0 is refused: a partial xyz
		// write, a scalar that is not a transform constant, an indexed constant, a scalar half
		// touching xyz, an operand that is not read straight. R2's own indexed-palette programs
		// (fcbc61ec596e7e34, 0a8f3a6c16e60433) apply their bias *after* the palette matrix, so the
		// backward walk hits a DP4 on the way and refuses them here, which is correct - their
		// position is not attr * s + b at all.
		bool match_const_affine(const program_walker& prog, u32 temp, u32 before, const_affine_result& out)
		{
			out = const_affine_result{};

			const auto refuse = [&out](const char* why)
			{
				out.reason = why;
				return false;
			};

			// A scalar-half write into xyz is a term this walk never sees, so it has to be
			// excluded for every temp the chase visits, not just the one it starts on.
			const auto sca_touches_xyz = [&](u32 reg, u32 bound)
			{
				for (u32 i = 0; i < bound && i < static_cast<u32>(prog.size()); ++i)
				{
					if (sca_writes_temp(prog[i], reg) && (sca_writemask(prog[i]) & 0x7))
					{
						return true;
					}
				}

				return false;
			};

			if (sca_touches_xyz(temp, before))
			{
				return refuse("sca-xyz");
			}

			std::vector<u32> writers;
			prog.collect_vec_writers(walk_target{ false, temp }, writers, before);

			u32 cursor = before;

			// One MUL and one ADD is the whole grammar; the bound is hop count, not instructions.
			for (u32 hop = 0; hop < 4; ++hop)
			{
				u32 chosen = umax;

				for (auto it = writers.rbegin(); it != writers.rend(); ++it)
				{
					if (*it >= cursor)
					{
						continue;
					}

					const u32 mask = vec_writemask(prog[*it]) & 0x7;

					if (mask == 0)
					{
						// A w-only write (the 'r0.w = c[K].z' that completes the homogeneous
						// vector) does not touch the position and cannot hide a term.
						continue;
					}

					if (mask != 0x7)
					{
						// A partial xyz write means one axis is produced somewhere this walk is
						// not looking. Refuse rather than fold two thirds of a decode.
						return refuse("partial-xyz");
					}

					chosen = *it;
					break;
				}

				if (chosen == umax)
				{
					// Nothing writes this temp's xyz before the cursor. For the first hop that
					// means the chain's innermost operand was never produced by a vec instruction
					// this walk can read - the 'innermost operand is not an attribute' population.
					return refuse(hop == 0 ? "no-writer" : "no-writer-mid");
				}

				const decoded_instr& in = prog[chosen];

				if (in.d3.index_const)
				{
					return refuse("indexed-const");
				}

				switch (in.d1.vec_opcode)
				{
				case RSX_VEC_OPCODE_ADD:
				{
					// ADD is '$0 + $2' - vec_source_mask(ADD) is 0b101 and there is no src1. A
					// scan written as 's = 0..1' silently misses every real case (41d9adc).
					constexpr u32 add_slots[2] = { 0, 2 };

					u32 const_slot = umax;
					u32 other_slot = umax;

					for (const u32 s : add_slots)
					{
						if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
						{
							const_slot = (const_slot == umax) ? s : umax;
						}
						else
						{
							other_slot = s;
						}
					}

					if (const_slot == umax || other_slot == umax || out.has_bias)
					{
						return refuse(out.has_bias ? "add-twice" : "add-operands");
					}

					if (!is_identity_xyz_swizzle(in.src[const_slot]) || !is_identity_xyz_swizzle(in.src[other_slot]))
					{
						return refuse("add-swizzle");
					}

					out.has_bias = true;
					out.bias_slot = in.d1.const_src;

					if (in.src[other_slot].reg_type == RSX_VP_REGISTER_TYPE_INPUT)
					{
						// Terminal: 'attr + c[B]', no scale.
						out.found = (u32{in.d1.input_src} == 0);

						if (!out.found)
						{
							// A bias on some attribute other than ATTR0 - a decode of a vector this
							// backend never submits as a position.
							return refuse("add-not-attr0");
						}

						return true;
					}

					if (in.src[other_slot].reg_type != RSX_VP_REGISTER_TYPE_TEMP)
					{
						return refuse("add-src-not-temp");
					}

					if (sca_touches_xyz(in.src[other_slot].tmp_src, chosen))
					{
						return refuse("add-sca-xyz");
					}

					prog.collect_vec_writers(walk_target{ false, in.src[other_slot].tmp_src }, writers, chosen);
					cursor = chosen;
					break;
				}
				case RSX_VEC_OPCODE_MUL:
				{
					// MUL is '$0 * $1'.
					u32 input_slot = umax;
					u32 const_slot = umax;

					for (u32 s = 0; s < 2; ++s)
					{
						if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_INPUT)
						{
							input_slot = s;
						}
						else if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
						{
							const_slot = s;
						}
					}

					if (out.has_scale)
					{
						return refuse("mul-twice");
					}

					// Tested before the INPUT operand because it is the stronger statement: with no
					// constant operand there is no constant scale to rebuild per draw, so this
					// matcher cannot reach the program at all - it exists to fold 'attr * s + b'
					// where s and b are transform constants. Every one of R2's scale~1.0
					// vertex-explosion programs lands here. Ordering these the other way round
					// reported them as 'mul-src', which reads as "wrong operand" and sent one
					// round of work chasing a grammar widening that could never have matched them.
					if (const_slot == umax)
					{
						return refuse("mul-no-const");
					}

					if (input_slot == umax)
					{
						return refuse("mul-src");
					}

					// ATTR0 only, and read straight: it is the one attribute this backend submits
					// as a position, so a decode on any other input describes a vector we never
					// send. The same rule match_wdivide states.
					if (u32{in.d1.input_src} != 0)
					{
						return refuse("mul-not-attr0");
					}

					if (!is_identity_xyz_swizzle(in.src[input_slot]))
					{
						return refuse("mul-swizzle");
					}

					const SRC& factor = in.src[const_slot];

					out.has_scale = true;
					out.scale_slot = in.d1.const_src;
					out.scale_component[0] = static_cast<u8>(factor.swz_x);
					out.scale_component[1] = static_cast<u8>(factor.swz_y);
					out.scale_component[2] = static_cast<u8>(factor.swz_z);
					out.found = true;
					return true;
				}
				case RSX_VEC_OPCODE_MAD:
				{
					// 'attr * c[K].<swz> + temp'. R2's indexed-palette characters decode their
					// position this way:
					//     9:MAD>r1.xyz(I0.xyzx,C0.wwww,T1.xyzx)c45i0   attr * c45.w + palette
					// which is the mirror of the shape match_prescale looks for - there the scale is
					// a scalar temp and the addend is a constant, here the scale is the constant and
					// the addend is a temp - so neither matcher expressed it and the raw quantised
					// attribute reached the world untouched. Measured on ba93cfeb/f7576a48 with the
					// basis already applied: model=3937, wext=4733, scale=1.202 against a scene
					// median of 0.646, i.e. correctly oriented and placed but ~4000x too big.
					// Before this arm existed the walk reached this instruction and refused with
					// 'opcode', which is what named it.
					//
					// Only the scale is folded. A temp addend is the palette term and travels
					// separately as the indexed world, so recording it as a bias would apply it
					// twice; a constant addend is a genuine scale-and-bias and is taken.
					if (out.has_scale)
					{
						return refuse("mad-twice");
					}

					if (in.src[0].reg_type != RSX_VP_REGISTER_TYPE_INPUT)
					{
						// The placement MAD lands here. The walk does not try to cross it: the
						// basis below it is three single-component DP3 writes, which this walk
						// refuses as 'partial-xyz' by a rule worth keeping. match_basis_affine
						// parses that whole construction already and hands its input temp to
						// scan_vertex_program, which runs this matcher from there instead.
						return refuse("mad-src0-not-input");
					}

					// ATTR0 only, read straight - the same rule the MUL arm and match_wdivide state.
					if (u32{in.d1.input_src} != 0 || !is_identity_xyz_swizzle(in.src[0]))
					{
						return refuse("mad-not-attr0");
					}

					if (in.src[1].reg_type != RSX_VP_REGISTER_TYPE_CONSTANT)
					{
						return refuse("mad-scale-not-const");
					}

					if (in.src[0].neg || in.src[1].neg)
					{
						return refuse("mad-negated");
					}

					const SRC& factor = in.src[1];

					out.has_scale = true;
					out.scale_slot = in.d1.const_src;
					out.scale_component[0] = static_cast<u8>(factor.swz_x);
					out.scale_component[1] = static_cast<u8>(factor.swz_y);
					out.scale_component[2] = static_cast<u8>(factor.swz_z);

					if (in.src[2].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT
						&& is_identity_xyz_swizzle(in.src[2])
						&& !in.src[2].neg)
					{
						out.has_bias = true;
						out.bias_slot = in.d1.const_src;
					}

					out.found = true;
					return true;
				}
				default:
					// Some other vec opcode produces the position. MOV and DP4 land here, and they
					// are different problems: a DP4 means the walk has run into the matrix rather
					// than the decode.
					return refuse("opcode");
				}
			}

			return refuse("hop-limit");
		}

		// Resistance 2's indexed-palette character programs place the blended vertex with a
		// rotation basis built from two transform constants and their cross product, then a
		// uniform scale and a translation from a third:
		//
		//     out = ( dot(p, cA.xyz), dot(p, cB.xyz), dot(p, cross(cA,cB)) ) * cS.w + cS.xyz
		//
		// f7576a48e6289f83 writes it as (cA=c47, cB=c48, cS=c46):
		//     5:MUL>r1.xyz(T0.zxyz,C0.yzxy)c48      r1 = cA.zxy * cB.yzx
		//     6:MAD>r0.xyz(T0.yzxy,C0.zxyz,-T1)c48  r0 = cA.yzx * cB.zxy - r1   <- cross, note the '-'
		//    11:DP3>r0.z(T0,T1)c0                   out.z = dot(p, cross)
		//    12:DP3>r0.y(T1,C0.xyzx)c48             out.y = dot(p, cB)
		//    13:DP3>r0.x(T1,C0.xyzx)c47             out.x = dot(p, cA)
		//    14:MAD>r1.xyz(T0,C0.wwww,C0.xyzx)c46   out   = out * cS.w + cS.xyz
		// ba93cfeb is the same shape with the basis at 8-9 and the MAD at 21.
		//
		// This is what the indexed-const refusal comment means by "a third vector the ucode builds
		// from those two in a temp, then a MAD by c46.w and c46.xyz - which no matcher here
		// expresses". Every operand is a transform constant, so the whole step rebuilds per draw.
		//
		// The negate on the cross term is load-bearing and was invisible until describe_output_slice
		// learned to print SRC::neg: 'a.yzx*b.zxy + a.zxy*b.yzx' is a sum, and only the subtraction
		// is a right-handed basis. It is verified here rather than assumed.
		struct basis_affine_result
		{
			bool found = false;
			u32 row_slot[2] = { 0, 0 };
			u32 scale_slot = 0;
			u32 bias_slot = 0;
			// The vertex the basis rotates - the other operand of the x-row DP3 - and the
			// instruction it was read at. This is where the position decode lives, and handing it
			// out is what lets the decode be found without teaching the generic chain walk to
			// cross a basis it cannot represent.
			u32 position_tmp = s_no_temp;
			u32 position_before = 0;
			const char* reason = "untried";
		};

		bool match_basis_affine(const program_walker& prog, basis_affine_result& out)
		{
			out = basis_affine_result{};

			const auto refuse = [&out](const char* why)
			{
				out.reason = why;
				return false;
			};

			const auto is_broadcast_w = [](const SRC& s)
			{
				return s.swz_x == 3 && s.swz_y == 3 && s.swz_z == 3 && s.swz_w == 3;
			};

			const auto is_xyz = [](const SRC& s)
			{
				return s.swz_x == 0 && s.swz_y == 1 && s.swz_z == 2;
			};

			// 1. The terminal 'temp * cS.w + cS.xyz'. One const slot per instruction, so both
			//    operands necessarily name the same slot - which is what makes this signature
			//    tight enough to search for directly instead of walking to it.
			u32 mad_at = umax;
			u32 rotated_tmp = s_no_temp;

			for (u32 i = 0; i < static_cast<u32>(prog.size()); ++i)
			{
				const decoded_instr& in = prog[i];

				if (in.d1.vec_opcode != RSX_VEC_OPCODE_MAD || vec_writemask(in) != 0x7 || in.d3.index_const)
				{
					continue;
				}

				if (in.src[0].reg_type != RSX_VP_REGISTER_TYPE_TEMP
					|| in.src[1].reg_type != RSX_VP_REGISTER_TYPE_CONSTANT
					|| in.src[2].reg_type != RSX_VP_REGISTER_TYPE_CONSTANT)
				{
					continue;
				}

				if (!is_broadcast_w(in.src[1]) || !is_xyz(in.src[2]) || !is_xyz(in.src[0]))
				{
					continue;
				}

				if (in.src[0].neg || in.src[1].neg || in.src[2].neg)
				{
					continue;
				}

				mad_at = i;
				rotated_tmp = in.src[0].tmp_src;
				out.scale_slot = in.d1.const_src;
				out.bias_slot = in.d1.const_src;
			}

			if (mad_at == umax)
			{
				return refuse("no-scale-bias-mad");
			}

			// 2. The DP3 triple that produced that temp: x and y from a constant row each, z from
			//    two temps (the position and the cross vector). Only writes before the MAD count.
			u32 cross_tmp = s_no_temp;
			bool have_row[3] = { false, false, false };

			for (u32 i = 0; i < mad_at; ++i)
			{
				const decoded_instr& in = prog[i];

				if (in.d1.vec_opcode != RSX_VEC_OPCODE_DP3 || in.d0.dst_tmp != rotated_tmp || in.d3.index_const)
				{
					continue;
				}

				const u32 mask = vec_writemask(in);

				if (mask == 0x1 || mask == 0x2)
				{
					const u32 row = (mask == 0x1) ? 0u : 1u;

					bool row_has_const = false;
					u32 rotated = s_no_temp;

					for (u32 s = 0; s < 2; ++s)
					{
						if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
						{
							row_has_const = true;
						}
						else if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_TEMP)
						{
							rotated = in.src[s].tmp_src;
						}
					}

					if (!row_has_const)
					{
						return refuse("dp3-row-not-const");
					}

					out.row_slot[row] = in.d1.const_src;
					have_row[row] = true;

					// The non-constant operand is the vertex being rotated, i.e. the palette-blended
					// position - and the thing the decode is applied to. Taken from the x row; the
					// y row reads the same temp, so either would do.
					if (row == 0 && rotated != s_no_temp)
					{
						out.position_tmp = rotated;
						out.position_before = i;
					}
				}
				else if (mask == 0x4)
				{
					if (in.src[0].reg_type != RSX_VP_REGISTER_TYPE_TEMP
						|| in.src[1].reg_type != RSX_VP_REGISTER_TYPE_TEMP)
					{
						return refuse("dp3-z-not-temps");
					}

					// Either operand may be the cross; the other is the position. The cross is the
					// one a MAD built, which step 3 decides.
					cross_tmp = in.src[0].tmp_src;
					have_row[2] = true;
				}
			}

			if (!have_row[0] || !have_row[1] || !have_row[2])
			{
				return refuse("dp3-triple-incomplete");
			}

			if (out.row_slot[0] == out.row_slot[1])
			{
				return refuse("dp3-rows-same-slot");
			}

			// 3. Prove the z row really is cross(cA,cB): a MAD into the temp the z DP3 read, over
			//    the same two row slots, with the second product negated. Without the negate this
			//    is a sum and the basis would be mirrored, so a miss here is a refusal, not a
			//    downgrade.
			for (u32 i = 0; i < mad_at; ++i)
			{
				const decoded_instr& in = prog[i];

				if (in.d1.vec_opcode != RSX_VEC_OPCODE_MAD || in.d0.dst_tmp != cross_tmp || vec_writemask(in) != 0x7)
				{
					continue;
				}

				const bool over_row_slot = (in.d1.const_src == out.row_slot[0]) || (in.d1.const_src == out.row_slot[1]);

				if (over_row_slot && in.src[2].neg && in.src[2].reg_type == RSX_VP_REGISTER_TYPE_TEMP)
				{
					out.found = true;
					out.reason = "ok";
					return true;
				}
			}

			return refuse("cross-not-proven");
		}

		// Which input attribute the ucode writes into output register 'output' xy. This is the
		// texcoord half of the match_wdivide lesson: the backend submits the raw attribute and
		// never runs the program, so guessing which attribute is the texcoord from its size and
		// type is guessing at something 'MOV>o7.xy(I0.xyxx,...)c0i<N>' states outright - and R2
		// disagrees with the guess on 13 of its 70 TEX0-writing programs: replaying this matcher
		// over that dump resolves 67 of the 70 and picks attribute 1 for 54, attribute 3 for 11 and
		// attribute 2 for 2, and the size/type scan excludes attribute 3 outright as RSX's colour
		// register.
		//
		// Refuses anything that is not a straight read of one attribute: a write assembled from
		// temps or constants (ab9725268da2ac85 writes o7 from a temp), a texture matrix applied to
		// the position (ef8f10966ce1500b writes 'DP4>o7.y(I0.xyzw,C0.xyzw,...)c2i0' - attribute 0,
		// which is the position, not a texcoord), a swizzle that is not the identity, an indexed
		// constant, a scalar-half write, or two writers that disagree.
		bool resolve_output_input(const program_walker& prog, u32 output, u32& out_attribute)
		{
			bool found = false;
			u32 attribute = 0;

			for (u32 i = 0; i < static_cast<u32>(prog.size()); ++i)
			{
				const decoded_instr& in = prog[i];

				if (sca_writes_output(in, output) && (sca_writemask(in) & 0x3))
				{
					return false;
				}

				if (!vec_writes_output(in, output))
				{
					continue;
				}

				const u32 mask = vec_writemask(in) & 0x3;

				if (mask == 0)
				{
					// Writes only z/w. R2 packs a second coordinate set, a fog factor or a
					// scroll term there ('MOV>o7.zw(T0.xxxy,...)'); only xy is the 2D texcoord.
					continue;
				}

				if (in.d3.index_const)
				{
					return false;
				}

				const u32 sources = vec_source_mask(in.d1.vec_opcode);
				u32 input_slot = umax;

				// ADD's mask is 0b101 - src0 and src2, no src1. Walking 0..1 misses it (41d9adc).
				for (u32 s = 0; s < 3; ++s)
				{
					if ((sources & (1u << s)) && in.src[s].reg_type == RSX_VP_REGISTER_TYPE_INPUT)
					{
						input_slot = s;
					}
				}

				if (input_slot == umax)
				{
					return false;
				}

				// One input register index per instruction, so the operand's components have to
				// line up with the ones being written for the attribute to mean what it says.
				const SRC& src = in.src[input_slot];

				if (((mask & 1) && src.swz_x != 0) || ((mask & 2) && src.swz_y != 1))
				{
					return false;
				}

				const u32 index = u32{in.d1.input_src};

				if (found && index != attribute)
				{
					return false;
				}

				found = true;
				attribute = index;
			}

			// Attribute 0 is the position. A program that texture-maps from it is doing texgen,
			// which this backend does not evaluate; the heuristic scan is a better answer than a
			// stream of vertex positions handed to a sampler.
			if (!found || attribute == 0 || attribute > 15)
			{
				return false;
			}

			out_attribute = attribute;
			return true;
		}

		// Defined below, next to the position slicing it was written for.
		void slice_position(const program_walker& prog, u32 output_index, u32& out_distinct_consts,
			u32& out_instructions, bool& out_indexed, std::vector<u32>* out_indices);

		// The constant slot a program scales its texcoord attribute by on the way to TEXn.
		//
		// resolve_output_input above refuses any program whose TEXn write is not a straight read of
		// an attribute, and a scaled texcoord is exactly that shape - so the heuristic scan picks
		// the attribute up raw and the scale is silently dropped. On Haze (BLUS30094) 25 of 43
		// TEX0-writing programs are refused and 17 carry one motif:
		//
		//     MUL>rN.xy(I0.xyxx, C0.xxxx, ...)c151i8
		//
		// attr8 times slot 151, routed through temps into o7. Measured live, c151 holds 1/32768 on
		// most of those programs, ~1/4094 on some and 1 on others, while UVINTSCALE hardcodes 4096:
		// the 1/32768 population comes out 8x too large, and the uv census reads u=[0..8.000] and
		// [0.008..7.862] against a clean [0..1] elsewhere. 32768/4096 = 8.
		//
		// This does not attempt the full dataflow - it does not need to. slice_position already
		// gives the backward slice of the output, so every instruction it returns contributes to
		// TEXn by construction, and a MUL of an attribute by a constant inside that slice *is* the
		// scale. Downstream biases and swizzles are still not replicated; the scale is the term that
		// puts coordinates off by an order of magnitude, which is what tiles the texture.
		//
		// Refuses on ambiguity rather than guessing: two MULs naming different slots, or naming
		// different attributes, and the caller keeps the old fixed divisor.
		bool resolve_texcoord_scale_slot(const program_walker& prog, u32 output, u32& out_attribute, u32& out_slot)
		{
			u32 consts = 0;
			u32 instructions = 0;
			bool indexed = false;
			std::vector<u32> indices;

			slice_position(prog, output, consts, instructions, indexed, &indices);

			if (instructions == 0 || indexed)
			{
				return false;
			}

			bool found = false;
			u32 slot = 0;
			u32 attribute = 0;

			// The one attribute the whole slice reads, if it reads exactly one. Collected on the
			// same walk so the temp-form pass below costs no extra traversal. A slice that touches
			// two attributes cannot say which one a 'temp * constant' scales, and is refused.
			u32 slice_attribute = umax;
			bool slice_attribute_ambiguous = false;

			// 'temp.xy = temp.xy * cN' - the same scale with the attribute already MOVed into a
			// register. Kept separate from the direct form and only consulted when the direct form
			// finds nothing, so a program that states the scale outright is never second-guessed.
			bool temp_found = false;
			u32 temp_slot = 0;
			bool temp_ambiguous = false;

			for (const u32 i : indices)
			{
				const decoded_instr& in = prog[i];

				// Which attribute this instruction reads, if any. One input register per
				// instruction on RSX, so d1.input_src is only meaningful when a source names it.
				for (u32 s = 0; s < 3; ++s)
				{
					if (!(vec_source_mask(in.d1.vec_opcode) & (1u << s))
						|| in.src[s].reg_type != RSX_VP_REGISTER_TYPE_INPUT)
					{
						continue;
					}

					const u32 read = u32{in.d1.input_src};

					// Position feeding a texcoord slice is texgen, not a UV set, and must not
					// become the attribute this scale is attributed to.
					if (read == 0 || read > 15)
					{
						break;
					}

					if (slice_attribute != umax && slice_attribute != read)
					{
						slice_attribute_ambiguous = true;
					}

					slice_attribute = read;
					break;
				}

				// MAD is the third form, and the one carrying most of this title's texcoords:
				// 'MAD o7.xy(attr, cScale, cBias)' states scale and bias in one instruction, so a
				// program that biases its UVs at all never emits the bare MUL both passes above
				// were looking for. Measured at 58% of S32K draws falling back to the fixed
				// divisor in the later areas against 21% in the first, which is the same content
				// drawn by programs that happen to bias.
				const bool is_mad = in.d1.vec_opcode == RSX_VEC_OPCODE_MAD
					&& texcoord_scale_mad_form();

				if ((in.d1.vec_opcode != RSX_VEC_OPCODE_MUL && !is_mad) || in.d3.index_const)
				{
					continue;
				}

				// Only xy matters: z/w on a texcoord output carry a second set or a fog term.
				if ((vec_writemask(in) & 0x3) == 0)
				{
					continue;
				}

				const u32 sources = vec_source_mask(in.d1.vec_opcode);
				u32 input_slot = umax;
				u32 const_slot = umax;

				// MAD computes src0 * src1 + src2. Only the product is the scale; src2 is the
				// bias and is itself typically a constant, so scanning all three sources would
				// find two constants and refuse the program as ambiguous - turning the form this
				// is meant to recognise into a guaranteed refusal. The bias is not reproduced,
				// for the reason the header already gives: a bias shifts the coordinate, a wrong
				// scale tiles the texture, and only the second is what this exists to fix.
				const u32 scan = is_mad ? 2u : 3u;

				for (u32 s = 0; s < scan; ++s)
				{
					if (!(sources & (1u << s)))
					{
						continue;
					}

					if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_INPUT)
					{
						input_slot = s;
					}
					else if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
					{
						const_slot = s;
					}
				}

				if (const_slot == umax)
				{
					continue;
				}

				if (input_slot == umax)
				{
					// No attribute in this MUL, but it is still a multiply by a constant inside the
					// backward slice of TEXn, so it contributes to the coordinate by construction -
					// the same argument the direct form rests on. The only thing it cannot supply is
					// which attribute it scales, which is what slice_attribute is for.
					//
					// This is the form the strict pass was dropping. resolve_output_input already
					// refuses a program whose TEXn write is not a straight attribute read, so the
					// scaled programs were meant to be caught here; requiring the MUL's operand to
					// be the input register itself missed every program that MOVs the attribute into
					// a temp first, and those fell back to the fixed divisor.
					for (u32 s = 0; s < 3; ++s)
					{
						if ((sources & (1u << s)) && s != const_slot
							&& in.src[s].reg_type != RSX_VP_REGISTER_TYPE_TEMP)
						{
							// Multiplied by something that is neither a temp nor the constant -
							// not the shape this recognises.
							temp_ambiguous = true;
						}
					}

					if (temp_found && temp_slot != u32{in.d1.const_src})
					{
						temp_ambiguous = true;
					}

					temp_found = true;
					temp_slot = u32{in.d1.const_src};
					continue;
				}

				const u32 index = u32{in.d1.input_src};

				// Attribute 0 is the position; a texcoord scaled off it is texgen, not a UV.
				if (index == 0 || index > 15)
				{
					continue;
				}

				if (found && (slot != u32{in.d1.const_src} || attribute != index))
				{
					return false;
				}

				found = true;
				slot = u32{in.d1.const_src};
				attribute = index;
			}

			if (!found)
			{
				// Second chance on the temp form, under its own knob so the two populations can be
				// separated in one run. Every condition below has to hold: exactly one constant
				// slot across the slice's MULs, nothing multiplied by a third operand kind, and
				// exactly one attribute read anywhere in the slice. Any ambiguity keeps the fixed
				// divisor, which is the old behaviour and a known-safe answer.
				if (!texcoord_scale_temp_form() || !temp_found || temp_ambiguous
					|| slice_attribute_ambiguous || slice_attribute == umax)
				{
					return false;
				}

				out_attribute = slice_attribute;
				out_slot = temp_slot;
				return true;
			}

			out_attribute = attribute;
			out_slot = slot;
			return true;
		}

		struct wdivide_result
		{
			bool found = false;
			u32 attribute = 0;
		};

		// 'temp.xyz = attribute.xyz * RCP(attribute.w)': a per-vertex divide applied before the
		// first matrix.
		//
		// Haze stores positions as four 16-bit integers in which w is that vertex's own divisor,
		// and undoes the packing in the ucode. This backend submits the *stored* attribute, so
		// unless the divide is reproduced on the CPU every vertex is left displaced along its own
		// ray by its own factor - a mesh blown apart from the inside, which is exactly the
		// symptom. Unlike match_prescale's constant scale it cannot be folded into the world
		// matrix, because the factor differs per vertex; the divide has to happen at decode time.
		bool match_wdivide(const program_walker& prog, u32 temp, u32 before, wdivide_result& out)
		{
			const u32 writer = prog.last_temp_writer(temp, before);

			if (writer == umax)
			{
				return false;
			}

			const decoded_instr& in = prog[writer];

			if (in.d1.vec_opcode != RSX_VEC_OPCODE_MUL || (vec_writemask(in) & 0x7) != 0x7 || in.d3.index_const)
			{
				return false;
			}

			u32 attribute_slot = umax;
			u32 scalar_slot = umax;

			for (u32 s = 0; s < 2; ++s)
			{
				if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_INPUT)
				{
					attribute_slot = s;
				}
				else if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_TEMP)
				{
					scalar_slot = s;
				}
			}

			if (attribute_slot == umax || scalar_slot == umax)
			{
				return false;
			}

			// The attribute has to be read straight: any other swizzle means the operand is not
			// this vertex's position in its own axis order.
			const SRC& attr = in.src[attribute_slot];

			if (attr.swz_x != 0 || attr.swz_y != 1 || attr.swz_z != 2)
			{
				return false;
			}

			u32 component = 0;
			if (!is_broadcast_swizzle(in.src[scalar_slot], component))
			{
				return false;
			}

			const u32 attribute = u32{in.d1.input_src};
			const u32 scalar_temp = u32{in.src[scalar_slot].tmp_src};

			// The scalar must be the reciprocal of the *same* attribute's w, taken from the last
			// write of that one component. A VEC write landing there first means the value is
			// something else and the match is off.
			for (u32 i = writer; i-- > 0;)
			{
				const decoded_instr& prev = prog[i];

				if (vec_writes_temp(prev, scalar_temp) && (vec_writemask(prev) & (1u << component)))
				{
					return false;
				}

				if (!sca_writes_temp(prev, scalar_temp) || !(sca_writemask(prev) & (1u << component)))
				{
					continue;
				}

				if (prev.d1.sca_opcode != RSX_SCA_OPCODE_RCP || prev.d3.index_const)
				{
					return false;
				}

				// The SCA half reads src2.
				const SRC& divisor = prev.src[2];
				u32 divisor_component = 0;

				if (divisor.reg_type != RSX_VP_REGISTER_TYPE_INPUT
					|| u32{prev.d1.input_src} != attribute
					|| !is_broadcast_swizzle(divisor, divisor_component)
					|| divisor_component != 3)
				{
					return false;
				}

				out.found = true;
				out.attribute = attribute;
				return true;
			}

			return false;
		}

		// A write only happens when its condition holds, so which instruction last defined a
		// register stops being decidable from position alone. cond == lt|gt|eq is "always".
		bool is_conditional(const decoded_instr& in)
		{
			return in.d0.cond_test_enable && in.d0.cond != 0x7;
		}

		// Replace every writer that is a plain MOV out of a temp with the instruction that defined
		// the component the MOV forwards, so a matrix row that took a detour through a register
		// still reaches the matchers. Nothing else is relaxed: the resolved set still has to agree
		// on one source and four consecutive slots.
		//
		// Refuses rather than guesses. The definition has to be the *first* write of that component
		// found walking back from the MOV, it has to come from the VEC half, and neither it nor the
		// MOV may be conditional; the MOV itself may not negate, take an absolute value or saturate,
		// because all three change the row it forwards.
		bool resolve_writers(const program_walker& prog, const std::vector<u32>& writers, std::vector<writer_ref>& out, bool& out_refused)
		{
			out.clear();

			bool resolved_any = false;

			for (const u32 i : writers)
			{
				const decoded_instr& in = prog[i];
				const u32 mask = vec_writemask(in);

				if (in.d1.vec_opcode != RSX_VEC_OPCODE_MOV
					|| in.src[0].reg_type != RSX_VP_REGISTER_TYPE_TEMP)
				{
					out.push_back(writer_ref{ i, mask });
					continue;
				}

				if (in.src[0].neg || in.d0.src0_abs || in.d0.staturate || is_conditional(in))
				{
					out_refused = true;
					return false;
				}

				const u32 tmp = u32{in.src[0].tmp_src};

				for (u32 c = 0; c < 4; ++c)
				{
					if (!(mask & (1u << c)))
					{
						continue;
					}

					bool from_sca = false;
					const u32 def = prog.last_component_writer(tmp, swizzle_component(in.src[0], c), i, from_sca);

					if (def == umax || from_sca || is_conditional(prog[def]))
					{
						out_refused = true;
						return false;
					}

					// One instruction can supply several components of the target.
					auto it = std::find_if(out.begin(), out.end(), [def](const writer_ref& w) { return w.instr == def; });

					if (it == out.end())
					{
						out.push_back(writer_ref{ def, 1u << c });
					}
					else
					{
						it->mask |= 1u << c;
					}
				}

				resolved_any = true;
			}

			// Keep the list in program order: match_mad_chain reads the last full-component writer
			// off the back of it.
			std::sort(out.begin(), out.end(), [](const writer_ref& a, const writer_ref& b) { return a.instr < b.instr; });
			return resolved_any;
		}

		void prune_dead_writers(const program_walker& prog, u32 temp, u32 demand, const std::vector<writer_ref>& in, std::vector<writer_ref>& out);

		// Whether every constant this program reads through the address register resolves to the same
		// constant for every vertex. See vp_fingerprint::indexed_addr_uniform for the shape and for
		// the two Resistance 2 programs that have it - the pair match_indexed_affine's census below
		// counts as "2 index a single non-matrix row off a *constant*".
		void scan_uniform_address(const program_walker& prog, vp_fingerprint& out)
		{
			u32 arl = umax;

			for (u32 i = 0; i < static_cast<u32>(prog.size()); ++i)
			{
				if (prog[i].d1.vec_opcode != RSX_VEC_OPCODE_ARL)
				{
					continue;
				}

				if (arl != umax)
				{
					// Two ARLs are two uniform values, and which of them a given read used is a
					// question this does not answer. Refused rather than guessed.
					return;
				}

				arl = i;
			}

			if (arl == umax)
			{
				return;
			}

			const decoded_instr& load = prog[arl];

			if (load.src[0].reg_type != RSX_VP_REGISTER_TYPE_CONSTANT || load.d3.index_const || is_conditional(load))
			{
				// An address loaded from an attribute is a real per-vertex palette and belongs to the
				// skinning path; one loaded through the address register is self-referential.
				return;
			}

			u32 component = 0;

			if (!is_broadcast_swizzle(load.src[0], component))
			{
				// A per-component address vector would make the value depend on which component each
				// read selects, and the ARL's write mask on top of that.
				return;
			}

			const u32 addr_reg = u32{load.d0.dst_tmp} & 1u;
			const index_audit audit = audit_indexing(prog, addr_reg, 0xf);

			if (audit.arl_count != 1 || audit.foreign_reads != 0 || audit.indexed_reads == 0)
			{
				return;
			}

			// The span of base slots the program indexes, and the address components its reads
			// select. A component the ARL did not write reads back as the register's initial zero,
			// which is a different value than the one proven here.
			u32 lo = umax;
			u32 hi = 0;
			u32 read_swz = 0;

			for (u32 i = 0; i < static_cast<u32>(prog.size()); ++i)
			{
				const decoded_instr& in = prog[i];

				if (in.d3.index_const && reads_constant(in))
				{
					lo = std::min(lo, u32{in.d1.const_src});
					hi = std::max(hi, u32{in.d1.const_src});
					read_swz |= 1u << (u32{in.d0.addr_swz} & 3);
				}
			}

			if ((vec_writemask(load) & read_swz) != read_swz)
			{
				return;
			}

			out.indexed_addr_uniform = true;
			out.addr_const_slot = load.d1.const_src;
			out.addr_const_component = component;
			out.addr_index_abs = load.d0.src0_abs;
			out.addr_index_negate = load.src[0].neg;
			out.indexed_base_min = lo;
			out.indexed_base_max = hi;
		}

		// The object-to-world step Resistance 2 (NPEA00431) writes for every one of its indexed
		// programs, and the reason 1481510 of its draws were counted skin_unrecognised at ae94587:
		//
		//   1:MUL>r2.xyz(attr0.xyz, c18.wwww)     the quantised position decode (match_const_affine)
		//   4:ARL>a.x(attr0.wwww)                 the index
		//   5:MOV>r1.w(c0.zzzz)                   the homogeneous 1
		//   7:DP4>r1.z(c33[a], r2)  \
		//   8:DP4>r1.y(c32[a], r2)   >            three rows of a 3x4 object matrix, read indexed
		//   9:DP4>r1.x(c31[a], r2)  /
		//  13:ADD>r1.xyz(c18.xyz, r1.xyz)         a constant translation applied *after* the matrix
		//  16..19:DP4>o0.xyzw(c8..c11, r1)        the view-projection
		//
		// Neither generic matcher can express it. match_dp4_chain wants four writers, all DP4, over
		// four consecutive non-indexed slots; this has three DP4s, an indexed read, a MOV and an ADD.
		// match_mad_chain wants an accumulator. So the walk stopped at the outer c8..c11 group with
		// 'innermost operand is not an attribute', the program reported indexed_const, and the draw
		// was refused whole.
		//
		// Replayed on paper over every dumped program that reads an indexed constant in its position
		// slice (36 unique, from fp.log / d8.log / r2_dump3.log): 16 match this shape exactly, all of
		// them at base c31 with three rows, a c18 bias and a single ARL off attr0.w. The other 20 do
		// not and stay refused - 16 are four-bone blends (three accumulators over c32/c33/c34 driven
		// by four weights and four address components), 2 index a single non-matrix row off a
		// *constant*, 1 has three ARLs and branches, 1 reads two unrelated indexed slots.
		//
		// Everything it cannot account for is refused: every writer of the target has to be one of
		// the four roles below, or the whole match fails. A writer this does not understand is a term
		// applied to the position that the backend would then silently drop.
		//
		// The 16 that match write their rows into the register the translation then reads back;
		// f56d765aa4ea4cb8 writes them into another one and lets the translation move them
		// ('16:ADD>r1.xyz(c18.xyz, r0.xyz)' against 6ee02187fb587944's '17:ADD>r1.xyz(c18.xyz,
		// r1.xyz)'), which is the same transform under a different register allocation and is what
		// 'forward_temp' follows. See indexed_bias_reg_enabled for the pair side by side.
		bool match_indexed_affine(const program_walker& prog, const walk_target& target, const std::vector<writer_ref>& writers, chain_result& out, u32 depth = 0)
		{
			if (target.is_output || writers.empty())
			{
				// The bias reads the target back, so the target has to be a temp. An output register
				// is never read by a vertex program.
				return false;
			}

			u32 row_instr[4] = { umax, umax, umax, umax };
			u32 row_const[4] = { umax, umax, umax, umax };

			chain_source source{};
			bool have_source = false;

			u32 addr_reg = umax;
			u32 addr_swz = umax;

			u32 bias_instr = umax;
			u32 bias_slot = 0;

			// Set when the translation reads a temp other than the one it writes: the rows are in
			// that register, not this one.
			u32 forward_temp = umax;

			u32 w_slot = umax;
			u32 w_component = 0;

			u32 first = umax;
			u32 last_row = 0;

			for (const writer_ref& w : writers)
			{
				const decoded_instr& in = prog[w.instr];
				const u32 opcode = in.d1.vec_opcode;
				const u32 mask = vec_writemask(in);

				if (is_conditional(in) || in.d0.staturate || mask != w.mask)
				{
					// A conditional or saturated write, or a component reached through a MOV
					// (w.mask parting company with the instruction's own mask) - neither is a
					// matrix row and neither can be folded into a transform.
					return false;
				}

				// Role 1: a matrix row. One DP4/DPH into one component, its constant read through
				// the address register.
				if ((opcode == RSX_VEC_OPCODE_DP4 || opcode == RSX_VEC_OPCODE_DPH) && in.d3.index_const)
				{
					u32 component = 0;

					if (!single_component(mask, component) || row_instr[component] != umax)
					{
						return false;
					}

					const u32 reg = u32{in.d0.addr_reg_sel_1};
					const u32 swz = u32{in.d0.addr_swz};

					if (addr_reg == umax)
					{
						addr_reg = reg;
						addr_swz = swz;
					}
					else if (addr_reg != reg || addr_swz != swz)
					{
						// Two address registers inside one group is a blend rig, not a matrix.
						return false;
					}

					// Exactly one of the two consumed sources is the constant; the other is the
					// operand, and every row has to agree on it.
					u32 const_slots = 0;
					chain_source other{};

					for (u32 s = 0; s < 2; ++s)
					{
						if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
						{
							++const_slots;
						}
						else
						{
							other.reg_type = in.src[s].reg_type;
							other.index = (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_INPUT) ? u32{in.d1.input_src} : u32{in.src[s].tmp_src};
						}
					}

					if (const_slots != 1)
					{
						return false;
					}

					if (!have_source)
					{
						source = other;
						have_source = true;
					}
					else if (!(source == other))
					{
						return false;
					}

					row_instr[component] = w.instr;
					row_const[component] = in.d1.const_src;
					first = std::min(first, w.instr);
					last_row = std::max(last_row, w.instr);
					continue;
				}

				// Role 2: the homogeneous 1. A MOV of one constant component into w only - the
				// value is read back per draw and required to be 1, because a w of anything else
				// means the rows are not being applied to a point.
				if (opcode == RSX_VEC_OPCODE_MOV && mask == 0x8
					&& in.src[0].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT
					&& !in.d3.index_const && !in.src[0].neg && !in.d0.src0_abs
					&& w_slot == umax)
				{
					w_slot = in.d1.const_src;
					w_component = in.src[0].swz_w;
					continue;
				}

				// Role 3: the post-matrix translation. 'target.xyz = c[B].xyz + target.xyz', read
				// straight in both operands so the constant really is a translation in this
				// vertex's own axis order.
				if (opcode == RSX_VEC_OPCODE_ADD && mask == 0x7 && !in.d3.index_const && bias_instr == umax)
				{
					constexpr u32 add_slots[2] = { 0, 2 };

					u32 const_slot = umax;
					u32 other_slot = umax;

					for (const u32 s : add_slots)
					{
						if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
						{
							const_slot = (const_slot == umax) ? s : umax;
						}
						else
						{
							other_slot = s;
						}
					}

					if (const_slot == umax || other_slot == umax)
					{
						return false;
					}

					const SRC& k = in.src[const_slot];
					const SRC& t = in.src[other_slot];

					if (k.neg || t.neg || in.d0.src0_abs
						|| k.swz_x != 0 || k.swz_y != 1 || k.swz_z != 2
						|| t.swz_x != 0 || t.swz_y != 1 || t.swz_z != 2
						|| t.reg_type != RSX_VP_REGISTER_TYPE_TEMP)
					{
						return false;
					}

					if (u32{t.tmp_src} != target.index)
					{
						// The translation moves the rows out of the register they were built in
						// instead of translating them in place. Same transform, different register
						// allocation, so the rows are looked for in that register below - a bias
						// accumulated out of a temp that holds no rows still fails there.
						if (depth != 0 || !indexed_bias_reg_enabled())
						{
							return false;
						}

						forward_temp = t.tmp_src;
					}

					bias_instr = w.instr;
					bias_slot = in.d1.const_src;
					continue;
				}

				// Anything else writing the target is a term this cannot express.
				return false;
			}

			if (forward_temp != umax)
			{
				// Only the translation and the homogeneous 1 belong to this register; the group
				// itself is one hop back. Rows on both registers would be two halves of a matrix
				// that never existed as one.
				if (row_instr[0] != umax || have_source
					|| prog.has_sca_writer(walk_target{ false, forward_temp }, bias_instr))
				{
					// The SCA test is the one find_chain applies to every target it walks: a scalar
					// op writing the row register is a definition none of the roles below can see.
					return false;
				}

				std::vector<u32> row_writers;
				prog.collect_vec_writers(walk_target{ false, forward_temp }, row_writers, bias_instr);

				std::vector<writer_ref> refs;
				refs.reserve(row_writers.size());

				for (const u32 i : row_writers)
				{
					refs.push_back(writer_ref{ i, vec_writemask(prog[i]) });
				}

				// Only the components the translation actually reads. Resistance 2's
				// f56d765aa4ea4cb8 parks SSG(attr0.w) - the sign flag its index attribute packs -
				// in the row register's w, and refusing the whole group over a component nothing
				// downstream reads is the second half of why that program never matched.
				std::vector<writer_ref> live;
				prune_dead_writers(prog, forward_temp, 0x7, refs, live);

				chain_result inner{};

				if (live.empty() || !match_indexed_affine(prog, walk_target{ false, forward_temp }, live, inner, depth + 1))
				{
					return false;
				}

				if (inner.has_bias)
				{
					return false;
				}

				out = inner;
				out.has_bias = true;
				out.bias_slot = bias_slot;
				out.bias_forwarded = true;

				if (w_slot != umax)
				{
					out.w_slot = w_slot;
					out.w_component = w_component;
				}

				// The homogeneous 1 is written on this register rather than the row one, so the
				// three-row proof is completed here.
				return out.rows == 4 || out.w_slot != umax;
			}

			u32 rows = 0;

			for (u32 k = 0; k < 4; ++k)
			{
				if (row_instr[k] == umax)
				{
					break;
				}

				++rows;
			}

			// Three rows plus a proven w, or all four. A group holding x and z but not y counts 1
			// here (the scan stops at the first gap) and is refused: it is not a matrix however its
			// slots are laid out. At depth the w is written on the register the translation lands
			// in, so the caller completes the proof instead.
			if (rows < 3 || (rows == 3 && w_slot == umax && depth == 0))
			{
				return false;
			}

			// c[base + k] feeds component k. No stride: every program observed reading an indexed
			// DP4 palette lays its rows out consecutively, and inventing a stride for a shape
			// nothing has presented is how a matrix gets rebuilt out of unrelated slots.
			const u32 base = row_const[0];

			for (u32 k = 1; k < rows; ++k)
			{
				if (row_const[k] != (base + k))
				{
					return false;
				}
			}

			// The translation has to come after every row it translates.
			if (bias_instr != umax && bias_instr < last_row)
			{
				return false;
			}

			if ((base + rows) > s_legal_constant_slots)
			{
				return false;
			}

			out.found = true;
			out.shape = chain_shape::dp4;
			out.base = base;
			out.source = source;
			out.instructions = rows;
			out.first_instruction = first;
			out.indexed = true;
			out.addr_reg = (addr_reg == umax) ? 0 : addr_reg;
			out.addr_swz = (addr_swz == umax) ? 0 : addr_swz;
			out.rows = rows;
			out.stride = 1;
			out.xyz_only = false;
			out.has_bias = (bias_instr != umax);
			out.bias_slot = bias_slot;
			out.w_slot = w_slot;
			out.w_component = w_component;
			return true;
		}

		// One row of a weighted palette blend: the four indexed reads of one constant slot that
		// build one row of the blended matrix in one accumulator.
		struct blend_row
		{
			u32 slot = umax;   // the row's constant slot, read as c[slot + a.<swz>]
			u32 first = umax;  // the MUL that opens the accumulator
			u32 last = 0;
			u32 addr_reg = umax;
			u32 weight_attribute = umax;
			u8 weight_component[max_blend_bones] = {};
			u8 addr_swz[max_blend_bones] = {};
		};

		// 'acc = w.a * c[K + a.p] ; acc += w.b * c[K + a.q] ; acc += ... ; acc += ...' - one MUL
		// and three MADs into one accumulator, every read of the same constant slot through a
		// different component of the same address register, every weight a broadcast component of
		// one vertex attribute.
		//
		// The MUL is required to write all four components, which is what makes taking only the
		// last four writers of the temp sound: a full-width write kills every earlier definition,
		// so the doubling chain that shares the register in twelve of the sixteen observed programs
		// (r1 is both '3 * attr1' and the c34 accumulator in a3af6e3d5f0ac8e6) cannot leak in.
		bool match_blend_row(const program_walker& prog, u32 temp, u32 before, blend_row& out)
		{
			std::vector<u32> writers;
			prog.collect_vec_writers(walk_target{ false, temp }, writers, before);

			if (writers.size() < max_blend_bones)
			{
				return false;
			}

			// collect_vec_writers returns program order, so the last four are the live ones.
			const u32 steps = max_blend_bones;
			const usz base_index = writers.size() - steps;

			for (u32 k = 0; k < steps; ++k)
			{
				const u32 index = writers[base_index + k];
				const decoded_instr& in = prog[index];

				const u32 opcode = in.d1.vec_opcode;
				const u32 wanted = (k == 0) ? RSX_VEC_OPCODE_MUL : RSX_VEC_OPCODE_MAD;

				if (opcode != wanted || vec_writemask(in) != 0xf || !in.d3.index_const
					|| is_conditional(in) || in.d0.staturate
					|| in.src[0].neg || in.src[1].neg || in.src[2].neg
					|| in.d0.src0_abs || in.d0.src1_abs || in.d0.src2_abs)
				{
					return false;
				}

				// The two multiplied operands: one indexed constant read straight (a swizzled row
				// would permute the matrix), one broadcast component of a vertex attribute.
				u32 constant_slot = umax;
				u32 weight_slot = umax;

				for (u32 s = 0; s < 2; ++s)
				{
					const SRC& src = in.src[s];

					if (src.reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
					{
						constant_slot = (constant_slot == umax) ? s : umax;
					}
					else if (src.reg_type == RSX_VP_REGISTER_TYPE_INPUT)
					{
						weight_slot = (weight_slot == umax) ? s : umax;
					}
					else
					{
						return false;
					}
				}

				if (constant_slot == umax || weight_slot == umax)
				{
					return false;
				}

				const SRC& row = in.src[constant_slot];
				const SRC& weight = in.src[weight_slot];

				if (row.swz_x != 0 || row.swz_y != 1 || row.swz_z != 2 || row.swz_w != 3)
				{
					return false;
				}

				u32 weight_component = 0;

				if (!is_broadcast_swizzle(weight, weight_component))
				{
					return false;
				}

				// Every MAD accumulates into the register it is building, read straight.
				if (k != 0)
				{
					const SRC& addend = in.src[2];

					if (addend.reg_type != RSX_VP_REGISTER_TYPE_TEMP || u32{addend.tmp_src} != temp
						|| addend.swz_x != 0 || addend.swz_y != 1 || addend.swz_z != 2 || addend.swz_w != 3)
					{
						return false;
					}
				}

				if (k == 0)
				{
					out.slot = in.d1.const_src;
					out.addr_reg = u32{in.d0.addr_reg_sel_1};
					out.weight_attribute = u32{in.d1.input_src};
					out.first = index;
				}
				else if (out.slot != u32{in.d1.const_src}
					|| out.addr_reg != u32{in.d0.addr_reg_sel_1}
					|| out.weight_attribute != u32{in.d1.input_src})
				{
					// A row assembled out of two slots, two address registers or two attributes is
					// not one row of one palette.
					return false;
				}

				out.weight_component[k] = static_cast<u8>(weight_component);
				out.addr_swz[k] = static_cast<u8>(in.d0.addr_swz);
				out.last = index;
			}

			// Each bone has to be a different entry of the palette; two reads through the same
			// address component are one bone counted twice, and the weights would not sum.
			u32 swz_seen = 0;
			u32 weight_seen = 0;

			for (u32 k = 0; k < steps; ++k)
			{
				swz_seen |= 1u << (out.addr_swz[k] & 3);
				weight_seen |= 1u << (out.weight_component[k] & 3);
			}

			return swz_seen == 0xf && weight_seen == 0xf;
		}

		// One level of the blend group's writer set, after dead definitions have been dropped.
		//
		// 'demand' is which components of this temp the consumer actually reads: 0xf at the level
		// the outer chain reads (a DP4 takes all four), 0x7 one level down through the post-matrix
		// translation, which only ever adds xyz.
		//
		// It matters for the five programs that build a row accumulator in the same register they
		// later write the dotted rows into (2f310e8120a344b2 on r1, the other four on r3): the
		// accumulator's last MAD writes xyzw, so its leftover w would otherwise stay live even
		// though the translation below only ever reads xyz. Note that 'demand' alone does not
		// rescue those programs - it kills the leftover w, but the row DP4 reading its own register
		// then has to be kept out of prune_dead_writers' accumulate reset as well, or all four
		// accumulator writers come back regardless.
		struct blend_level
		{
			u32 row_instr[4] = { umax, umax, umax, umax };
			blend_row row[4]{};
			chain_source source{};
			bool have_source = false;
			u32 first = umax;
			u32 last_row = 0;
			u32 bias_instr = umax;
			u32 bias_slot = 0;
			u32 bias_source = umax; // the temp the translation reads, when it is not this one
			u32 rows = 0;

			// The constant this level moves into its own .w (Role 2). Not the homogeneous 1 the
			// rows multiply - that one lives on the operand - but the w handed on to whatever
			// consumes this level, and it has to be provably the same 1 or the outer group's
			// fourth column is being scaled by something nobody checked.
			u32 target_w_slot = umax;
			u32 target_w_component = 0;
		};

		// Drop definitions of 'temp' that nothing can read: a writer whose components are all
		// re-written before 'before', by later writers that do not read the register back.
		//
		// Needed because seven of the sixteen observed blend programs assemble their result in a
		// register that already held something else - 731962646a64e3a4 and three others reuse the
		// very register holding the decoded position, so the target's writer list carries the
		// position decode and a stale homogeneous w alongside the two instructions that matter.
		// Refusing on those would refuse the programs; ignoring writers without proving them dead
		// is what match_indexed_affine's "anything else is a term this cannot express" rule exists
		// to prevent. Proving them dead is the third option.
		//
		// The 'reads it back' reset is load-bearing in the other direction: the in-place
		// translation 'r4.xyz = c18.xyz + r4.xyz' covers xyz, and without the reset it would mark
		// the three dotted rows feeding it as dead.
		void prune_dead_writers(const program_walker& prog, u32 temp, u32 demand, const std::vector<writer_ref>& in, std::vector<writer_ref>& out)
		{
			out.clear();

			u32 covered = 0;

			for (usz i = in.size(); i-- > 0;)
			{
				const writer_ref& w = in[i];
				const decoded_instr& instr = prog[w.instr];

				if ((w.mask & demand & ~covered) == 0)
				{
					continue;
				}

				out.push_back(w);

				// An instruction that *accumulates* into the register it writes consumes every
				// earlier definition, so nothing before it is dead.
				//
				// A dot product does not accumulate: it reads the register as a whole vector
				// operand for a different role - the bone accumulator - and replaces the one
				// component it writes. Its operand's definitions belong to match_blend_row, which
				// walks them itself, not to this level.
				//
				// Getting that distinction wrong is not hypothetical. Five of the sixteen blend
				// programs allocate the row DP4s' destination on top of the dying c34 accumulator,
				// so the last row reads the register it writes:
				//   edb0911a4c3181d6  6,9,12,15:MUL/MAD>r3.xyzw c34[a]   the accumulator
				//                    17:DP4>r3.z(r3, r0)                 dest r3, src0 r3
				//                    18:DP4>r3.y  19:DP4>r3.x
				// Treating instruction 17 as an accumulate resets 'covered' and resurrects all four
				// accumulator writers, and classify_blend_level then meets '6:MUL>r3.xyzw c34[a]',
				// which is none of the three roles, and refuses the program. The other four -
				// 2f310e8120a344b2 (r1), 731962646a64e3a4, 88fe4699c66df009 and dc56c444e5cc83e7
				// (r3) - fail identically. The eleven that matched without this differ only in
				// register allocation: they write their rows into an r4 distinct from r1/r2/r3.
				//
				// The reset is still load-bearing for the ADD form: the in-place translation
				// 'r4.xyz = c18.xyz + r4.xyz' covers xyz, and without it the three dotted rows
				// feeding that ADD would be marked dead in the nine direct-bias programs.
				const u32 opcode = instr.d1.vec_opcode;
				const bool row_read = (opcode == RSX_VEC_OPCODE_DP4 || opcode == RSX_VEC_OPCODE_DPH);

				bool accumulates = false;
				const u32 sources = (opcode != RSX_VEC_OPCODE_NOP) ? vec_source_mask(opcode) : 0;

				for (u32 s = 0; s < 3; ++s)
				{
					if ((sources & (1u << s)) && instr.src[s].reg_type == RSX_VP_REGISTER_TYPE_TEMP
						&& u32{instr.src[s].tmp_src} == temp)
					{
						accumulates = !row_read;
					}
				}

				covered = accumulates ? 0u : (covered | w.mask);
			}

			std::reverse(out.begin(), out.end());
		}

		// Classify one level's live writers into the three roles a blend group is made of. Anything
		// else writing a demanded component fails the whole match, same contract as
		// match_indexed_affine: a writer this does not understand is a term the backend would
		// silently drop from the transform.
		bool classify_blend_level(const program_walker& prog, u32 temp, const std::vector<writer_ref>& live, blend_level& out)
		{
			for (const writer_ref& w : live)
			{
				const decoded_instr& in = prog[w.instr];
				const u32 opcode = in.d1.vec_opcode;
				const u32 mask = vec_writemask(in);

				if (is_conditional(in) || in.d0.staturate || mask != w.mask)
				{
					return false;
				}

				// Role 1: one row of the blended matrix dotted into the position. Both operands are
				// temps and neither is indexed - the indexing has moved one level in, into the
				// accumulator this reads.
				if ((opcode == RSX_VEC_OPCODE_DP4 || opcode == RSX_VEC_OPCODE_DPH) && !in.d3.index_const
					&& in.src[0].reg_type == RSX_VP_REGISTER_TYPE_TEMP
					&& in.src[1].reg_type == RSX_VP_REGISTER_TYPE_TEMP)
				{
					u32 component = 0;

					if (!single_component(mask, component) || out.row_instr[component] != umax
						|| in.src[0].neg || in.src[1].neg || in.d0.src0_abs || in.d0.src1_abs)
					{
						return false;
					}

					// Which operand is the accumulator and which is the position is not encoded, so
					// it is decided by which one is provably built by the blend recurrence. Both
					// would be ambiguous; neither is not this shape.
					blend_row candidate[2]{};
					bool ok[2] = {};

					for (u32 s = 0; s < 2; ++s)
					{
						ok[s] = match_blend_row(prog, u32{in.src[s].tmp_src}, w.instr, candidate[s]);
					}

					if (ok[0] == ok[1])
					{
						return false;
					}

					const u32 acc = ok[0] ? 0u : 1u;
					const SRC& operand = in.src[1 - acc];

					// The position is read straight: a swizzle here would mean the row is being
					// applied to a permutation of the vertex.
					if (operand.swz_x != 0 || operand.swz_y != 1 || operand.swz_z != 2 || operand.swz_w != 3)
					{
						return false;
					}

					chain_source other{};
					other.reg_type = RSX_VP_REGISTER_TYPE_TEMP;
					other.index = u32{operand.tmp_src};

					if (!out.have_source)
					{
						out.source = other;
						out.have_source = true;
					}
					else if (!(out.source == other))
					{
						// Rows applied to two different operands are not one matrix times one point.
						return false;
					}

					out.row[component] = candidate[acc];
					out.row_instr[component] = w.instr;
					out.first = std::min(out.first, candidate[acc].first);
					out.last_row = std::max(out.last_row, w.instr);
					continue;
				}

				// Role 2: a constant moved into w only. This is the w handed on to whatever consumes
				// the group, not the homogeneous 1 of the operand the rows multiply - that one is
				// read off the operand itself, which is what build_palette_matrix documents
				// palette_w_slot to be. Recorded rather than waved through: the caller requires it
				// to name the same constant component as the operand's, so the one value
				// build_palette_matrix proves to be 1 covers both.
				if (opcode == RSX_VEC_OPCODE_MOV && mask == 0x8
					&& in.src[0].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT
					&& !in.d3.index_const && !in.src[0].neg && !in.d0.src0_abs)
				{
					out.target_w_slot = in.d1.const_src;
					out.target_w_component = in.src[0].swz_w;
					continue;
				}

				// Role 3: the post-matrix translation, 'target.xyz = c[B].xyz + T.xyz'. T is this
				// temp when the ucode adds in place (nine of the sixteen) and a different temp when
				// it biases on the way out to another register (the other seven); the second form is
				// how the group's rows end up one level down.
				if (opcode == RSX_VEC_OPCODE_ADD && mask == 0x7 && !in.d3.index_const && out.bias_instr == umax)
				{
					constexpr u32 add_slots[2] = { 0, 2 };

					u32 const_slot = umax;
					u32 other_slot = umax;

					for (const u32 s : add_slots)
					{
						if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
						{
							const_slot = (const_slot == umax) ? s : umax;
						}
						else
						{
							other_slot = s;
						}
					}

					if (const_slot == umax || other_slot == umax)
					{
						return false;
					}

					const SRC& k = in.src[const_slot];
					const SRC& t = in.src[other_slot];

					// Both operands read straight, so the constant really is a translation in this
					// vertex's own axis order.
					if (k.neg || t.neg || in.d0.src0_abs || in.d0.src2_abs
						|| k.swz_x != 0 || k.swz_y != 1 || k.swz_z != 2
						|| t.swz_x != 0 || t.swz_y != 1 || t.swz_z != 2
						|| t.reg_type != RSX_VP_REGISTER_TYPE_TEMP)
					{
						return false;
					}

					out.bias_instr = w.instr;
					out.bias_slot = in.d1.const_src;
					out.bias_source = (u32{t.tmp_src} == temp) ? umax : u32{t.tmp_src};
					continue;
				}

				// Anything else writing a component the consumer reads is a term this cannot express.
				return false;
			}

			for (u32 k = 0; k < 4; ++k)
			{
				if (out.row_instr[k] == umax)
				{
					break;
				}

				++out.rows;
			}

			return true;
		}

		// The weighted-skinning shape: a palette read once per bone and summed, then applied to the
		// position. Resistance 2 (NPEA00431) writes all sixteen of its character rigs this way and
		// writes them identically - a3af6e3d5f0ac8e6 in full, with only register numbers differing
		// across the other fifteen:
		//
		//    2:ADD>r1.xyzw(attr1, attr1)              \  r1 = 3 * attr1, the palette's row stride
		//    6:ADD>r1.xyzw(attr1, r1)                 /
		//    8:ARL>a0.xyzw(r1.wzxy)                      one ARL, all four components
		//   10:MOV>r0.w(c0.zzzz)                         the operand's homogeneous 1
		//   13:MUL>r3.xyzw(attr2.yyyy, c32[a])       \
		//   14:MAD>r3.xyzw(attr2.xxxx, c32[a], r3)    |  row 0 of the blended matrix
		//   19:MAD>r3.xyzw(attr2.zzzz, c32[a], r3)    |
		//   20:MAD>r3.xyzw(attr2.wwww, c32[a], r3)   /
		//   11,16,17,22: the same four into r2 off c33     row 1
		//   12,15,18,21: the same four into r1 off c34     row 2
		//   25:DP4>r4.x(r0, r3)  24:DP4>r4.y(r0, r2)  23:DP4>r4.z(r1, r0)
		//   32:ADD>r5.xyz(c18.xyz, r4.xyz)              the post-matrix translation
		//
		// which is 'sum_k weight.k * M[a.<swz_k>]' applied to the position: a convex blend of
		// palette entries, exactly what remixapi_MeshInfoSkinning carries. The three bases c32,
		// c33 and c34 being one consecutive group is corroborated independently by the x3 on the
		// index - three rows per bone is the only stride that makes both facts true at once.
		//
		// Neither older matcher can express it and neither should: match_indexed_affine's rows are
		// single indexed DP4s, and here the DP4s read no constant at all. The one structural
		// variation across the sixteen is where the translation lands - nine add in place, seven
		// bias out into another register - which is the one descent below.
		bool match_blend_palette(const program_walker& prog, const walk_target& target, const std::vector<writer_ref>& writers, chain_result& out)
		{
			if (target.is_output || writers.empty())
			{
				// The translation reads a temp back, and an output register is never read by a
				// vertex program, so the target has to be a temp.
				return false;
			}

			// The consumer of this level is the outer matrix chain, and a DP4 reads all four
			// components.
			std::vector<writer_ref> live;
			prune_dead_writers(prog, target.index, 0xf, writers, live);

			blend_level level{};

			if (live.empty() || !classify_blend_level(prog, target.index, live, level))
			{
				return false;
			}

			u32 bias_instr = level.bias_instr;
			u32 bias_slot = level.bias_slot;

			if (level.rows == 0)
			{
				// The rows are one level in, behind a translation that copies as it adds. The
				// translation only ever reads xyz, so that is all the level below has to define.
				if (bias_instr == umax || level.bias_source == umax)
				{
					return false;
				}

				const u32 inner = level.bias_source;

				std::vector<u32> inner_writers;
				prog.collect_vec_writers(walk_target{ false, inner }, inner_writers, bias_instr);

				std::vector<writer_ref> inner_refs;
				inner_refs.reserve(inner_writers.size());

				for (const u32 i : inner_writers)
				{
					inner_refs.push_back(writer_ref{ i, vec_writemask(prog[i]) });
				}

				std::vector<writer_ref> inner_live;
				prune_dead_writers(prog, inner, 0x7, inner_refs, inner_live);

				blend_level inner_level{};

				if (inner_live.empty() || !classify_blend_level(prog, inner, inner_live, inner_level))
				{
					return false;
				}

				if (inner_level.rows == 0 || inner_level.bias_instr != umax)
				{
					// Two translations would mean one of them is not the object-to-world step.
					return false;
				}

				level.rows = inner_level.rows;
				level.source = inner_level.source;
				level.have_source = inner_level.have_source;
				level.first = inner_level.first;
				level.last_row = inner_level.last_row;

				for (u32 k = 0; k < 4; ++k)
				{
					level.row[k] = inner_level.row[k];
					level.row_instr[k] = inner_level.row_instr[k];
				}
			}
			else if (level.bias_source != umax)
			{
				// Rows here and a translation reading somewhere else: the two are not the same
				// transform, whatever each of them is.
				return false;
			}

			// Same rule the single-matrix palette uses: three rows plus a proven homogeneous w, or
			// all four. A group holding x and z but not y stops at 1 above and is refused here.
			if (level.rows < 3 || !level.have_source)
			{
				return false;
			}

			const u32 base = level.row[0].slot;

			for (u32 k = 1; k < level.rows; ++k)
			{
				if (level.row[k].slot != (base + k))
				{
					return false;
				}
			}

			// Every row has to blend the same bones, in the same weight-to-address-component
			// pairing, off the same address register and the same weight attribute. Rows that
			// disagree are three unrelated sums, not one matrix.
			u8 pairing[max_blend_bones] = {};

			for (u32 k = 0; k < max_blend_bones; ++k)
			{
				pairing[level.row[0].weight_component[k] & 3] = level.row[0].addr_swz[k];
			}

			for (u32 r = 0; r < level.rows; ++r)
			{
				if (level.row[r].addr_reg != level.row[0].addr_reg
					|| level.row[r].weight_attribute != level.row[0].weight_attribute)
				{
					return false;
				}

				for (u32 k = 0; k < max_blend_bones; ++k)
				{
					if (pairing[level.row[r].weight_component[k] & 3] != level.row[r].addr_swz[k])
					{
						return false;
					}
				}
			}

			// The homogeneous 1, taken from the *operand* the rows multiply rather than from the
			// target. That is what build_palette_matrix documents palette_w_slot to be, and on this
			// family the two are different constants: a3af6e3d5f0ac8e6 writes c24.z into the target
			// and c0.z into the operand, and only the second is the fourth coordinate the rows are
			// applied to. Reading the wrong one refuses every draw of that program the moment
			// build_palette_matrix checks the value is 1.
			u32 w_slot = umax;
			u32 w_component = 0;

			if (level.rows < 4)
			{
				// Bounded at the *first* row, not the last. Every row multiplies the same operand,
				// so the w that matters is the one standing when the first of them runs; taking the
				// last row's bound would accept a redefinition landing between the rows, i.e. a w
				// only the later rows ever saw. Both bounds are resolved and required to agree,
				// which is what actually proves no such redefinition exists.
				u32 first_row = umax;

				for (u32 k = 0; k < level.rows; ++k)
				{
					first_row = std::min(first_row, level.row_instr[k]);
				}

				bool from_sca = false;
				bool from_sca_last = false;
				const u32 def = prog.last_component_writer(level.source.index, 3, first_row, from_sca);
				const u32 def_last = prog.last_component_writer(level.source.index, 3, level.last_row, from_sca_last);

				if (def == umax || from_sca || def != def_last || from_sca_last)
				{
					return false;
				}

				const decoded_instr& in = prog[def];

				if (in.d1.vec_opcode != RSX_VEC_OPCODE_MOV || in.d3.index_const
					|| in.src[0].reg_type != RSX_VP_REGISTER_TYPE_CONSTANT
					|| in.src[0].neg || in.d0.src0_abs || in.d0.staturate || is_conditional(in))
				{
					return false;
				}

				w_slot = in.d1.const_src;
				w_component = in.src[0].swz_w;

				// The level's own .w has to be that same constant component. build_palette_matrix
				// reads back exactly one w and requires it to be 1; if the group hands its consumer
				// a different constant, that one is unproven and the outer group's fourth column is
				// being scaled by a value nobody looked at. All sixteen observed programs move c0.z
				// into both, so this refuses none of them - a3af6e3d5f0ac8e6's c24.z sits on the
				// inner r4, which the translation reads xyz-only and pruning drops.
				if (level.target_w_slot != umax
					&& (level.target_w_slot != w_slot || level.target_w_component != w_component))
				{
					return false;
				}
			}

			// The translation has to come after every row it translates.
			if (bias_instr != umax && bias_instr < level.last_row)
			{
				return false;
			}

			if ((base + level.rows) > s_legal_constant_slots)
			{
				return false;
			}

			out.found = true;
			out.shape = chain_shape::dp4;
			out.base = base;
			out.source = level.source;
			out.instructions = level.rows * max_blend_bones;
			out.first_instruction = level.first;
			out.indexed = true;
			out.addr_reg = level.row[0].addr_reg;
			// The single-bone fields describe bone 0, so anything that still reads them (the
			// indexing audit, the diagnostics) sees a coherent palette rather than a default.
			out.addr_swz = pairing[0];
			out.rows = level.rows;
			out.stride = 1;
			out.xyz_only = false;
			out.has_bias = (bias_instr != umax);
			out.bias_slot = bias_slot;
			out.w_slot = w_slot;
			out.w_component = w_component;
			out.blended = true;
			out.blend_bones = max_blend_bones;
			out.blend_weight_attribute = level.row[0].weight_attribute;

			for (u32 k = 0; k < max_blend_bones; ++k)
			{
				out.blend_weight_component[k] = static_cast<u8>(k);
				out.blend_addr_swz[k] = pairing[k];
			}

			return true;
		}


		// Depth written as a w-buffer: 'o0.z = (c[k] . pos) * (c[w] . pos)', i.e. the clip z
		// premultiplied by the clip w so that the perspective divide leaves the *linear* eye-space
		// depth in the depth register instead of z/w. The four rows of an ordinary 4x4 are all
		// present and consecutive - only z takes the detour.
		//
		// Resistance 2's 3152b710c603e12d is exactly that, and it is a 34679 x 0 x 32770 plane -
		// spanY is zero to the last bit, so it is a ground or water plane and nothing else:
		//    4:DP4>o0.y(T0,c33)  5:DP4>o0.x(T0,c32)  6:DP4>r1.z(T0,c34)  7:DP4>r0.x(T0,c35)
		//   10:MUL>o0.z(T1.zzzz,T0.xxxx)   11:MOV>o0.w(T0.xxxx)
		// x, y and w are plain rows over one source; z is the c34 row times the c35 row, and c35 is
		// the row that also produces w. It was the "1 mixed" program the HPOS-indirection work
		// listed and refused, and it reports arch=unknown(no matrix chain into HPOS) on every
		// capture up to and including ae94587.
		//
		// What Remix needs from this program is the position; the depth encoding is the title's own
		// business and a path tracer has no use for a w-buffer nonlinearity. So z is taken from the
		// row that feeds the trick and the projection carries depth normally.
		//
		// Deliberately not a general "ignore what z does" rule. The multiply has to be of two
		// temps, each provably defined by a plain non-indexed DP4/DPH, and one of the two has to be
		// the very row that produces w - that is what makes it a premultiply by w rather than an
		// arbitrary product. Everything else about the group (one common source, four consecutive
		// slots) is still proven afterwards by match_dp4_chain on the repaired writers.
		// RPCS3_REMIX_WBUFFERZ=0 restores refusing them.
		bool repair_wbuffer_z(const program_walker& prog, std::vector<writer_ref>& refs)
		{
			// Exactly one writer per HPOS component, or there is no single z to repair.
			u32 by_component[4] = { umax, umax, umax, umax };

			for (u32 i = 0; i < ::size32(refs); ++i)
			{
				u32 component = 0;

				if (!single_component(refs[i].mask, component) || by_component[component] != umax)
				{
					return false;
				}

				by_component[component] = i;
			}

			for (const u32 slot : by_component)
			{
				if (slot == umax)
				{
					return false;
				}
			}

			const decoded_instr& zi = prog[refs[by_component[2]].instr];
			const decoded_instr& wi = prog[refs[by_component[3]].instr];

			if (zi.d1.vec_opcode != RSX_VEC_OPCODE_MUL || zi.d3.index_const || is_conditional(zi) || zi.d0.staturate)
			{
				return false;
			}

			if ((wi.d1.vec_opcode != RSX_VEC_OPCODE_DP4 && wi.d1.vec_opcode != RSX_VEC_OPCODE_DPH) || wi.d3.index_const)
			{
				return false;
			}

			const u32 w_const = wi.d1.const_src;

			// Both factors: a temp read through a broadcast swizzle, defined by a plain DP4/DPH.
			u32 factor_instr[2] = { umax, umax };
			u32 factor_const[2] = { umax, umax };

			for (u32 s = 0; s < 2; ++s)
			{
				const SRC& src = zi.src[s];

				u32 component = 0;

				if (src.reg_type != RSX_VP_REGISTER_TYPE_TEMP || src.neg
					|| !is_broadcast_swizzle(src, component))
				{
					return false;
				}

				bool from_sca = false;
				const u32 def = prog.last_component_writer(u32{src.tmp_src}, component, refs[by_component[2]].instr, from_sca);

				if (def == umax || from_sca)
				{
					return false;
				}

				const decoded_instr& in = prog[def];

				if ((in.d1.vec_opcode != RSX_VEC_OPCODE_DP4 && in.d1.vec_opcode != RSX_VEC_OPCODE_DPH)
					|| in.d3.index_const || is_conditional(in) || in.d0.staturate)
				{
					return false;
				}

				factor_instr[s] = def;
				factor_const[s] = in.d1.const_src;
			}

			// One factor has to be the w row itself. Without that this is some other product and
			// there is no reason to believe the other factor is the depth row.
			u32 z_row = umax;

			if (factor_const[0] == w_const && factor_const[1] != w_const)
			{
				z_row = factor_instr[1];
			}
			else if (factor_const[1] == w_const && factor_const[0] != w_const)
			{
				z_row = factor_instr[0];
			}

			if (z_row == umax)
			{
				return false;
			}

			refs[by_component[2]].instr = z_row;
			return true;
		}

		chain_result find_chain(const program_walker& prog, const walk_target& target, u32 before, chain_context& ctx, u32 depth = 0)
		{
			chain_result result{};

			if (prog.has_sca_writer(target, before))
			{
				return result;
			}

			std::vector<u32> writers;
			prog.collect_vec_writers(target, writers, before);

			if (writers.empty())
			{
				return result;
			}

			std::vector<writer_ref> refs;
			refs.reserve(writers.size());
			for (const u32 i : writers)
			{
				refs.push_back(writer_ref{ i, vec_writemask(prog[i]) });
			}

			// The DP4 form is left non-indexed on purpose: no title has presented an indexed
			// dot-product palette here, and inventing a matcher for one would be speculation.
			if (match_dp4_chain(prog, refs, result))
			{
				return result;
			}

			result = chain_result{};

			if (match_mad_chain(prog, refs, result, ctx.allow_indexed))
			{
				return result;
			}

			result = chain_result{};

			// The indexed object matrix, tried after both generic matchers so a group that already
			// matches one keeps matching it. Direct writers only: its MOV and ADD roles are
			// structural, and a component reached through a parked register is not one of them.
			if (ctx.allow_indexed && indexed_world_enabled() && match_indexed_affine(prog, target, refs, result))
			{
				return result;
			}

			result = chain_result{};

			// ...and the same group read once per bone and summed. Tried after match_indexed_affine
			// so a single-matrix palette keeps matching the matcher it already matched: the two
			// shapes are disjoint (one indexes its DP4s, the other indexes the accumulators the
			// DP4s read) but ordering them makes that a property of the code, not of the ucode.
			if (ctx.allow_indexed && indexed_world_enabled() && bone_blend_enabled()
				&& match_blend_palette(prog, target, refs, result))
			{
				return result;
			}

			// ...and, last of the direct forms, the w-buffer depth encoding.
			if (target.is_output && wbuffer_z_enabled())
			{
				result = chain_result{};

				std::vector<writer_ref> repaired = refs;

				if (repair_wbuffer_z(prog, repaired) && match_dp4_chain(prog, repaired, result))
				{
					result.wbuffer_z = true;
					return result;
				}
			}

			// Same writers, reached through the registers they were parked in.
			if (ctx.allow_indirect)
			{
				result = chain_result{};

				std::vector<writer_ref> resolved;
				bool refused = false;

				if (resolve_writers(prog, writers, resolved, refused))
				{
					if (match_dp4_chain(prog, resolved, result))
					{
						result.indirect = true;
						return result;
					}

					result = chain_result{};

					if (match_mad_chain(prog, resolved, result, ctx.allow_indexed))
					{
						result.indirect = true;
						return result;
					}

					// The two detours compose: 3152b710c603e12d parks its w row in a register
					// *and* premultiplies its z by it, so neither repair reaches the matrix alone.
					if (target.is_output && wbuffer_z_enabled())
					{
						result = chain_result{};

						std::vector<writer_ref> repaired = resolved;

						if (repair_wbuffer_z(prog, repaired) && match_dp4_chain(prog, repaired, result))
						{
							result.indirect = true;
							result.wbuffer_z = true;
							return result;
						}
					}
				}

				ctx.refused |= refused;
			}

			// A program that assembles the clip position in a temp and copies it out at the end
			// hides the chain one hop away. Chase the copy.
			if (depth < 3)
			{
				// The forwarding copy is the last thing that touched the target.
				const u32 last = writers.back();
				const decoded_instr& in = prog[last];

				if (in.d1.vec_opcode == RSX_VEC_OPCODE_MOV
					&& vec_writemask(in) == 0xf
					&& in.src[0].reg_type == RSX_VP_REGISTER_TYPE_TEMP)
				{
					return find_chain(prog, walk_target{ false, in.src[0].tmp_src }, last, ctx, depth + 1);
				}
			}

			return chain_result{};
		}

		// Bounded backward slice from HPOS, only to count constants and spot indexed addressing.
		//
		// The walk is order-aware: a register read by the instruction at index i can only have
		// been written by an instruction at index < i, because RSX vertex programs are
		// straight-line code. Collecting every writer in the program regardless of position -
		// which is what this did up to e9a7956 - drags the *later* half of the program into the
		// position slice. That is not a theoretical concern: all three of Haze's indexed
		// programs (af06f6d32ec048ee, ea1fc49cc2ef7afc, df46f03b1b7ab8a4) write HPOS and then
		// read c27[a]/c29[a] into a temp several instructions afterwards, so the unordered walk
		// reported indexed addressing in the position chain for programs whose position never
		// touches an indexed constant, and submit_subdraw refused them as unrecognised rigs.
		// RPCS3_REMIX_LOOSESLICE=1 restores the old behaviour.
		// 'output_index' is the vertex-program output register the backward slice starts from.
		// 0 is HPOS, which is the only one the matcher itself cares about; the texcoord outputs
		// (TEX0..TEX7 are o7..o14, per rpcs3's own output table in VKVertexProgram.cpp:292-299)
		// are sliced by the diagnostics, to read what a title does to a texcoord attribute between
		// loading it and writing it out.
		void slice_position(const program_walker& prog, u32 output_index, u32& out_distinct_consts, u32& out_instructions, bool& out_indexed, std::vector<u32>* out_indices = nullptr)
		{
			// 'before' is the exclusive instruction bound a write has to precede to reach this
			// read. Two reads of the same register at different points in the program are
			// genuinely different targets, so it takes part in the visited test.
			struct slice_target
			{
				walk_target reg;
				u32 before = 0;

				bool operator==(const slice_target& other) const
				{
					return reg == other.reg && before == other.before;
				}
			};

			const bool ordered = !loose_slice_enabled();
			const u32 program_size = static_cast<u32>(prog.size());

			// The visited set is now (register, bound) rather than register alone, so a linear
			// scan of it is quadratic in a way the old one was not. A dense bitmap keeps the
			// walk linear in the number of distinct targets: 2 spaces x 64 registers x one slot
			// per instruction boundary.
			constexpr u32 register_space = 64;
			const u32 bound_slots = program_size + 1;
			std::vector<u8> visited(usz{2} * register_space * bound_slots, 0);

			const auto mark_visited = [&](const slice_target& t) -> bool
			{
				if (t.reg.index >= register_space || t.before > program_size)
				{
					// Out of the bitmap's range. Cannot loop forever regardless: 'before'
					// strictly decreases along every edge in ordered mode, and the walk budget
					// below is the backstop for the loose one.
					return false;
				}

				const usz slot = ((usz{t.reg.is_output ? 1u : 0u} * register_space) + t.reg.index) * bound_slots + t.before;

				if (visited[slot])
				{
					return true;
				}

				visited[slot] = 1;
				return false;
			};

			std::vector<slice_target> pending;
			std::vector<u8> seen_instr(prog.size(), 0);
			std::vector<u32> consts;

			pending.push_back(slice_target{ walk_target{ true, output_index }, program_size });

			u32 instructions = 0;
			bool indexed = false;

			// One walk per distinct (register, bound) plus slack. Never reached in practice;
			// it exists so a malformed ucode cannot wedge the RSX thread.
			u32 budget = 4 * static_cast<u32>(visited.size()) + 1024;

			while (!pending.empty() && budget-- != 0)
			{
				const slice_target entry = pending.back();
				pending.pop_back();

				const walk_target target = entry.reg;

				if (mark_visited(entry))
				{
					continue;
				}

				const u32 limit = ordered ? std::min(entry.before, program_size) : program_size;

				for (u32 i = 0; i < limit; ++i)
				{
					const decoded_instr& in = prog[i];

					const bool vec_hit = target.is_output ? vec_writes_output(in, target.index) : vec_writes_temp(in, target.index);
					const bool sca_hit = target.is_output ? sca_writes_output(in, target.index) : sca_writes_temp(in, target.index);

					if (!vec_hit && !sca_hit)
					{
						continue;
					}

					if (!seen_instr[i])
					{
						seen_instr[i] = 1;
						++instructions;
					}

					u32 mask = 0;

					if (vec_hit)
					{
						mask |= vec_source_mask(in.d1.vec_opcode);
					}

					if (sca_hit)
					{
						// SCA operands come from src2.
						mask |= 0b100;
					}

					for (u32 s = 0; s < 3; ++s)
					{
						if (!(mask & (1u << s)))
						{
							continue;
						}

						switch (in.src[s].reg_type)
						{
						case RSX_VP_REGISTER_TYPE_CONSTANT:
						{
							indexed |= !!in.d3.index_const;

							// An indexed read depends on the address register as surely as it
							// depends on its operands, and vec_writes_temp no longer pretends the
							// ARL wrote a temp. Absorb the ARL that loaded this read's register so
							// the slice keeps naming it - that instruction is the whole reason a
							// program is classified as a rig - and keep walking through its own
							// operands so the bone attribute stays in the slice's input set.
							// Only its temp sources: a constant the ARL reads scales the *index*,
							// not the position, so counting it in distinct_consts would move the
							// '<4 constants means this is not a matrix' threshold on a value that
							// has nothing to do with the matrix.
							if (in.d3.index_const)
							{
								const u32 addr_reg = u32{in.d0.addr_reg_sel_1};

								for (u32 k = i; k-- > 0;)
								{
									const decoded_instr& arl = prog[k];

									if (arl.d1.vec_opcode != RSX_VEC_OPCODE_ARL || (u32{arl.d0.dst_tmp} & 1u) != addr_reg)
									{
										continue;
									}

									if (!seen_instr[k])
									{
										seen_instr[k] = 1;
										++instructions;
									}

									if (arl.src[0].reg_type == RSX_VP_REGISTER_TYPE_TEMP)
									{
										pending.push_back(slice_target{ walk_target{ false, arl.src[0].tmp_src }, ordered ? k : program_size });
									}

									break;
								}
							}

							const u32 slot = in.d1.const_src;
							bool known = false;
							for (const u32 c : consts)
							{
								if (c == slot)
								{
									known = true;
									break;
								}
							}

							if (!known)
							{
								consts.push_back(slot);
							}

							break;
						}
						case RSX_VP_REGISTER_TYPE_TEMP:
							// In loose mode the bound is not used, so collapse it to keep the
							// visited set keyed on the register alone - exactly the pre-e9a7956
							// walk, at the pre-e9a7956 cost.
							pending.push_back(slice_target{ walk_target{ false, in.src[s].tmp_src }, ordered ? i : program_size });
							break;
						default:
							break;
						}
					}
				}
			}

			out_distinct_consts = ::size32(consts);
			out_instructions = instructions;
			out_indexed = indexed;

			if (out_indices)
			{
				out_indices->clear();
				for (u32 i = 0; i < ::size32(seen_instr); ++i)
				{
					if (seen_instr[i])
					{
						out_indices->push_back(i);
					}
				}
			}
		}

		const char* source_kind(const SRC& s)
		{
			switch (s.reg_type)
			{
			case RSX_VP_REGISTER_TYPE_TEMP: return "T";
			case RSX_VP_REGISTER_TYPE_INPUT: return "I";
			case RSX_VP_REGISTER_TYPE_CONSTANT: return "C";
			default: return "-";
			}
		}

		void append_mask(std::string& out, u32 mask)
		{
			if (mask == 0)
			{
				out += '0';
				return;
			}

			if (mask & 1) out += 'x';
			if (mask & 2) out += 'y';
			if (mask & 4) out += 'z';
			if (mask & 8) out += 'w';
		}

		// Read an environment flag once.
		bool env_flag(const wchar_t* name)
		{
			wchar_t buffer[8]{};
			const DWORD written = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return false;
			}

			return buffer[0] != L'0';
		}

		f32 env_float(const wchar_t* name, f32 fallback)
		{
			wchar_t buffer[64]{};
			const DWORD written = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return fallback;
			}

			const f32 parsed = static_cast<f32>(::_wtof(buffer));
			return (std::isfinite(parsed) && parsed > 0.f) ? parsed : fallback;
		}

		// Unsigned integer knob. Unlike env_float the fallback covers "unset" only, so 0 stays a
		// usable value (RPCS3_REMIX_SKINBONE=0 means bone 0, not "off").
		u32 env_u32(const wchar_t* name, u32 fallback)
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return fallback;
			}

			wchar_t* end = nullptr;
			const unsigned long parsed = ::wcstoul(buffer, &end, 10);

			if (end == buffer || parsed > 0xFFFFFFFEul)
			{
				return fallback;
			}

			return static_cast<u32>(parsed);
		}

		// "aabbccdd11223344,556677..." -> the hashes, parsed once. Separators are comma, space
		// or semicolon so a value pasted out of the dev menu or the dump line works as typed.
		std::vector<u64> parse_hash_list(const wchar_t* name)
		{
			std::vector<u64> out;

			// Enough for ~200 hashes; a longer list is a conf-file problem, not a knob problem.
			std::vector<wchar_t> buffer(4096, L'\0');
			const DWORD written = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));

			if (written == 0 || written >= buffer.size())
			{
				return out;
			}

			const wchar_t* cursor = buffer.data();

			while (*cursor)
			{
				while (*cursor == L',' || *cursor == L' ' || *cursor == L';' || *cursor == L'\t')
				{
					++cursor;
				}

				if (!*cursor)
				{
					break;
				}

				wchar_t* end = nullptr;
				const u64 value = ::_wcstoui64(cursor, &end, 16);

				if (end == cursor)
				{
					// Not a hex digit: skip the token rather than spinning on it.
					while (*cursor && *cursor != L',' && *cursor != L' ' && *cursor != L';')
					{
						++cursor;
					}

					continue;
				}

				if (value)
				{
					out.push_back(value);
				}

				cursor = end;
			}

			return out;
		}

		// Same grammar as the environment parser above, for the config strings.
		void append_hash_list(std::vector<u64>& out, const std::string& text)
		{
			const char* cursor = text.c_str();

			while (*cursor)
			{
				while (*cursor == ',' || *cursor == ' ' || *cursor == ';' || *cursor == '\t')
				{
					++cursor;
				}

				if (!*cursor)
				{
					break;
				}

				char* end = nullptr;
				const u64 value = ::_strtoui64(cursor, &end, 16);

				if (end == cursor)
				{
					while (*cursor && *cursor != ',' && *cursor != ' ' && *cursor != ';')
					{
						++cursor;
					}

					continue;
				}

				if (value)
				{
					out.push_back(value);
				}

				cursor = end;
			}
		}

		// The environment entries are parsed once; the config strings are re-parsed whenever they
		// change, so editing a category list in the settings dialog takes effect without a restart.
		const std::vector<u64>* category_lists()
		{
			static const std::vector<u64> from_env[4] = {
				parse_hash_list(L"RPCS3_REMIX_CAT_SKY"),
				parse_hash_list(L"RPCS3_REMIX_CAT_HIDE"),
				parse_hash_list(L"RPCS3_REMIX_CAT_PARTICLE"),
				parse_hash_list(L"RPCS3_REMIX_CAT_DECAL"),
			};

			static std::vector<u64> lists[4];
			static std::string cached[4];

			const std::string current[4] = {
				g_cfg.video.remix.category_sky.to_string(),
				g_cfg.video.remix.category_hide.to_string(),
				g_cfg.video.remix.category_particle.to_string(),
				g_cfg.video.remix.category_decal.to_string(),
			};

			for (u32 i = 0; i < 4; ++i)
			{
				if (cached[i] != current[i] || (lists[i].empty() && !from_env[i].empty()))
				{
					cached[i] = current[i];
					lists[i]  = from_env[i];
					append_hash_list(lists[i], cached[i]);
				}
			}

			return lists;
		}

		f32 vec_length(const f32 (&v)[3])
		{
			return std::sqrt((v[0] * v[0]) + (v[1] * v[1]) + (v[2] * v[2]));
		}

		bool normalize3(f32 (&v)[3])
		{
			const f32 len = vec_length(v);
			if (!std::isfinite(len) || len < 1e-8f)
			{
				return false;
			}

			v[0] /= len;
			v[1] /= len;
			v[2] /= len;
			return true;
		}

		void cross3(const f32 (&a)[3], const f32 (&b)[3], f32 (&out)[3])
		{
			out[0] = (a[1] * b[2]) - (a[2] * b[1]);
			out[1] = (a[2] * b[0]) - (a[0] * b[2]);
			out[2] = (a[0] * b[1]) - (a[1] * b[0]);
		}

		// Row-vector transform of a homogeneous point: out = p * m.
		void transform_point(const mat4& m, const f32 (&p)[4], f32 (&out)[4])
		{
			for (u32 j = 0; j < 4; ++j)
			{
				out[j] = (p[0] * m.m[0][j]) + (p[1] * m.m[1][j]) + (p[2] * m.m[2][j]) + (p[3] * m.m[3][j]);
			}
		}

		bool unproject(const mat4& inverse_m, f32 ndc_x, f32 ndc_y, f32 ndc_z, f32 (&out)[3])
		{
			const f32 clip[4] = { ndc_x, ndc_y, ndc_z, 1.f };
			f32 world[4]{};
			transform_point(inverse_m, clip, world);

			if (!std::isfinite(world[3]) || std::abs(world[3]) < 1e-9f)
			{
				return false;
			}

			out[0] = world[0] / world[3];
			out[1] = world[1] / world[3];
			out[2] = world[2] / world[3];
			return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
		}

		// The eye is the world point whose clip x, y and w all vanish.
		bool solve_camera_position(const mat4& m, f32 (&out)[3])
		{
			const u32 cols[3] = { 0, 1, 3 };

			f64 a[3][4]{};
			for (u32 r = 0; r < 3; ++r)
			{
				for (u32 k = 0; k < 3; ++k)
				{
					a[r][k] = m.m[k][cols[r]];
				}

				a[r][3] = -static_cast<f64>(m.m[3][cols[r]]);
			}

			// Gauss-Jordan with partial pivoting.
			for (u32 col = 0; col < 3; ++col)
			{
				u32 pivot = col;
				for (u32 r = col + 1; r < 3; ++r)
				{
					if (std::abs(a[r][col]) > std::abs(a[pivot][col]))
					{
						pivot = r;
					}
				}

				if (std::abs(a[pivot][col]) < 1e-12)
				{
					return false;
				}

				if (pivot != col)
				{
					for (u32 k = 0; k < 4; ++k)
					{
						std::swap(a[pivot][k], a[col][k]);
					}
				}

				const f64 inv = 1.0 / a[col][col];
				for (u32 k = 0; k < 4; ++k)
				{
					a[col][k] *= inv;
				}

				for (u32 r = 0; r < 3; ++r)
				{
					if (r == col)
					{
						continue;
					}

					const f64 factor = a[r][col];
					for (u32 k = 0; k < 4; ++k)
					{
						a[r][k] -= factor * a[col][k];
					}
				}
			}

			for (u32 r = 0; r < 3; ++r)
			{
				if (!std::isfinite(a[r][3]))
				{
					return false;
				}

				out[r] = static_cast<f32>(a[r][3]);
			}

			return true;
		}

		bool try_split_once(const mat4& fused, vp_split& out)
		{
			mat4 inverse_fused{};
			if (!mat4_invert(fused, inverse_fused))
			{
				return false;
			}

			f32 cam_pos[3]{};
			if (!solve_camera_position(fused, cam_pos))
			{
				return false;
			}

			f32 center[3]{};
			f32 above[3]{};
			if (!unproject(inverse_fused, 0.f, 0.f, 0.5f, center) ||
				!unproject(inverse_fused, 0.f, 1.f, 0.5f, above))
			{
				return false;
			}

			f32 forward[3] = { center[0] - cam_pos[0], center[1] - cam_pos[1], center[2] - cam_pos[2] };
			if (!normalize3(forward))
			{
				return false;
			}

			f32 up_hint[3] = { above[0] - center[0], above[1] - center[1], above[2] - center[2] };
			if (!normalize3(up_hint))
			{
				return false;
			}

			f32 right[3]{};
			cross3(up_hint, forward, right);
			if (!normalize3(right))
			{
				return false;
			}

			f32 up[3]{};
			cross3(forward, right, up);
			if (!normalize3(up))
			{
				return false;
			}

			// viewToWorld: rows are right / up / forward / position (Remix's own layout).
			mat4 view_to_world = mat4_identity();
			for (u32 k = 0; k < 3; ++k)
			{
				view_to_world.m[0][k] = right[k];
				view_to_world.m[1][k] = up[k];
				view_to_world.m[2][k] = forward[k];
				view_to_world.m[3][k] = cam_pos[k];
			}

			mat4 world_to_view{};
			if (!mat4_invert(view_to_world, world_to_view))
			{
				return false;
			}

			// M = V * P  =>  P = viewToWorld * M.
			const mat4 projection = mat4_multiply(view_to_world, fused);

			if (!mat4_is_finite(projection) || classify_perspective(projection) != 1)
			{
				return false;
			}

			out.view = world_to_view;
			out.projection = projection;
			out.l1_error = mat4_l1_error(mat4_multiply(world_to_view, projection), fused);
			out.used_transpose = false;
			return std::isfinite(out.l1_error);
		}
	}

	// -------------------------------------------------------------------------------------------
	// Matrix utilities
	// -------------------------------------------------------------------------------------------

	mat4 mat4_identity()
	{
		mat4 result{};
		result.m[0][0] = 1.f;
		result.m[1][1] = 1.f;
		result.m[2][2] = 1.f;
		result.m[3][3] = 1.f;
		return result;
	}

	mat4 mat4_zero()
	{
		return mat4{};
	}

	mat4 mat4_multiply(const mat4& a, const mat4& b)
	{
		mat4 result{};

		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				f32 sum = 0.f;
				for (u32 k = 0; k < 4; ++k)
				{
					sum += a.m[i][k] * b.m[k][j];
				}

				result.m[i][j] = sum;
			}
		}

		return result;
	}

	mat4 mat4_transpose(const mat4& a)
	{
		mat4 result{};

		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				result.m[i][j] = a.m[j][i];
			}
		}

		return result;
	}

	bool mat4_invert(const mat4& a, mat4& out)
	{
		const f32* m = &a.m[0][0];
		f32 inv[16]{};

		inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
		inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
		inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
		inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
		inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
		inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
		inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
		inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
		inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
		inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
		inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
		inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
		inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
		inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
		inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
		inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

		f32 det = (m[0] * inv[0]) + (m[1] * inv[4]) + (m[2] * inv[8]) + (m[3] * inv[12]);

		if (!std::isfinite(det) || std::abs(det) < 1e-24f)
		{
			return false;
		}

		det = 1.f / det;

		for (u32 i = 0; i < 16; ++i)
		{
			(&out.m[0][0])[i] = inv[i] * det;
		}

		return mat4_is_finite(out);
	}

	f32 mat4_l1_error(const mat4& a, const mat4& b)
	{
		f32 sum = 0.f;

		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				sum += std::abs(a.m[i][j] - b.m[i][j]);
			}
		}

		return sum;
	}

	bool mat4_is_finite(const mat4& a)
	{
		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				if (!std::isfinite(a.m[i][j]))
				{
					return false;
				}
			}
		}

		return true;
	}

	bool mat4_is_identity(const mat4& a, f32 tol)
	{
		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				const f32 expected = (i == j) ? 1.f : 0.f;
				if (std::abs(a.m[i][j] - expected) > tol)
				{
					return false;
				}
			}
		}

		return true;
	}

	// -------------------------------------------------------------------------------------------
	// Fingerprint
	// -------------------------------------------------------------------------------------------

	vp_fingerprint scan_vertex_program(const RSXVertexProgram& vp)
	{
		vp_fingerprint result{};

		if (vp.data.size() < 4)
		{
			result.note = "empty ucode";
			return result;
		}

		const program_walker prog(vp);

		// Detected before any early return, and independently of the chain walk: this step sits
		// between the bone palette and the outer group, so a program can carry it while still
		// reporting 'innermost operand is not an attribute'. Purely recorded here - nothing reads
		// these fields yet, see vp_fingerprint.
		{
			basis_affine_result basis{};
			const bool matched = match_basis_affine(prog, basis);
			result.basis_reason = basis.reason;

			if (matched)
			{
				result.has_basis_affine = true;
				result.basis_row_slot[0] = basis.row_slot[0];
				result.basis_row_slot[1] = basis.row_slot[1];
				result.basis_scale_slot = basis.scale_slot;
				result.basis_bias_slot = basis.bias_slot;

				// The position decode sits below the basis, and the chain walk below cannot reach
				// it: the basis rows are three single-component DP3 writes and that walk refuses a
				// partial xyz write - correctly, since an axis produced somewhere it is not looking
				// is exactly what it must not fold. So the search runs from the basis's own input
				// instead, which is the temp the decode writes. On ba93cfeb/f7576a48 that lands
				// directly on
				//     9:MAD>r1.xyz(I0.xyzx,C0.wwww,T1.xyzx)c45i0
				// and the MAD arm folds c45.w. Two earlier attempts taught the chain walk to step
				// across the basis piece by piece; this replaces both, because the matcher that
				// parses the basis already knows where it starts.
				if (basis.position_tmp != s_no_temp)
				{
					const_affine_result decode{};
					const bool decoded = match_const_affine(prog, basis.position_tmp, basis.position_before, decode);

					result.affine_reason = decode.reason;

					if (decoded)
					{
						result.has_const_affine = true;
						result.affine_has_scale = decode.has_scale;
						result.affine_has_bias = decode.has_bias;
						result.affine_scale_slot = decode.scale_slot;
						result.affine_scale_component[0] = decode.scale_component[0];
						result.affine_scale_component[1] = decode.scale_component[1];
						result.affine_scale_component[2] = decode.scale_component[2];
						result.affine_bias_slot = decode.bias_slot;
					}
				}
			}
		}

		// Read the texcoord attributes out of the ucode before any early return: a screen_space or
		// unknown program still draws through composite_ui_draw, which resolves texcoords the same
		// way. Once per program, not per draw - the fingerprint is cached by program hash.
		for (u32 unit = 0; unit < 8; ++unit)
		{
			if (u32 attribute = 0; resolve_output_input(prog, 7 + unit, attribute))
			{
				result.texcoord_input[unit] = static_cast<u8>(attribute);
			}

			// Independent of the above: a program whose write resolves straight can still scale,
			// and one that does not resolve is precisely the case this exists for.
			if (u32 attribute = 0, slot = 0; resolve_texcoord_scale_slot(prog, 7 + unit, attribute, slot))
			{
				result.texcoord_scale_slot[unit] = static_cast<u8>(std::min<u32>(slot, 0xfeu));
				result.texcoord_scale_input[unit] = static_cast<u8>(attribute);
			}
		}

		slice_position(prog, 0, result.distinct_consts, result.chain_instructions, result.indexed_const);

		if (result.indexed_const)
		{
			scan_uniform_address(prog, result);
		}

		if (result.chain_instructions == 0)
		{
			result.note = "no HPOS write";
			return result;
		}

		if (result.distinct_consts <= 1)
		{
			result.archetype = vp_archetype::screen_space;
			result.note = "0-1 constants feed HPOS";
			return result;
		}

		// Follow the chain of 4-slot groups back from HPOS towards the vertex attribute.
		chain_result chains[max_transform_groups]{};
		u32 count = 0;
		walk_target target{ true, 0 };
		u32 before = static_cast<u32>(prog.size());
		bool reached_input = false;

		chain_context ctx{};
		ctx.allow_indexed = true;

		// The indirection used to be withheld from a program whose position slice reads an indexed
		// constant, on the grounds that resolving those hands them to the skinning path - which was
		// the right call while that path could not express what they do. It now can
		// (match_indexed_affine), and withholding it is what keeps them out: 16 of Resistance 2's
		// 36 indexed programs park one of their four outer DP4 rows in a register, so without the
		// indirection the outer c8..c11 group does not match either and the walk has nothing at all.
		// vp_hpos_indexed counted that population at 30 on the ae94587 capture.
		ctx.allow_indirect = hpos_indirect_enabled() && (!result.indexed_const || indexed_world_enabled());
		result.hpos_indirect_indexed = hpos_indirect_enabled() && result.indexed_const;

		while (count < max_transform_groups)
		{
			const chain_result chain = find_chain(prog, target, before, ctx);
			result.hpos_indirect_refused = ctx.refused;

			if (!chain.found || (chain.base + 4) > s_legal_constant_slots)
			{
				break;
			}

			result.hpos_indirect |= chain.indirect;
			result.hpos_wbuffer_z |= chain.wbuffer_z;

			if (chain.indexed)
			{
				// A bone palette. It is the innermost group by construction - the address
				// register is loaded from a vertex attribute - so the walk stops here and the
				// palette is handed to Remix as per-instance bone transforms instead of being
				// folded into the object-to-world matrix.
				result.skinned = true;
				result.palette_base = chain.base;
				result.palette_shape = chain.shape;
				result.palette_rows = chain.rows;
				result.palette_stride = chain.stride;
				result.palette_xyz_only = chain.xyz_only;
				result.palette_has_bias = chain.has_bias;
				result.palette_bias_slot = chain.bias_slot;
				result.palette_w_slot = chain.w_slot;
				result.palette_w_component = chain.w_component;
				result.palette_bias_forwarded = chain.bias_forwarded;

				result.skin_blended = chain.blended;
				result.blend_bones = chain.blend_bones;
				result.blend_weight_attribute = chain.blend_weight_attribute;

				for (u32 k = 0; k < max_blend_bones; ++k)
				{
					result.blend_weight_component[k] = chain.blend_weight_component[k];
					result.blend_addr_swz[k] = chain.blend_addr_swz[k];
				}

				// Hardening: prove the whole program only ever indexes this one palette through the
				// address components the matched group accounted for, before anything is submitted.
				// A second ARL (an address register reloaded between palette reads) or a read
				// through a register/component the match did not cover means a contributor nobody
				// has looked at. Refuse, count, draw nothing.
				{
					u32 swz_mask = 0;

					if (chain.blended)
					{
						for (u32 k = 0; k < chain.blend_bones && k < max_blend_bones; ++k)
						{
							swz_mask |= 1u << (chain.blend_addr_swz[k] & 3);
						}
					}
					else
					{
						swz_mask = 1u << (chain.addr_swz & 3);
					}

					const index_audit audit = audit_indexing(prog, chain.addr_reg, swz_mask);

					result.arl_count = audit.arl_count;
					result.indexed_reads = audit.indexed_reads;
					result.foreign_indexed_reads = audit.foreign_reads;

					// Every indexed read in the program has to be one of the ones the group
					// matched. The count is exact rather than a bound: all sixteen observed blend
					// programs read the palette exactly rows x bones = 12 times and nowhere else,
					// so an extra read through a component that happens to be in the mask is a
					// term this has not accounted for.
					const u32 expected_reads = chain.blended ? (chain.rows * chain.blend_bones) : 0;

					if (audit.arl_count > 1 || audit.foreign_reads != 0
						|| (chain.blended && audit.indexed_reads != expected_reads))
					{
						result.skin_unrecognised = true;
						result.skin_note = (audit.arl_count > 1)
							? "more than one ARL: address register is reloaded, so the rig blends"
							: (audit.foreign_reads != 0
								? "palette read through an address register component the match does not cover"
								: "the program indexes the palette more times than the matched group explains");
						result.note = "skinned rig not provably understood";
						return result;
					}
				}

				// One walk per bone. The pairing of weight component to address component is the
				// ucode's, taken from the instruction that reads both (match_blend_row): twelve of
				// the sixteen R2 blend programs load a0 through a swizzle (a0.xyzw <- r1.wzxy), so
				// assuming weight .x selects a0.x would read a different bone than the hardware.
				bool blend_resolved = chain.blended;

				if (chain.blended)
				{
					for (u32 k = 0; k < chain.blend_bones && k < max_blend_bones; ++k)
					{
						if (!resolve_bone_index(prog, chain.addr_reg, chain.blend_addr_swz[k],
								chain.first_instruction, result, &result.blend_bone[k]))
						{
							blend_resolved = false;
							break;
						}

						// Every bone reads the same index attribute - they are four components of
						// one blend-index vector. Four different attributes would mean this is not
						// the shape it was matched as.
						if (result.blend_bone[k].attribute != result.blend_bone[0].attribute)
						{
							result.skin_note = "blend bones read different index attributes";
							blend_resolved = false;
							break;
						}
					}

					// The single-bone fields keep describing bone 0, so every existing reader (the
					// diagnostics, describe_skinning, resolve_indexed_world) stays coherent.
					if (blend_resolved)
					{
						result.bone_attribute = result.blend_bone[0].attribute;
						result.bone_component = result.blend_bone[0].component;
						result.bone_op_count = result.blend_bone[0].op_count;
						result.bone_index_abs = result.blend_bone[0].index_abs;
						result.bone_index_negate = result.blend_bone[0].index_negate;

						for (u32 i = 0; i < result.blend_bone[0].op_count; ++i)
						{
							result.bone_ops[i] = result.blend_bone[0].ops[i];
						}

						result.bone_resolved = true;
					}
				}

				if (blend_resolved || (!chain.blended && resolve_bone_index(prog, chain.addr_reg, chain.addr_swz, chain.first_instruction, result)))
				{
					const chain_source palette_source = resolve_source(prog, chain.source, chain.first_instruction);
					reached_input = (palette_source.reg_type == RSX_VP_REGISTER_TYPE_INPUT);

					if (!reached_input && palette_source.reg_type == RSX_VP_REGISTER_TYPE_TEMP)
					{
						prescale_result prescale{};
						if (match_prescale(prog, palette_source.index, chain.first_instruction, prescale))
						{
							result.has_prescale = true;
							result.prescale_scale_slot = prescale.scale_slot;
							result.prescale_scale_component = prescale.scale_component;
							result.prescale_bias_slot = prescale.bias_slot;
							reached_input = true;
						}
						// ...or the same decompression spelled out as separate instructions, which is
						// the only form Resistance 2 uses: every one of its 16 matchable indexed
						// programs feeds the palette 'attr0.xyz * c18.wwww' with the homogeneous 1
						// moved in separately. Only match_prescale was tried here, so the palette was
						// handed a source that "is not an attribute" and the quantised position would
						// have gone to Remix undecoded even once the palette itself was understood.
						else if (const_affine_result affine{}; match_const_affine(prog, palette_source.index, chain.first_instruction, affine))
						{
							result.has_const_affine = true;
							result.affine_has_scale = affine.has_scale;
							result.affine_has_bias = affine.has_bias;
							result.affine_scale_slot = affine.scale_slot;
							result.affine_scale_component[0] = affine.scale_component[0];
							result.affine_scale_component[1] = affine.scale_component[1];
							result.affine_scale_component[2] = affine.scale_component[2];
							result.affine_bias_slot = affine.bias_slot;
							reached_input = true;
						}
					}
				}

				break;
			}

			chains[count++] = chain;

			const chain_source source = resolve_source(prog, chain.source, chain.first_instruction);

			// Both exits below leave the walk before match_const_affine is ever called, so without
			// these the census reports 'untried' - the initialiser - and the refusal carries no
			// attribution at all. That is the same blind spot 'areason' was added to close one
			// level down, and it hid two of the three refusals in the first capture that had it.
			if (source.reg_type == RSX_VP_REGISTER_TYPE_INPUT)
			{
				// The chain reads the attribute directly: there is no decode step to find, which is
				// a healthy answer rather than a refusal.
				result.affine_reason = "chain-reaches-attr";
				reached_input = true;
				break;
			}

			if (source.reg_type != RSX_VP_REGISTER_TYPE_TEMP)
			{
				// Innermost operand is a constant or an address register, so nothing this matcher
				// understands produced the position.
				result.affine_reason = "source-not-temp";
				break;
			}

			// A quantised attribute is decompressed one step before the first matrix.
			prescale_result prescale{};
			if (match_prescale(prog, source.index, chain.first_instruction, prescale))
			{
				result.has_prescale = true;
				result.prescale_scale_slot = prescale.scale_slot;
				result.prescale_scale_component = prescale.scale_component;
				result.prescale_bias_slot = prescale.bias_slot;
				// Matched by an earlier, tighter shape - const_affine is never consulted, and
				// 'untried' on a decoded program would read as a failure rather than a bypass.
				result.affine_reason = "prescale";
				reached_input = true;
				break;
			}

			// ...or with a per-vertex divisor carried in the attribute's own w. Only ATTR0 is
			// accepted: it is the one attribute this backend submits as the position, so a divide
			// on any other input would describe a vector we never send.
			if (wdivide_result wdivide{}; match_wdivide(prog, source.index, chain.first_instruction, wdivide) && wdivide.attribute == 0)
			{
				result.has_wdivide = true;
				result.affine_reason = "wdivide";
				reached_input = true;
				break;
			}

			// ...or with the same constant scale and bias spelled out as separate instructions,
			// which is how Resistance 2 writes it. Tried last so a program that matches either of
			// the older, tighter shapes keeps matching it.
			// The reason is recorded whether or not the match succeeds - a refusal is the case the
			// census exists to explain, and it is the only path that does not reach the assignments
			// below. Recorded per hop, so what survives is the reason from the innermost operand
			// the walk actually got to.
			// Skipped entirely once the basis path has resolved the decode from the basis's own
			// input: this walk cannot reach past a basis and would only overwrite a good result
			// with its own 'partial-xyz' refusal.
			if (result.has_const_affine)
			{
				reached_input = true;
				break;
			}

			const_affine_result affine{};
			const bool affine_matched = match_const_affine(prog, source.index, chain.first_instruction, affine);
			result.affine_reason = affine.reason;

			if (affine_matched)
			{
				result.has_const_affine = true;
				result.affine_has_scale = affine.has_scale;
				result.affine_has_bias = affine.has_bias;
				result.affine_scale_slot = affine.scale_slot;
				result.affine_scale_component[0] = affine.scale_component[0];
				result.affine_scale_component[1] = affine.scale_component[1];
				result.affine_scale_component[2] = affine.scale_component[2];
				result.affine_bias_slot = affine.bias_slot;
				reached_input = true;
				break;
			}

			target = walk_target{ false, source.index };
			before = chain.first_instruction;
		}

		// Catch-all for the walk exits that reach neither a matcher nor one of the labelled breaks
		// above: no chain found at all, or the group limit reached with temps still to follow.
		// Every refusal that gets a census line should say something; 'untried' surviving to the
		// log means an exit was added without a reason, which is exactly how this gap opened.
		if (std::string_view{result.affine_reason} == "untried")
		{
			result.affine_reason = (count == 0) ? "no-chain" : "walk-exhausted";
		}

		if (count == 0)
		{
			if (result.skinned)
			{
				// The palette is the only group: there is nothing left to be the projection,
				// so there is no camera to anchor this draw against. Skipped, not guessed.
				result.note = "skinned with no outer group";
				return result;
			}

			// A 4x4 transform needs four distinct constant slots by construction (one constant
			// per instruction, digest 1.3). Fewer than that and no chain means the program cannot
			// be transforming a position at all - it is a 2D / pre-projected program.
			if (result.distinct_consts < 4)
			{
				result.archetype = vp_archetype::screen_space;
				result.note = "no matrix chain, <4 constants";

				// ...unless the reason there are fewer than four is that this is a 2D transform.
				// The archetype stays screen_space - the 3D path must keep refusing a program with
				// no depth row - but the 2D compositor can now apply the two rows it does have
				// instead of compositing the raw attribute (has_ortho2d, RemixTransforms.h).
				if (match_ortho2d(prog, result))
				{
					result.note = "2D transform into HPOS.xy";
				}
			}
			else if (result.indexed_const)
			{
				result.note = "indexed constants";
			}
			else
			{
				result.note = "no matrix chain into HPOS";
			}

			return result;
		}

		if (result.skinned && !result.bone_resolved)
		{
			// Recognised the palette but not the index that selects from it. Everything the
			// program draws is skipped; 'skin_note' says which hop failed.
			result.note = "skinned, bone index unresolved";
			return result;
		}

		// chains[0] writes HPOS, so reverse into "innermost first" order.
		result.group_count = count;
		for (u32 i = 0; i < count; ++i)
		{
			result.group_base[i] = chains[count - 1 - i].base;
			result.group_shape[i] = chains[count - 1 - i].shape;
		}

		result.inner_is_input = reached_input;

		if (result.skinned)
		{
			result.archetype = vp_archetype::skinned_layered;
		}
		else
		{
			result.archetype = (count == 1) ? vp_archetype::fused : vp_archetype::layered;
		}

		result.note = reached_input ? "chain reaches the vertex attribute" : "innermost operand is not an attribute";
		return result;
	}

	bool evaluate_bone_offset(const vp_fingerprint& fp, f32 value, u32& out)
	{
		out = 0;

		if (!fp.skinned || !fp.bone_resolved)
		{
			return false;
		}

		f32 v = value;

		for (u32 i = 0; i < fp.bone_op_count; ++i)
		{
			const bone_index_op& op = fp.bone_ops[i];

			switch (op.op)
			{
			case bone_index_op::kind::floor:
			{
				v = std::floor(v);
				break;
			}
			case bone_index_op::kind::immediate_scale:
			{
				// An integer the ucode built out of repeated self-addition, so there is no slot to
				// read back - see bone_index_op::immediate.
				v *= op.immediate;
				break;
			}
			case bone_index_op::kind::scale:
			case bone_index_op::kind::affine:
			{
				if (op.mul_slot != umax)
				{
					f32 factor[4]{};
					if (!read_slot(op.mul_slot, factor))
					{
						return false;
					}

					v *= factor[op.mul_component];
				}

				if (op.op == bone_index_op::kind::affine)
				{
					f32 bias[4]{};
					if (!read_slot(op.add_slot, bias))
					{
						return false;
					}

					v += bias[op.add_component];
				}

				break;
			}
			default:
				return false;
			}
		}

		// The ARL's own source modifiers, in the hardware's order: abs, then negate.
		if (fp.bone_index_abs)
		{
			v = std::abs(v);
		}

		if (fp.bone_index_negate)
		{
			v = -v;
		}

		if (!std::isfinite(v))
		{
			return false;
		}

		// ARL truncates toward zero (both rpcs3 implementations do, whatever the name says).
		const f32 truncated = std::trunc(v);

		if (truncated < 0.f || truncated >= static_cast<f32>(s_legal_constant_slots))
		{
			return false;
		}

		out = static_cast<u32>(truncated);
		return true;
	}

	bool evaluate_uniform_address(const vp_fingerprint& fp, s32& out_offset)
	{
		out_offset = 0;

		if (!fp.indexed_addr_uniform)
		{
			return false;
		}

		f32 slot[4]{};

		if (!read_slot(fp.addr_const_slot, slot))
		{
			return false;
		}

		f32 v = slot[fp.addr_const_component];

		// The ARL's own source modifiers, in the hardware's order: abs, then negate. The same
		// order evaluate_bone_offset applies, and for the same reason - reading them the other way
		// round selects a different constant row.
		if (fp.addr_index_abs)
		{
			v = std::abs(v);
		}

		if (fp.addr_index_negate)
		{
			v = -v;
		}

		if (!std::isfinite(v))
		{
			return false;
		}

		// ARL truncates toward zero, and unlike a palette offset the result may legitimately be
		// negative: it is an offset applied to a base slot, not a slot itself.
		const f32 truncated = std::trunc(v);

		if (std::abs(truncated) >= static_cast<f32>(s_legal_constant_slots))
		{
			return false;
		}

		const s32 offset = static_cast<s32>(truncated);

		// Every read the program makes has to land inside the legal constants. An ARL this code
		// read wrong shows up here as a row outside them, and a row outside them is garbage
		// geometry rather than a refusal.
		if (static_cast<s64>(fp.indexed_base_min) + offset < 0
			|| static_cast<s64>(fp.indexed_base_max) + offset >= s_legal_constant_slots)
		{
			return false;
		}

		out_offset = offset;
		return true;
	}

	bool evaluate_palette_slot(const vp_fingerprint& fp, f32 value, u32& out_slot)
	{
		// Bone 0's path, which for a single-matrix rig is the only one there is. Kept as a wrapper
		// rather than duplicated so a blend bone and a single bone can never disagree about how the
		// index is evaluated - the modifier bug this file already paid for once.
		bone_index_chain chain{};
		chain.resolved = fp.bone_resolved;
		chain.attribute = fp.bone_attribute;
		chain.component = fp.bone_component;
		chain.op_count = fp.bone_op_count;
		chain.index_abs = fp.bone_index_abs;
		chain.index_negate = fp.bone_index_negate;

		for (u32 i = 0; i < fp.bone_op_count && i < max_bone_index_ops; ++i)
		{
			chain.ops[i] = fp.bone_ops[i];
		}

		return evaluate_palette_slot(fp, chain, value, out_slot);
	}

	bool evaluate_palette_slot(const vp_fingerprint& fp, const bone_index_chain& chain, f32 value, u32& out_slot)
	{
		out_slot = 0;

		if (!fp.skinned || !chain.resolved)
		{
			return false;
		}

		f32 v = value;

		for (u32 i = 0; i < chain.op_count && i < max_bone_index_ops; ++i)
		{
			const bone_index_op& op = chain.ops[i];

			switch (op.op)
			{
			case bone_index_op::kind::floor:
			{
				v = std::floor(v);
				break;
			}
			case bone_index_op::kind::immediate_scale:
			{
				// An integer the ucode built out of repeated self-addition, so there is no slot to
				// read back - see bone_index_op::immediate.
				v *= op.immediate;
				break;
			}
			case bone_index_op::kind::scale:
			case bone_index_op::kind::affine:
			{
				if (op.mul_slot != umax)
				{
					f32 factor[4]{};
					if (!read_slot(op.mul_slot, factor))
					{
						return false;
					}

					v *= factor[op.mul_component];
				}

				if (op.op == bone_index_op::kind::affine)
				{
					f32 bias[4]{};
					if (!read_slot(op.add_slot, bias))
					{
						return false;
					}

					v += bias[op.add_component];
				}

				break;
			}
			default:
				return false;
			}
		}

		// The ARL's own source modifiers, in the hardware's order: abs, then negate.
		if (chain.index_abs)
		{
			v = std::abs(v);
		}

		if (chain.index_negate)
		{
			v = -v;
		}

		if (!std::isfinite(v))
		{
			return false;
		}

		// ARL truncates toward zero (both rpcs3 implementations do, whatever the name says).
		const f32 offset = std::trunc(v);

		// Bounded well inside anything f64 cannot represent exactly, so the arithmetic below is
		// exact and a wild index cannot wrap into a legal slot.
		if (std::abs(offset) >= 1e9f)
		{
			return false;
		}

		const f64 slot = static_cast<f64>(fp.palette_base) + static_cast<f64>(offset);

		// The whole group has to be inside the legal constants, not just its first row. A palette
		// read that runs off the end reads whatever the previous draw left there.
		if (slot < 0.0 || (slot + fp.palette_rows) > static_cast<f64>(s_legal_constant_slots))
		{
			return false;
		}

		out_slot = static_cast<u32>(slot);
		return true;
	}

	bool build_palette_matrix(const vp_fingerprint& fp, u32 slot, mat4& out)
	{
		out = mat4_identity();

		if (!fp.skinned || fp.palette_rows == 0 || fp.palette_rows > 4)
		{
			return false;
		}

		// Three rows only carry a point when the operand's fourth coordinate is one. The ucode
		// moves that in from a constant, so it is a live value like everything else here - and a
		// program whose w is a scale factor is a different transform wearing the same instructions.
		if (fp.palette_rows < 4)
		{
			if (fp.palette_w_slot == s_no_palette_w)
			{
				return false;
			}

			f32 w[4]{};

			if (!read_slot(fp.palette_w_slot, w))
			{
				return false;
			}

			if (!std::isfinite(w[fp.palette_w_component & 3]) || std::abs(w[fp.palette_w_component & 3] - 1.f) > 1e-4f)
			{
				return false;
			}
		}

		// The rows the group supplies, in the layout its shape implies. dp4: c[slot + k*stride] is
		// row k of a column-vector matrix, so it lands in column k here. mad: it is row k already.
		for (u32 k = 0; k < fp.palette_rows; ++k)
		{
			f32 row[4]{};

			if (!read_slot(slot + (k * fp.palette_stride), row))
			{
				return false;
			}

			for (u32 j = 0; j < 4; ++j)
			{
				if (!std::isfinite(row[j]))
				{
					return false;
				}

				// A group whose rows only carry xyz has no fourth column of its own; the identity
				// this started from already holds the (0,0,0,1) it needs.
				if (fp.palette_xyz_only && j == 3)
				{
					continue;
				}

				if (fp.palette_shape == chain_shape::dp4)
				{
					out.m[j][k] = row[j];
				}
				else
				{
					out.m[k][j] = row[j];
				}
			}
		}

		// Rows the group does not supply stay identity, which is what the ucode's own separate
		// 'operand.w = 1' amounts to for the missing w row.
		for (u32 k = fp.palette_rows; k < 4; ++k)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				const f32 v = (j == k) ? 1.f : 0.f;

				if (fp.palette_shape == chain_shape::dp4)
				{
					out.m[j][k] = v;
				}
				else
				{
					out.m[k][j] = v;
				}
			}
		}

		// The constant translation the ucode applies after the matrix. In row-vector form a
		// translation is row 3, and it composes on the right of the rows above - which is exactly
		// where the ucode puts it.
		if (fp.palette_has_bias)
		{
			f32 bias[4]{};

			if (!read_slot(fp.palette_bias_slot, bias))
			{
				return false;
			}

			for (u32 j = 0; j < 3; ++j)
			{
				if (!std::isfinite(bias[j]))
				{
					return false;
				}

				out.m[3][j] += bias[j];
			}
		}

		return mat4_is_finite(out);
	}

	const char* archetype_name(vp_archetype a)
	{
		switch (a)
		{
		case vp_archetype::screen_space: return "screen_space";
		case vp_archetype::fused: return "fused";
		case vp_archetype::layered: return "layered";
		case vp_archetype::skinned_layered: return "skinned_layered";
		default: return "unknown";
		}
	}

	// -------------------------------------------------------------------------------------------
	// Fragment-program fingerprint
	// -------------------------------------------------------------------------------------------

	namespace
	{
		// RSX packs FP instructions as big-endian halfwords. Same fixup OPDEST::from_be32
		// (RSXFragmentProgram.h:58) and decode_instruction (Assembler/FPToCFG.cpp:27) apply, and it
		// applies to all four words, not just the dest - is_any_src_constant tests the *raw* src
		// words at bits 8-9 (ProgramStateCache.cpp:579), which are reg_type's bits 0-1 afterwards.
		u32 fp_decode_word(u32 raw)
		{
			return ((raw & 0x00FF00FFu) << 8) | ((raw & 0xFF00FF00u) >> 8);
		}

		// A register the walk tracks: r<n> and h<n> are separate names, exactly as the decompiler
		// declares them (FragmentProgramDecompiler.cpp:259 AddReg). Real hardware aliases h(2k) and
		// h(2k+1) onto r(k); rpcs3's own backends ignore that and render these titles correctly, so
		// this does too rather than invent an aliasing rule no reference implementation uses.
		constexpr u32 fp_reg_id(u32 index, bool fp16)
		{
			return (fp16 ? 0x40u : 0u) | (index & 0x3fu);
		}

		constexpr u32 s_fp_reg_count = 0x80;

		// TEX-family: the instruction *is* the sample. Its sources are the texture coordinate, not a
		// colour, so the walk stops here rather than following them.
		bool fp_op_is_sample(u32 opcode)
		{
			switch (opcode)
			{
			case RSX_FP_OPCODE_TEX:
			case RSX_FP_OPCODE_TXP:
			case RSX_FP_OPCODE_TXD:
			case RSX_FP_OPCODE_TXL:
			case RSX_FP_OPCODE_TXB:
			case RSX_FP_OPCODE_TEXBEM:
			case RSX_FP_OPCODE_TXPBEM:
				return true;
			default:
				return false;
			}
		}

		// How many source slots a colour-preserving opcode reads; 0 for everything else.
		//
		// "Colour-preserving" is the load-bearing definition. These are the operations that combine
		// two colours into a colour - the ones a title uses to modulate, tint, blend or mask a
		// diffuse map on its way to the framebuffer. Deliberately excluded are the operations that
		// turn a sampled vector into a *scalar direction or magnitude*: DP2/DP3/DP4/DP2A, NRM, DST,
		// RCP/RSQ, EX2/LG2/POW, LIT/LIF, REFL, BEM/BEMLUM, DDX/DDY. A texture consumed by one of
		// those is being read as a normal, a gloss exponent or a perturbation - never as albedo.
		//
		// That is the whole discriminator, and it is the same shape as the tangent-space lighting
		// every PS3 title writes: the diffuse map reaches COL0 as 'MUL/MAD albedo, lighting', while
		// the normal map only reaches it through 'DP3 N, L' first. The first path is followed, the
		// second is cut, so the two units separate without knowing anything about their formats.
		u32 fp_colour_sources(u32 opcode)
		{
			switch (opcode)
			{
			case RSX_FP_OPCODE_MOV:
			case RSX_FP_OPCODE_FRC:
			case RSX_FP_OPCODE_FLR:
			case RSX_FP_OPCODE_PK4:
			case RSX_FP_OPCODE_UP4:
			case RSX_FP_OPCODE_PK2:
			case RSX_FP_OPCODE_UP2:
			case RSX_FP_OPCODE_PKB:
			case RSX_FP_OPCODE_UPB:
			case RSX_FP_OPCODE_PK16:
			case RSX_FP_OPCODE_UP16:
			case RSX_FP_OPCODE_PKG:
			case RSX_FP_OPCODE_UPG:
				return 1;
			case RSX_FP_OPCODE_MUL:
			case RSX_FP_OPCODE_ADD:
			case RSX_FP_OPCODE_MIN:
			case RSX_FP_OPCODE_MAX:
			case RSX_FP_OPCODE_SLT:
			case RSX_FP_OPCODE_SGE:
			case RSX_FP_OPCODE_SLE:
			case RSX_FP_OPCODE_SGT:
			case RSX_FP_OPCODE_SNE:
			case RSX_FP_OPCODE_SEQ:
			case RSX_FP_OPCODE_DIV:
				return 2;
			case RSX_FP_OPCODE_MAD:
			case RSX_FP_OPCODE_LRP:
				return 3;
			default:
				return 0;
			}
		}

		bool fp_op_is_flow(u32 opcode)
		{
			switch (opcode)
			{
			case RSX_FP_OPCODE_BRK:
			case RSX_FP_OPCODE_CAL:
			case RSX_FP_OPCODE_IFE:
			case RSX_FP_OPCODE_LOOP:
			case RSX_FP_OPCODE_REP:
			case RSX_FP_OPCODE_RET:
				return true;
			default:
				return false;
			}
		}

		// One decoded fragment instruction, reduced to what the reachability walk needs.
		struct fp_instr
		{
			u32 opcode = 0;
			u32 dest = 0;
			u8 write_mask = 0;
			u8 tex_num = 0;
			bool sample = false;
			bool writes = false;
			u8 source_count = 0;
			u8 source[3] = {};       // register ids, TEMP sources only
			u8 source_valid = 0;     // bit per slot
		};

		// The RSX fragment pipeline allows 4096 slots; no shipped PS3 program comes near this, and a
		// program that does gets 'truncated' rather than a wrong answer.
		constexpr u32 s_max_fp_instructions = 512;
	}

	fp_fingerprint scan_fragment_program(const void* ucode, u32 ucode_length, bool fp32_outputs)
	{
		fp_fingerprint result{};

		if (!ucode || ucode_length < 16)
		{
			result.note = "empty ucode";
			return result;
		}

		const u32 slots = ucode_length / 16;
		const u32* words = static_cast<const u32*>(ucode);

		std::vector<fp_instr> code;
		code.reserve(std::min<u32>(slots, s_max_fp_instructions));

		bool saw_end = false;

		for (u32 slot = 0; slot < slots; ++slot)
		{
			const u32 d0_raw = words[slot * 4 + 0];
			const OPDEST d0{ .HEX = fp_decode_word(d0_raw) };
			const SRC0 s0{ .HEX = fp_decode_word(words[slot * 4 + 1]) };
			const SRC1 s1{ .HEX = fp_decode_word(words[slot * 4 + 2]) };
			const SRC2 s2{ .HEX = fp_decode_word(words[slot * 4 + 3]) };

			// The opcode is seven bits, not six. analyse_fragment_program reads only d0.opcode and
			// therefore never matches a flow-control opcode at all (they all sit at 0x40+); the CFG
			// builder gets it right (Assembler/FPToCFG.cpp:164) and so does this.
			const u32 opcode = u32{d0.opcode} | (u32{s1.opcode_hi} << 6);
			const bool end = !!d0.end;

			if (fp_op_is_flow(opcode))
			{
				result.has_flow = true;
			}

			if (code.size() >= s_max_fp_instructions)
			{
				result.truncated = true;
				break;
			}

			fp_instr in{};
			in.opcode = opcode;
			in.tex_num = static_cast<u8>(d0.tex_num);
			in.sample = fp_op_is_sample(opcode);
			in.write_mask = static_cast<u8>(d0.write_mask);
			in.writes = !d0.no_dest && in.write_mask != 0;
			in.dest = fp_reg_id(d0.dest_reg, !!d0.fp16);

			if (in.sample)
			{
				result.sampled_mask |= static_cast<u16>(1u << d0.tex_num);
			}

			in.source_count = static_cast<u8>(fp_colour_sources(opcode));

			const SRC_Common srcs[3] =
			{
				SRC_Common{ .HEX = s0.HEX },
				SRC_Common{ .HEX = s1.HEX },
				SRC_Common{ .HEX = s2.HEX },
			};

			for (u32 s = 0; s < in.source_count; ++s)
			{
				if (srcs[s].reg_type != RSX_FP_REGISTER_TYPE_TEMP)
				{
					continue;
				}

				in.source[s] = static_cast<u8>(fp_reg_id(srcs[s].tmp_reg_index, !!srcs[s].fp16));
				in.source_valid |= static_cast<u8>(1u << s);
			}

			code.push_back(in);

			// An instruction with a literal operand is followed by a slot of data, not code. Stepped
			// exactly the way analyse_fragment_program steps it (ProgramStateCache.cpp:682) so
			// sampled_mask lands on the same units its referenced_textures_mask does.
			if (s0.reg_type == RSX_FP_REGISTER_TYPE_CONSTANT ||
				s1.reg_type == RSX_FP_REGISTER_TYPE_CONSTANT ||
				s2.reg_type == RSX_FP_REGISTER_TYPE_CONSTANT)
			{
				++slot;
			}

			if (end)
			{
				saw_end = true;
				break;
			}
		}

		result.instructions = static_cast<u32>(code.size());

		if (!saw_end)
		{
			result.truncated = true;
		}

		if (code.empty())
		{
			result.note = "no instructions";
			return result;
		}

		// COL0 is R0 with 32-bit exports and H0 without - rpcs3's own output table,
		// FragmentProgramDecompiler.cpp:64-82 and GL/VK's "ocol0" binding.
		const u32 col0 = fp_reg_id(0, !fp32_outputs);

		// Backward reachability. No kill: a register that is live stays live, because the walk does
		// not follow flow control and a conditional write must not be allowed to retire a live
		// value. That makes the answer an over-approximation, which can only ever add units to
		// colour_mask - i.e. move the caller back towards the lowest-unit guess it already had.
		const auto walk = [&](u8 seed_mask) -> u16
		{
			std::array<u8, s_fp_reg_count> live{};
			live[col0] = seed_mask;

			u16 colour = 0;

			// Straight-line code resolves in one reverse pass; flow control can put a def after its
			// use in program order, so iterate to a fixpoint. Bounded because 'live' only grows.
			for (u32 round = 0; round < 8; ++round)
			{
				bool changed = false;

				for (u32 i = static_cast<u32>(code.size()); i-- > 0;)
				{
					const fp_instr& in = code[i];

					if (!in.writes || !(live[in.dest] & in.write_mask))
					{
						continue;
					}

					if (in.sample)
					{
						colour |= static_cast<u16>(1u << in.tex_num);
						continue;
					}

					for (u32 s = 0; s < in.source_count; ++s)
					{
						if (!(in.source_valid & (1u << s)))
						{
							continue;
						}

						// Swizzles let any source component reach any destination one, so a live
						// source is live in all four channels. Conservative in the safe direction.
						if (live[in.source[s]] != 0xf)
						{
							live[in.source[s]] = 0xf;
							changed = true;
						}
					}
				}

				if (!changed)
				{
					break;
				}
			}

			return colour;
		};

		// COL0.rgb first: a unit that only ever reaches the alpha channel is a cutout or gloss
		// source, not the surface colour. Falling back to rgba rather than refusing outright keeps
		// the programs that write their colour through the w channel (alpha-blended decals) from
		// dropping to the guess for no reason.
		result.colour_mask = static_cast<u16>(walk(0x7) & result.sampled_mask);

		if (result.colour_mask == 0)
		{
			result.colour_mask = static_cast<u16>(walk(0xf) & result.sampled_mask);
			result.alpha_only = result.colour_mask != 0;
		}

		if (result.colour_mask == 0)
		{
			result.note = result.sampled_mask ? "no sample reaches COL0" : "no sampled units";
		}
		else if (result.colour_mask == result.sampled_mask)
		{
			result.note = "every sampled unit reaches COL0";
		}
		else
		{
			result.note = "ok";
		}

		return result;
	}

	std::string describe_position_slice(const RSXVertexProgram& vp, u32 max_instructions)
	{
		return describe_output_slice(vp, 0, max_instructions);
	}

	std::string describe_output_slice(const RSXVertexProgram& vp, u32 output_index, u32 max_instructions)
	{
		if (vp.data.size() < 4)
		{
			return "<empty>";
		}

		const program_walker prog(vp);

		u32 consts = 0;
		u32 instructions = 0;
		bool indexed = false;
		std::vector<u32> indices;
		slice_position(prog, output_index, consts, instructions, indexed, &indices);

		if (instructions == 0)
		{
			return " <not written>";
		}

		std::string out;
		u32 emitted = 0;

		for (const u32 i : indices)
		{
			if (emitted >= max_instructions)
			{
				out += " ...";
				break;
			}

			const decoded_instr& in = prog[i];

			fmt::append(out, " %u:", i);

			// VEC half.
			if (in.d1.vec_opcode != RSX_VEC_OPCODE_NOP)
			{
				out += rsx_vp_vec_op_names[in.d1.vec_opcode < std::size(rsx_vp_vec_op_names) ? in.d1.vec_opcode : 0];

				if (in.d0.vec_result && in.d3.dst != s_no_output)
				{
					fmt::append(out, ">o%u.", u32{in.d3.dst});
				}
				else
				{
					fmt::append(out, ">r%u.", u32{in.d0.dst_tmp});
				}

				append_mask(out, vec_writemask(in));
			}

			// SCA half.
			if (in.d1.sca_opcode != RSX_SCA_OPCODE_NOP)
			{
				out += '/';
				out += rsx_vp_sca_op_names[in.d1.sca_opcode < std::size(rsx_vp_sca_op_names) ? in.d1.sca_opcode : 0];

				if (!in.d0.vec_result && in.d3.dst != s_no_output)
				{
					fmt::append(out, ">o%u.", u32{in.d3.dst});
				}
				else
				{
					fmt::append(out, ">r%u.", u32{in.d3.sca_dst_tmp});
				}

				append_mask(out, sca_writemask(in));
			}

			const char comps[] = "xyzw";

			// The '-' is SRC::neg, and it is not cosmetic: 'a.yzx*b.zxy + a.zxy*b.yzx' and
			// 'a.yzx*b.zxy - a.zxy*b.yzx' are a sum and a cross product, and only the second is a
			// rotation basis. Without this the two read identically here, so a matcher written
			// against the printed slice would pick the wrong handedness and mirror the geometry.
			const auto sign = [](const SRC& s) { return s.neg ? "-" : ""; };

			fmt::append(out, "(%s%s%u.%c%c%c%c,%s%s%u.%c%c%c%c,%s%s%u.%c%c%c%c)",
				sign(in.src[0]), source_kind(in.src[0]), u32{in.src[0].tmp_src},
				comps[in.src[0].swz_x], comps[in.src[0].swz_y], comps[in.src[0].swz_z], comps[in.src[0].swz_w],
				sign(in.src[1]), source_kind(in.src[1]), u32{in.src[1].tmp_src},
				comps[in.src[1].swz_x], comps[in.src[1].swz_y], comps[in.src[1].swz_z], comps[in.src[1].swz_w],
				sign(in.src[2]), source_kind(in.src[2]), u32{in.src[2].tmp_src},
				comps[in.src[2].swz_x], comps[in.src[2].swz_y], comps[in.src[2].swz_z], comps[in.src[2].swz_w]);

			fmt::append(out, "c%u", u32{in.d1.const_src});

			if (in.d3.index_const)
			{
				out += "[a]";
			}

			fmt::append(out, "i%u", u32{in.d1.input_src});

			++emitted;
		}

		return out;
	}

	const char* shape_name(chain_shape s)
	{
		switch (s)
		{
		case chain_shape::dp4: return "dp4";
		case chain_shape::mad: return "mad";
		default: return "none";
		}
	}

	// -------------------------------------------------------------------------------------------
	// Slots and conventions
	// -------------------------------------------------------------------------------------------

	bool read_slot_block(u32 base, slot_block& out)
	{
		if ((base + 4) > s_legal_constant_slots)
		{
			return false;
		}

		for (u32 i = 0; i < 4; ++i)
		{
			const u32* raw = rsx::method_registers.transform_constants[base + i];
			for (u32 c = 0; c < 4; ++c)
			{
				out.v[i][c] = std::bit_cast<f32>(raw[c]);
			}
		}

		return true;
	}

	bool read_slot(u32 index, f32 (&out)[4])
	{
		if (index >= s_legal_constant_slots)
		{
			return false;
		}

		const u32* raw = rsx::method_registers.transform_constants[index];
		for (u32 c = 0; c < 4; ++c)
		{
			out[c] = std::bit_cast<f32>(raw[c]);
		}

		return true;
	}

	mat4 slots_to_matrix(const slot_block& slots, chain_shape shape)
	{
		mat4 result{};

		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				// dp4: c[N+i] is row i of a column-vector matrix, so the row-vector form is
				// its transpose. mad: c[N+k] is column k, which IS the row-vector form.
				result.m[i][j] = (shape == chain_shape::dp4) ? slots.v[j][i] : slots.v[i][j];
			}
		}

		return result;
	}

	bool build_ortho2d(const vp_fingerprint& fp, mat4& out)
	{
		if (!fp.has_ortho2d || !ortho2d_enabled())
		{
			return false;
		}

		f32 row_x[4]{};
		f32 row_y[4]{};

		if (!read_slot(fp.ortho2d_slot_x, row_x) || !read_slot(fp.ortho2d_slot_y, row_y))
		{
			return false;
		}

		for (u32 i = 0; i < 4; ++i)
		{
			if (!std::isfinite(row_x[i]) || !std::isfinite(row_y[i]))
			{
				return false;
			}
		}

		// dp4: clip_i = dot(c[slot_i], pos), so c[slot_i] is row i of a column-vector matrix and
		// the row-vector form is its transpose - the same rule slots_to_matrix applies for the
		// chain_shape::dp4 case. Only columns 0 and 1 are known; z and w pass through, which is
		// all the compositor reads.
		out = mat4_identity();

		for (u32 j = 0; j < 4; ++j)
		{
			out.m[j][0] = row_x[j];
			out.m[j][1] = row_y[j];
		}

		return true;
	}

	bool build_prescale(const vp_fingerprint& fp, mat4& out)
	{
		if (fp.has_const_affine && position_affine_enabled())
		{
			// Per-axis scale, so a diagonal rather than the uniform one below. Both halves are
			// optional: R2's ca526d308f1650bb scales and does not bias, its c87769e09c995db9
			// biases and does not scale.
			out = mat4_identity();

			if (fp.affine_has_scale)
			{
				f32 slot[4]{};

				if (!read_slot(fp.affine_scale_slot, slot))
				{
					return false;
				}

				for (u32 i = 0; i < 3; ++i)
				{
					const f32 s = slot[fp.affine_scale_component[i] & 3];

					// A zero or non-finite axis collapses the mesh into a plane or deletes it.
					// Refuse the whole decode rather than submit two thirds of it.
					if (!std::isfinite(s) || std::abs(s) < 1e-12f)
					{
						return false;
					}

					out.m[i][i] = s;
				}
			}

			if (fp.affine_has_bias)
			{
				f32 slot[4]{};

				if (!read_slot(fp.affine_bias_slot, slot))
				{
					return false;
				}

				for (u32 i = 0; i < 3; ++i)
				{
					if (!std::isfinite(slot[i]))
					{
						return false;
					}

					out.m[3][i] = slot[i];
				}
			}

			return fp.affine_has_scale || fp.affine_has_bias;
		}

		if (!fp.has_prescale)
		{
			return false;
		}

		f32 scale_slot[4]{};
		f32 bias_slot[4]{};

		if (!read_slot(fp.prescale_scale_slot, scale_slot) || !read_slot(fp.prescale_bias_slot, bias_slot))
		{
			return false;
		}

		const f32 scale = scale_slot[fp.prescale_scale_component & 3];

		if (!std::isfinite(scale) || std::abs(scale) < 1e-12f)
		{
			return false;
		}

		out = mat4_identity();
		out.m[0][0] = scale;
		out.m[1][1] = scale;
		out.m[2][2] = scale;
		out.m[3][0] = bias_slot[0];
		out.m[3][1] = bias_slot[1];
		out.m[3][2] = bias_slot[2];
		return true;
	}

	remixapi_Transform to_remix_transform(const mat4& m)
	{
		remixapi_Transform result{};

		for (u32 i = 0; i < 3; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				// remixapi_Transform is column-vector: out_i = sum_j matrix[i][j] * in_j.
				result.matrix[i][j] = m.m[j][i];
			}
		}

		return result;
	}

	void to_camera_matrix(const mat4& m, f32 (&out)[4][4])
	{
		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				out[i][j] = m.m[i][j];
			}
		}
	}

	mat4 fold_viewport_z(const mat4& p, f32 scale_z, f32 offset_z)
	{
		mat4 result = p;

		for (u32 k = 0; k < 4; ++k)
		{
			result.m[k][2] = (scale_z * p.m[k][2]) + (offset_z * p.m[k][3]);
		}

		return result;
	}

	// -------------------------------------------------------------------------------------------
	// Classifiers
	// -------------------------------------------------------------------------------------------

	int classify_perspective(const mat4& mat)
	{
		constexpr f32 tol = 0.02f;
		constexpr f32 jitter_tol = 0.35f;

		const auto& m = mat.m;

		if (!mat4_is_finite(mat))
		{
			return 0;
		}

		if (std::abs(m[0][1]) > tol || std::abs(m[0][3]) > tol)
		{
			return 0;
		}

		if (std::abs(m[1][0]) > tol || std::abs(m[1][3]) > tol)
		{
			return 0;
		}

		if (std::abs(m[0][0]) < 0.1f || std::abs(m[1][1]) < 0.1f)
		{
			return 0;
		}

		if (std::abs(std::abs(m[2][3]) - 1.f) < tol && std::abs(m[3][3]) < tol)
		{
			if (std::abs(m[0][2]) > tol || std::abs(m[1][2]) > tol)
			{
				return 0;
			}

			if (std::abs(m[3][0]) > tol || std::abs(m[3][1]) > tol)
			{
				return 0;
			}

			return 1;
		}

		if (std::abs(std::abs(m[3][2]) - 1.f) < tol && std::abs(m[3][3]) < tol)
		{
			if (std::abs(m[0][2]) > jitter_tol || std::abs(m[1][2]) > jitter_tol)
			{
				return 0;
			}

			if (std::abs(m[2][0]) > tol || std::abs(m[2][1]) > tol)
			{
				return 0;
			}

			if (std::abs(m[3][0]) > tol || std::abs(m[3][1]) > tol)
			{
				return 0;
			}

			return 2;
		}

		return 0;
	}

	bool is_orthographic(const mat4& mat)
	{
		const auto& m = mat.m;

		return std::abs(m[0][3]) < 1e-5f
			&& std::abs(m[1][3]) < 1e-5f
			&& std::abs(m[2][3]) < 1e-5f
			&& std::abs(m[3][3] - 1.f) < 1e-5f;
	}

	bool describe_projection(const mat4& mat, projection_params& out)
	{
		const auto& m = mat.m;

		const f32 x_scale = std::abs(m[0][0]);
		const f32 y_scale = std::abs(m[1][1]);

		if (!(x_scale > 1e-4f) || !(y_scale > 1e-4f))
		{
			return false;
		}

		out.fov_y_degrees = static_cast<f32>(2.0 * std::atan(1.0 / static_cast<f64>(y_scale)) * (180.0 / 3.14159265358979323846));
		out.aspect = y_scale / x_scale;

		out.near_plane = (std::abs(m[2][2]) > 1e-6f) ? std::abs(m[3][2] / m[2][2]) : 0.f;

		return std::isfinite(out.fov_y_degrees) && std::isfinite(out.aspect);
	}

	f32 score_perspective(const mat4& mat, f32 reference_aspect)
	{
		if (classify_perspective(mat) != 1)
		{
			return 0.f;
		}

		projection_params params{};
		if (!describe_projection(mat, params))
		{
			return 0.f;
		}

		if (params.fov_y_degrees < 15.f || params.fov_y_degrees > 150.f)
		{
			return 0.f;
		}

		// Cube-map faces and shadow cascades are square while the scene camera matches the display.
		if (reference_aspect > 1.1f && std::abs(params.aspect - 1.f) < 0.02f)
		{
			return 0.f;
		}

		f32 score = 1.f;

		score += (params.fov_y_degrees >= 30.f && params.fov_y_degrees <= 120.f) ? 2.f : 1.f;

		const f32 aspect_delta = std::abs(params.aspect - reference_aspect);
		if (aspect_delta < 0.15f)
		{
			score += 2.f;
		}
		else if (aspect_delta < 0.5f)
		{
			score += 1.f;
		}

		if (params.near_plane > 0.001f && params.near_plane < 100.f)
		{
			score += 1.f;
		}

		if (mat.m[0][0] < 0.f)
		{
			score -= 0.5f;
		}

		if (mat.m[1][1] < 0.f)
		{
			score -= 1.f;
		}

		return score;
	}

	bool is_affine(const mat4& mat, f32 tol)
	{
		const auto& m = mat.m;

		return std::abs(m[0][3]) < tol
			&& std::abs(m[1][3]) < tol
			&& std::abs(m[2][3]) < tol
			&& std::abs(m[3][3] - 1.f) < tol;
	}

	bool has_usable_basis(const mat4& mat, f32 tol)
	{
		const auto& m = mat.m;

		// Rows 0..2 are the basis vectors: this file is row-vector throughout, so p' = p * M sends
		// the unit x axis to row 0.
		f32 length[3]{};

		for (u32 i = 0; i < 3; ++i)
		{
			f32 sum = 0.f;

			for (u32 j = 0; j < 3; ++j)
			{
				if (!std::isfinite(m[i][j]))
				{
					return false;
				}

				sum += m[i][j] * m[i][j];
			}

			length[i] = std::sqrt(sum);

			// A zero-length axis collapses the mesh into a plane on its own.
			if (!(length[i] > 1e-12f))
			{
				return false;
			}
		}

		const f32 det =
			  m[0][0] * ((m[1][1] * m[2][2]) - (m[1][2] * m[2][1]))
			- m[0][1] * ((m[1][0] * m[2][2]) - (m[1][2] * m[2][0]))
			+ m[0][2] * ((m[1][0] * m[2][1]) - (m[1][1] * m[2][0]));

		if (!std::isfinite(det))
		{
			return false;
		}

		// |det| is the volume the three axes enclose; their length product is the volume they would
		// enclose if they were mutually perpendicular. The ratio is 1 for any rotation with any
		// uniform scale and 0 when they are coplanar, so the test says "not flattened" independently
		// of how large the transform is.
		return std::abs(det) >= (tol * length[0] * length[1] * length[2]);
	}

	f32 basis_extent(const mat4& mat)
	{
		const auto& m = mat.m;
		f32 longest = 0.f;

		for (u32 i = 0; i < 3; ++i)
		{
			f32 sum = 0.f;

			for (u32 j = 0; j < 3; ++j)
			{
				if (!std::isfinite(m[i][j]))
				{
					return std::numeric_limits<f32>::infinity();
				}

				sum += m[i][j] * m[i][j];
			}

			longest = std::max(longest, std::sqrt(sum));
		}

		return longest;
	}

	f32 translation_extent(const mat4& mat)
	{
		const auto& m = mat.m;
		f32 sum = 0.f;

		for (u32 j = 0; j < 3; ++j)
		{
			if (!std::isfinite(m[3][j]))
			{
				return std::numeric_limits<f32>::infinity();
			}

			sum += m[3][j] * m[3][j];
		}

		return std::sqrt(sum);
	}

	bool split_view_projection(const mat4& fused, vp_split& out)
	{
		if (!mat4_is_finite(fused))
		{
			return false;
		}

		vp_split direct{};
		const bool have_direct = try_split_once(fused, direct);

		vp_split transposed{};
		bool have_transposed = try_split_once(mat4_transpose(fused), transposed);
		if (have_transposed)
		{
			transposed.used_transpose = true;
		}

		if (have_direct && have_transposed)
		{
			out = (direct.l1_error <= transposed.l1_error) ? direct : transposed;
			return true;
		}

		if (have_direct)
		{
			out = direct;
			return true;
		}

		if (have_transposed)
		{
			out = transposed;
			return true;
		}

		return false;
	}

	std::string format_matrix(const mat4& m)
	{
		std::string result = "[";

		for (u32 i = 0; i < 4; ++i)
		{
			if (i)
			{
				result += " | ";
			}

			for (u32 j = 0; j < 4; ++j)
			{
				if (j)
				{
					result += ' ';
				}

				fmt::append(result, "%.5g", static_cast<f64>(m.m[i][j]));
			}
		}

		result += ']';
		return result;
	}

	std::string format_slots(const slot_block& s)
	{
		std::string result = "[";

		for (u32 i = 0; i < 4; ++i)
		{
			if (i)
			{
				result += " | ";
			}

			for (u32 j = 0; j < 4; ++j)
			{
				if (j)
				{
					result += ' ';
				}

				fmt::append(result, "%.5g", static_cast<f64>(s.v[i][j]));
			}
		}

		result += ']';
		return result;
	}

	bool dump_enabled()
	{
		static const bool env = env_flag(L"RPCS3_REMIX_DUMP");
		return env || g_cfg.video.remix.dump;
	}

	bool keep_ui_enabled()
	{
		static const bool value = env_flag(L"RPCS3_REMIX_KEEP_UI");
		return value;
	}

	bool nocam_enabled()
	{
		static const bool value = env_flag(L"RPCS3_REMIX_NOCAM");
		return value;
	}

	bool noskin_enabled()
	{
		static const bool value = env_flag(L"RPCS3_REMIX_NOSKIN");
		return value;
	}

	u32 camera_hold_frames()
	{
		// umax is the "unset" sentinel, not a fallback value: 0 is a legitimate setting here (it
		// restores the old per-frame latch), so it cannot double as "environment not set".
		static const u32 env = env_u32(L"RPCS3_REMIX_CAMHOLD", umax);
		return env != umax ? env : g_cfg.video.remix.camera_hold;
	}

	u32 mesh_idle_frames()
	{
		// How long a mesh handle survives unreferenced. The default 300 (~5 s at 60 fps) is short
		// enough that a title whose level assets cycle out of view for a corridor's length pays the
		// BLAS build again every time they come back; raising it trades host/VRAM residency for
		// that churn. The mesh *cap* (Live Mesh Cap) is the ceiling side and is unaffected - this
		// only moves the age at which an unreferenced mesh becomes eligible.
		// umax is the "unset" sentinel rather than a fallback because the environment variable can
		// express 0 - reap everything the frame it stops being drawn - which is a useful bisect
		// even though the config floor is 30. Exactly as camera_hold_frames argues.
		static const u32 env = env_u32(L"RPCS3_REMIX_MESHIDLE", umax);
		return env != umax ? env : g_cfg.video.remix.mesh_idle;
	}

	bool draw_without_world()
	{
		static const bool env = env_flag(L"RPCS3_REMIX_DRAWNOWORLD");
		return env || g_cfg.video.remix.draw_without_world;
	}

	bool nowdivide_enabled()
	{
		static const bool env = env_flag(L"RPCS3_REMIX_NOWDIV");
		return env || g_cfg.video.remix.no_w_divide;
	}

	u32 rt_feedback_max_vertices()
	{
		static const u32 env = env_u32(L"RPCS3_REMIX_RTVERTS", 0);
		return env ? env : g_cfg.video.remix.render_target_verts;
	}

	bool strict_input_enabled()
	{
		static const bool env = env_flag(L"RPCS3_REMIX_STRICTINPUT");
		return env || g_cfg.video.remix.strict_input;
	}

	bool texcoords_disabled()
	{
		static const bool value = env_flag(L"RPCS3_REMIX_NOUV");
		return value;
	}

	u32 texcoord_attribute()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_UVATTR", 0);
		return value;
	}

	bool texcoord_from_ucode()
	{
		// env_u32 rather than env_flag: the useful setting is the *off* one, and env_flag cannot
		// tell "set to 0" from "not set at all".
		static const u32 value = env_u32(L"RPCS3_REMIX_UVUCODE", 1);
		return value != 0;
	}

	bool fp_albedo_enabled()
	{
		// env_u32 rather than env_flag, same reason texcoord_from_ucode gives: the useful setting is
		// the off one.
		static const u32 value = env_u32(L"RPCS3_REMIX_FPALBEDO", 1);
		return value != 0;
	}

	bool texcoord_scale_from_ucode()
	{
		// On by default and the useful setting is the off one, so env_u32 rather than env_flag.
		static const u32 value = env_u32(L"RPCS3_REMIX_UVSCALEUCODE", 1);
		return value != 0;
	}

	bool texcoord_scale_temp_form()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_UVSCALETEMP", 1);
		return value != 0;
	}

	bool texcoord_scale_mad_form()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_UVSCALEMAD", 1);
		return value != 0;
	}

	bool pick_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_PICK", 1);
		return value != 0;
	}

	bool sky_camera_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_SKYCAM", 1);
		return value != 0;
	}

	f32 world_affine_tolerance()
	{
		// Scaled by 1000 so it can travel as an integer env var: 20 is the historical 0.02.
		static const u32 milli = env_u32(L"RPCS3_REMIX_AFFINETOL", 20);
		return static_cast<f32>(milli) / 1000.f;
	}

	bool sky_learn_dome_enabled()
	{
		// On by default and the useful setting is the off one, so env_u32 rather than env_flag -
		// same reasoning fp_albedo_enabled gives.
		static const u32 value = env_u32(L"RPCS3_REMIX_SKYLEARN", 1);
		return value != 0;
	}

	bool retry_unsupported_enabled()
	{
		// Shipped on, then measured off. The rule was sound in principle - a unit refused for its
		// format holds no albedo to lose, so walking past it cannot be the substitution the guard
		// prevents - but on Haze the *next* unit is frequently not the diffuse map either. One
		// in-world capture: tex_unit_substituted went 4 -> 3445 as the walk widened, and the
		// visible result was character face textures painted onto tree trunks. It also never did
		// the job it was added for: tex_none stayed at 790122 of 2939973 submitted, because for
		// ~99% of the walks no higher unit yields a material at all.
		//
		// So this defers to the principle the guard was written on and this backend states
		// everywhere else - a missing texture is better than a wrong one. Off by default;
		// RPCS3_REMIX_RETRYUNSUP=1 restores the walk. Counter: tex_retry_unsupported.
		static const u32 value = env_u32(L"RPCS3_REMIX_RETRYUNSUP", 0);
		return value != 0;
	}

	bool position_affine_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_POSAFFINE", 1);
		return value != 0;
	}

	bool pass_skip_enabled()
	{
		// env_u32 rather than env_flag: the useful setting is the *off* one, and env_flag cannot
		// tell "set to 0" from "not set at all".
		static const u32 value = env_u32(L"RPCS3_REMIX_PASSSKIP", 1);
		return value != 0;
	}

	bool ortho2d_enabled()
	{
		// env_u32 rather than env_flag: the useful setting is the *off* one, and env_flag cannot
		// tell "set to 0" from "not set at all".
		static const u32 value = env_u32(L"RPCS3_REMIX_ORTHO2D", 1);
		return value != 0;
	}

	bool full_chain_enabled()
	{
		// env_u32 rather than env_flag, same reason ortho2d_enabled gives: the useful setting is
		// the *off* one. Diagnostic bisect class, so no config row - the default is the fix and
		// =0 is the A/B that brings the old placement back.
		static const u32 value = env_u32(L"RPCS3_REMIX_FULLCHAIN", 1);
		return value != 0;
	}

	bool hpos_indirect_enabled()
	{
		// env_u32 rather than env_flag, same reason ortho2d_enabled gives: the useful setting is
		// the *off* one, and =0 is the A/B that puts the 33 recovered R2 programs back on the
		// origin.
		static const u32 value = env_u32(L"RPCS3_REMIX_HPOSINDIRECT", 1);
		return value != 0;
	}

	bool texcoord_wide_scan()
	{
		// env_u32 rather than env_flag: the useful setting here is the *off* one, and env_flag
		// cannot tell "set to 0" from "not set at all".
		static const u32 value = env_u32(L"RPCS3_REMIX_UVWIDE", 1);
		return value != 0;
	}

	bool texcoord_flip_v()
	{
		static const bool env = env_flag(L"RPCS3_REMIX_UVFLIPV");
		return env || g_cfg.video.remix.flip_texcoord_v;
	}

	u32 texcoord_int_scale()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_UVINTSCALE", 4096);
		return value;
	}

	bool loose_slice_enabled()
	{
		static const bool value = env_flag(L"RPCS3_REMIX_LOOSESLICE");
		return value;
	}

	bool draw_indexed_const()
	{
		static const bool value = env_flag(L"RPCS3_REMIX_DRAWINDEXED");
		return value;
	}

	bool indexed_world_enabled()
	{
		// env_u32 rather than env_flag: the useful setting here is the *off* one, and env_flag
		// cannot tell "set to 0" from "not set at all".
		static const u32 value = env_u32(L"RPCS3_REMIX_INDEXEDWORLD", 1);
		return value != 0;
	}

	bool indexed_bias_reg_enabled()
	{
		static const bool value = env_flag(L"RPCS3_REMIX_INDEXEDBIASREG");
		return value;
	}

	u32 indexed_uniform_mode()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_INDEXEDUNIFORM", 0);
		return value;
	}

	bool basis_affine_enabled()
	{
		// env_u32 rather than env_flag: the useful setting is the off one, and env_flag cannot tell
		// "set to 0" from "not set at all". On by default - a program that matches carries a proven
		// orthonormal right-handed basis (match_basis_affine refuses anything it cannot prove is a
		// cross product), and without it these draws reach the outer group unplaced.
		static const u32 value = env_u32(L"RPCS3_REMIX_BASISAFFINE", 1);
		return value != 0;
	}

	bool bone_blend_enabled()
	{
		// env_u32 rather than env_flag: the useful setting here is the *off* one, and env_flag
		// cannot tell "set to 0" from "not set at all". Off restores the ae94587 behaviour, where
		// Resistance 2's (NPEA00431) 16 four-bone character programs took 541960 draws into
		// skin_unrecognised and no character was drawn at all - see RemixTransforms.h for the list.
		static const u32 value = env_u32(L"RPCS3_REMIX_BONEBLEND", 1);
		return value != 0;
	}

	bool bone_scale_gate_enabled()
	{
		// env_u32 rather than env_flag: the useful setting is the *off* one, and env_flag cannot
		// tell "set to 0" from "not set at all".
		static const u32 value = env_u32(L"RPCS3_REMIX_BONESCALE", 1);
		return value != 0;
	}

	bool bone_uniform_allowed()
	{
		// env_u32 rather than env_flag, same reason: the useful setting is the *off* one.
		static const u32 value = env_u32(L"RPCS3_REMIX_BONEUNIFORM", 1);
		return value != 0;
	}

	f32 skin_reach_ratio()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_SKINREACH", 32);
		return static_cast<f32>(value);
	}

	f32 vertex_spread_ratio()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_VTXSPREAD", 64);
		return static_cast<f32>(value);
	}

	bool vertex_spread_refuse()
	{
		static const bool value = env_flag(L"RPCS3_REMIX_VTXREFUSE");
		return value;
	}

	f32 streak_extent_ratio()
	{
		// env_u32 rather than env_flag: 0 is the meaningful setting (it restores 81af315) and
		// env_flag cannot tell "set to 0" from "not set at all".
		static const u32 value = env_u32(L"RPCS3_REMIX_STREAKGATE", 128);
		return static_cast<f32>(value);
	}

	bool wbuffer_z_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_WBUFFERZ", 1);
		return value != 0;
	}

	bool alpha_state_disabled()
	{
		static const bool env = env_flag(L"RPCS3_REMIX_NOALPHA");
		return env || g_cfg.video.remix.no_alpha_test;
	}

	bool skinid_enabled()
	{
		static const bool value = env_flag(L"RPCS3_REMIX_SKINID");
		return value;
	}

	u32 skinbone_index()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_SKINBONE", umax);
		return value;
	}

	bool skinraw_enabled()
	{
		static const bool value = env_flag(L"RPCS3_REMIX_SKINRAW");
		return value;
	}

	u64 skip_vp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_SKIPVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();

		return value;
	}

	u64 world_vp_hash()
	{
		// Same 16-hex-digit parse as skip_vp_hash, and deliberately a separate variable: the two
		// are used together (SKIPVP names the program by making it disappear, WORLDVP then asks
		// what transform that program was being given), so one name cannot serve both.
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_WORLDVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();

		return value;
	}

	bool cull_from_rsx()
	{
		static const bool env = env_flag(L"RPCS3_REMIX_CULL");
		return env || g_cfg.video.remix.cull_from_rsx;
	}

	bool vertex_colour_disabled()
	{
		static const bool env = env_flag(L"RPCS3_REMIX_NOVCOL");
		return env || g_cfg.video.remix.no_vertex_colour;
	}

	// The environment variable is latched once and wins when set; otherwise the value is read
	// live from the emulator config, so these take effect without a restart. A negative sentinel
	// distinguishes "unset" from a legitimate zero, which several of these accept.
	f32 sky_min_extent()
	{
		static const f32 env = env_float(L"RPCS3_REMIX_SKYEXTENT", -1.f);
		return env >= 0.f ? env : static_cast<f32>(g_cfg.video.remix.sky_extent);
	}

	f32 sky_max_anchor()
	{
		// Default 4.0 world units. The two titles on record place their dome's origin at anchor
		// 0.000 (Haze fc0fac8afccec49a, origin on the eye) and 2.404 (R2 41c59a3a2bfc71bf and
		// c1781a2e32aba35d, origin on the ground below the eye); the nearest draw that clears
		// depth-write and sky_min_extent() without being a dome sits at 96.91. 4.0 is the round
		// number between 2.404 and 96.91, not a tuned one - the gap is 40x wide.
		//
		// Negative sentinel for "unset" like the other float knobs, and 0 is a legitimate value
		// meaning "do not require the anchor at all" (the extent-only rule of ae94587).
		static const f32 env = env_float(L"RPCS3_REMIX_SKYANCHOR", -1.f);
		return env >= 0.f ? env : 4.f;
	}

	u32 sky_backdrop_mode()
	{
		// Off by default. Mode 1 is the one to run first: it counts what mode 2 would tag and
		// leaves the image alone, which is the measurement the dome rule never had before it was
		// widened - and widening it blind is how cat_sky reached 825915 of 2038738 draws.
		// Defaulted to 1 - measure, tag nothing. Mode 1 is image-identical by construction: it
		// evaluates the rule and increments sky_backdrop_hit / sky_backdrop_dw without touching
		// categoryFlags, so the shipped render path is unchanged and the only cost is one AABB
		// test per candidate. It is on by default because the alternative is an environment
		// variable, and two captures in this project were already lost to a knob that survived in
		// a shell after the run it was set for - a measurement nobody can take is worth less than
		// a counter that is always there. Mode 2 (tag) stays opt-in and must not be armed until
		// sky_backdrop_hit / draws_submitted comes back a fraction of a percent: the regression
		// this rule exists to avoid measured 825915 of 2038738 draws, i.e. 40.5%.
		static const u32 value = env_u32(L"RPCS3_REMIX_SKYBACKDROP", 1);
		return value;
	}

	u32 sky_hash_mode()
	{
		// Default 1 (measure, tag nothing) - RPCS3_REMIX_SKYHASH=0 is 81af315 exactly.
		//
		// Measure-first because the hashes this learns from cannot be replayed out of any capture
		// taken before it: nothing in the backend has ever written an albedo hash and a draw's
		// world-space shape on the same line. 'Remix tex=' is emitted once per unique texture at
		// creation and carries no draw context; 'Remix sky-census:' carries the draw and no hash.
		// So the fraction of submitted draws a hash rule would cover was not a number anyone had,
		// and arming a rule on a number nobody has is the exact shape of the ae94587 regression
		// (cat_sky=825915 of 2038738 draws, 40.5%, scene uniformly blue). Mode 1 produces that
		// number in one ordinary run - sky_hash_dome / sky_hash_matched / sky_hash_rejected in the
		// stats line, and 'Remix sky-hash-census:' naming the hashes and the programs - and mode 2
		// acts on it.
		//
		// The rule itself is deliberately not a threshold. A hash is armed only when *every* draw
		// carrying it has been dome-shaped (camera inside its transformed AABB, world extent past
		// sky_min_extent(), and at least s_sky_backdrop_min_units_per_vertex of extent per vertex)
		// over at least s_sky_hash_min_draws draws. One non-dome draw disqualifies the hash for the
		// life of the process, so the false-positive population is not "draws below a threshold" -
		// it is "textures this title uses on nothing but a dome", which is a much smaller set and
		// one the census names before anything is tagged.
		//
		// RPCS3_REMIX_CAT_SKY is the manual form of the same thing and predates this: it takes an
		// explicit comma-separated hash list and tags it unconditionally through classify_draw().
		// Use it to pin the answer once the census has been read; this knob is what produces the
		// list to pin.
		static const u32 value = env_u32(L"RPCS3_REMIX_SKYHASH", 1);
		return value;
	}

	u32 viewmodel_mode()
	{
		// Default 2 (tag). Unlike sky_backdrop_mode, which ships at 1 because tagging it wrong
		// paints the scene blue, this one cannot currently change a pixel: the runtime this
		// backend targets has no bit 26 to receive (dxvk-remix-numos3 public/include/remix/
		// remix_c.h stops at SMOOTH_NORMALS = 1 << 24, and rtx_remix_api.cpp toRtCategories()
		// maps bits 0..24 by name under a static_assert on InstanceCategories::Count == 25).
		// Shipping it at 1 would therefore buy nothing and leave the backend needing a code
		// change on the day the fork gains the bit, which is precisely when it should already
		// be measured. RPCS3_REMIX_VIEWMODEL=0 restores 81af315's *tagging* exactly; it no longer
		// restores 81af315, because viewmodel_camera_mode does not consult this knob and decides
		// the reference on its own. Both have to be 0 to get the old behaviour whole.
		static const u32 value = env_u32(L"RPCS3_REMIX_VIEWMODEL", 2);
		return value;
	}

	u32 viewmodel_camera_mode()
	{
		// Default 2 (transform). See the declaration for the replay that justifies it: at 148b467 a
		// viewmodel draw was composed against the *world* camera's reference and came out with a
		// basis of (0.491, 0.002, 1.150) - a 500x collapse on Y - which is what smears the arms
		// across the screen. RPCS3_REMIX_VIEWMODELCAM=0 restores 148b467.
		static const u32 value = env_u32(L"RPCS3_REMIX_VIEWMODELCAM", 2);
		return value;
	}

	bool sky_allows_textured()
	{
		// Default ON as of the sky-anchor rule; it was OFF at ae94587 and that default was a
		// measurement, not caution. Allowing textured sky candidates on R2 (NPEA00431) tagged
		// cat_sky=825915 of 2038738 submitted draws - 40.5% of everything drawn, ~194 draws per
		// frame - and the scene turned uniformly blue, because a SKY-tagged instance is lit as sky
		// rather than as world geometry.
		//
		// The reason was the extent test, not the texture test: it measured the *submitted* vertex
		// positions, which on this title are raw quantised integers (attr0 type=5, w spanning
		// -16511..16511) that the program decodes with a constant scale before its matrix. Nearly
		// every R2 mesh therefore spans far more than sky_min_extent() in attribute space, so once
		// the untextured requirement stopped carrying the filter, the extent stopped filtering too.
		// Haze's dome only ever passed because its positions are already in world units.
		//
		// Both halves of that are now fixed: the extent is measured after the instance transform,
		// which contains the decode, and sky_max_anchor() requires the draw's origin to sit on the
		// camera. Replaying the pair over the 157 dumped draws that carry a fused matrix leaves 3
		// sky draws where the raw-extent rule left 63, and all 3 are domes (Haze
		// fc0fac8afccec49a, R2 41c59a3a2bfc71bf and c1781a2e32aba35d) - so the texture test is no
		// longer load-bearing and R2's textured dome can be reached at all. Set 0 to restore the
		// untextured-only requirement.
		static const u32 value = env_u32(L"RPCS3_REMIX_SKYTEXTURED", 1);
		return value != 0;
	}

	f32 debug_light_radius()
	{
		static const f32 env = env_float(L"RPCS3_REMIX_LIGHTRADIUS", -1.f);
		return env >= 0.f ? env : static_cast<f32>(g_cfg.video.remix.debug_light_radius);
	}

	f32 debug_light_radiance()
	{
		static const f32 env = env_float(L"RPCS3_REMIX_LIGHTRADIANCE", -1.f);
		return env >= 0.f ? env : static_cast<f32>(g_cfg.video.remix.debug_light_radiance);
	}

	bool hash_in_category(draw_category which, u64 hash)
	{
		if (!hash)
		{
			return false;
		}

		const std::vector<u64>& list = category_lists()[static_cast<u32>(which) & 3];
		return std::find(list.begin(), list.end(), hash) != list.end();
	}

	bool any_category_listed()
	{
		// Not cached: the lists are editable at runtime from the settings dialog, and latching
		// this would leave categorisation permanently off for anyone who starts with empty lists.
		const bool value = []() -> bool
		{
			const std::vector<u64>* lists = category_lists();

			for (u32 i = 0; i < 4; ++i)
			{
				if (!lists[i].empty())
				{
					return true;
				}
			}

			return false;
		}();

		return value;
	}

	void sun_direction(f32 (&out)[3])
	{
		static const std::array<f32, 3> value = []() -> std::array<f32, 3>
		{
			// Down and slightly across: a plausible mid-morning sun in a Y-up world, which is
			// what every title seen so far uses. Tuned by knob, not guessed at again in code.
			std::array<f32, 3> result = { -0.35f, -0.9f, -0.25f };

			wchar_t buffer[128]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_SUNDIR", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written != 0 && written < std::size(buffer))
			{
				f32 parsed[3]{};
				u32 count = 0;
				const wchar_t* cursor = buffer;

				while (count < 3 && *cursor)
				{
					wchar_t* end = nullptr;
					const double v = ::wcstod(cursor, &end);

					if (end == cursor)
					{
						break;
					}

					parsed[count++] = static_cast<f32>(v);
					cursor = end;

					while (*cursor == L',' || *cursor == L' ' || *cursor == L';')
					{
						++cursor;
					}
				}

				if (count == 3)
				{
					f32 v[3] = { parsed[0], parsed[1], parsed[2] };

					if (normalize3(v))
					{
						result = { v[0], v[1], v[2] };
						return result;
					}
				}
			}

			f32 v[3] = { result[0], result[1], result[2] };
			normalize3(v);
			return { v[0], v[1], v[2] };
		}();

		out[0] = value[0];
		out[1] = value[1];
		out[2] = value[2];
	}

	f32 sun_radiance()
	{
		// The shader divides a distant light's radiance by sin^2(halfAngle) and samples the
		// cone uniformly, so the irradiance it delivers works out at about pi * radiance
		// regardless of the angular diameter (distant_light.slangh:99-101). Single digits are
		// therefore the useful range, not the sphere light's 100.
		static const f32 env = env_float(L"RPCS3_REMIX_SUNRADIANCE", -1.f);
		return env >= 0.f ? env : static_cast<f32>(g_cfg.video.remix.sun_radiance);
	}

	f32 sun_angular_diameter()
	{
		static const f32 env = env_float(L"RPCS3_REMIX_SUNANGLE", -1.f);
		return env >= 0.f ? env : static_cast<f32>(g_cfg.video.remix.sun_angular_diameter);
	}

	f32 camera_light_radiance()
	{
		// 0 = off. env_float rejects non-positive values, so an explicit 0 from the environment
		// cannot be distinguished from unset; the config is what to use for turning it off.
		static const f32 env = env_float(L"RPCS3_REMIX_CAMLIGHT", -1.f);
		return env >= 0.f ? env : static_cast<f32>(g_cfg.video.remix.camera_light);
	}

	bool nosun_enabled()
	{
		// The environment can force this on but not off, so the config is the way to re-enable
		// the sun once a script has disabled it.
		static const bool env = env_flag(L"RPCS3_REMIX_NOSUN");
		return env || g_cfg.video.remix.no_sun;
	}
}

#endif
