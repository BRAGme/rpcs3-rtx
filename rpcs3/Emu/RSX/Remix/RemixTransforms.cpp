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

			// Round 9. The group only matched because its row scalars - which read DIFFERENT
			// temps and lanes - were each chased back through additive accumulator hops to one
			// base temp with lanes exactly {x, y, z}. The additive terms dropped by that chase
			// are, on the program this was named from, the wind sway; the replay is therefore
			// deliberately the un-swayed geometry. RPCS3_REMIX_MADCHAINMIX=0 refuses these again.
			bool mixed_lanes = false;

			// Round 17. The mixed-lane chase resolved every row to one base temp, but the LANES it
			// resolved to are a permutation of x, y, z rather than the identity - and the divide
			// instruction that wrote that temp proves which lane carries which component. Carried
			// so the wdivide step can accept the same permuted divide (a matched group whose divide
			// is refused replays the raw quantised attribute and explodes from the inside, the
			// round-8 failure with a new cause) and so the census can tell this population from
			// round 9's. RPCS3_REMIX_MADLANEMAP=0 refuses these again.
			bool lane_permuted = false;
			u32 lane_attribute = 0;
			u8 lane_of_component[3] = { 0, 1, 2 };

			// How many of the four rows the group actually supplies, the slot step between them,
			// and whether each row only carries xyz. A 4/1/false group is the shape every matcher
			// produced up to ae94587; a strided one only arises for an indexed palette. rows == 3
			// now also comes from the plain non-indexed DP4 matcher, which is the ordinary way a
			// game writes an object matrix - three DP4 rows and a 'MOV r.w, c[K].c' for the
			// homogeneous coordinate. Read those back through read_group_matrix, which substitutes
			// the (0,0,0,1) the ucode leaves implicit instead of taking c[base + 3].
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

		// --- round 17: the divide's own lane permutation --------------------------------------------
		// Named from the Selva canopy pair F7F12D5D15BB9C37 and C1F88035801F88DE, both captured by
		// UCODESTORE this session and decoded offline. Round 9's mixed-lane chase assumes the
		// per-vertex divide wrote the position into lanes x, y, z in that order, because
		// DF46F03B1B7AB8A4 - the program it was named from - does exactly that:
		// 'r4.xyz = I0.xyz * RCP(I0.w)'. Neither canopy program does:
		//
		//   f7f12d5d15bb9c37   3: RCP r4.w   = 1/v0.w
		//                     18: MUL r4.xyw = v0.xyxz * r4.wwww   x->lane x, y->lane y, z->lane w
		//                     22: MUL r1     = r4.yyyy * c[1]
		//                     24: MAD r2     = r4.xxxx * c[0] + r1
		//                     30: MAD r2     = r4.wwww * c[2] + r2
		//                     34: ADD o0     = r2 + c[3]
		//
		//   c1f88035801f88de   7: RCP r1.y   = 1/v0.w
		//                     14: MUL r0.xzw = v0.xxyz * r1.yyyy   x->lane x, y->lane z, z->lane w
		//
		// The rows are still c0, c1, c2 with c3 as the translation, and the group is still one
		// matrix; only the scratch lane each component was parked in differs. Round 9's rule
		// 'resolved_lane == const-derived component' therefore refuses both, and that refusal is
		// the whole of 'areason=no-chain / fail=lay_other' on the tree tops - measured live at
		// vtx=288 and vtx=594 in the round-16 run.
		//
		// This does not RELAX that rule, it PROVES it. The permutation is read out of the divide
		// instruction's own writemask and source swizzle, so lane L is only accepted for component
		// C when the ucode demonstrably put attribute component C in lane L. A group whose row
		// scalars merely happen to come from one register still fails, exactly as before.
		struct wdivide_lane_map
		{
			bool found = false;
			u32 attribute = 0;
			u32 writer = umax;
			// lane_of_component[k] = the lane of 'temp' that carries attribute component k.
			u32 lane_of_component[3] = { umax, umax, umax };
		};

		bool match_wdivide_lane_map(const program_walker& prog, u32 temp, u32 before, wdivide_lane_map& out)
		{
			std::vector<u32> writers;
			prog.collect_vec_writers(walk_target{ false, temp }, writers, before);

			u32 examined = 0;

			for (auto it = writers.rbegin(); it != writers.rend(); ++it)
			{
				// Bounded. The divide sits a handful of instructions ahead of the group in every
				// program of this family; an unbounded scan would let an unrelated MUL far up the
				// program stand in for it. Scanning past a non-divide writer at all is what the
				// strict matcher does not do, and is needed because c1f88035801f88de emits a
				// harmless 'MOV r0.y = r0.zzzz' copy one slot after its divide - a lane the group
				// never reads, which the clobber test below proves.
				if (++examined > 8)
				{
					break;
				}

				const u32 writer = *it;
				const decoded_instr& in = prog[writer];

				if (in.d1.vec_opcode != RSX_VEC_OPCODE_MUL || in.d3.index_const)
				{
					continue;
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
					continue;
				}

				u32 component = 0;

				if (!is_broadcast_swizzle(in.src[scalar_slot], component))
				{
					continue;
				}

				const u32 attribute = u32{in.d1.input_src};
				const u32 scalar_temp = u32{in.src[scalar_slot].tmp_src};

				// The scalar must be the reciprocal of the SAME attribute's w, taken from the last
				// write of that one component and unshadowed by a VEC write. This is match_wdivide's
				// own test, copied rather than loosened: the only thing round 17 changes is which
				// LANES the product may land in, never what the divisor has to be.
				bool reciprocal = false;

				for (u32 i = writer; i-- > 0;)
				{
					const decoded_instr& prev = prog[i];

					if (vec_writes_temp(prev, scalar_temp) && (vec_writemask(prev) & (1u << component)))
					{
						break;
					}

					if (!sca_writes_temp(prev, scalar_temp) || !(sca_writemask(prev) & (1u << component)))
					{
						continue;
					}

					if (prev.d1.sca_opcode != RSX_SCA_OPCODE_RCP || prev.d3.index_const)
					{
						break;
					}

					const SRC& divisor = prev.src[2];
					u32 divisor_component = 0;

					if (divisor.reg_type != RSX_VP_REGISTER_TYPE_INPUT
						|| u32{prev.d1.input_src} != attribute
						|| !is_broadcast_swizzle(divisor, divisor_component)
						|| divisor_component != 3)
					{
						break;
					}

					reciprocal = true;
					break;
				}

				if (!reciprocal)
				{
					continue;
				}

				// Which lane each of x, y and z landed in, read out of this instruction alone.
				u32 lanes[3] = { umax, umax, umax };
				const u32 mask = vec_writemask(in);
				bool ambiguous = false;

				for (u32 lane = 0; lane < 4; ++lane)
				{
					if (!(mask & (1u << lane)))
					{
						continue;
					}

					const u32 c = swizzle_component(in.src[attribute_slot], lane);

					if (c > 2)
					{
						continue;
					}

					if (lanes[c] != umax)
					{
						// One component written into two lanes: which one the group meant is not
						// decidable from the ucode, and guessing is how a mesh comes out
						// axis-swapped rather than merely misplaced.
						ambiguous = true;
						break;
					}

					lanes[c] = lane;
				}

				if (ambiguous || lanes[0] == umax || lanes[1] == umax || lanes[2] == umax)
				{
					continue;
				}

				// Nothing may overwrite a claimed lane between the divide and the group, or the
				// value the group read is not the one this instruction put there.
				const u32 limit = std::min<u32>(before, static_cast<u32>(prog.size()));
				bool clobbered = false;

				for (u32 i = writer + 1; i < limit && !clobbered; ++i)
				{
					const decoded_instr& later = prog[i];

					for (u32 k = 0; k < 3; ++k)
					{
						const u32 bit = 1u << lanes[k];

						if ((vec_writes_temp(later, temp) && (vec_writemask(later) & bit))
							|| (sca_writes_temp(later, temp) && (sca_writemask(later) & bit)))
						{
							clobbered = true;
							break;
						}
					}
				}

				if (clobbered)
				{
					continue;
				}

				out.found = true;
				out.attribute = attribute;
				out.writer = writer;
				out.lane_of_component[0] = lanes[0];
				out.lane_of_component[1] = lanes[1];
				out.lane_of_component[2] = lanes[2];
				return true;
			}

			return false;
		}

		// DP4/DPH one per component of the target, c[base+i] feeding component i: all four, or
		// three with the homogeneous w moved in from a constant instead.
		bool match_dp4_chain(const program_walker& prog, const std::vector<writer_ref>& writers, chain_result& out)
		{
			// Three rows plus a separately-written homogeneous w, or all four - the rule
			// match_indexed_affine and the blend path already state. A 3-row object matrix with
			// 'MOV r.w, c[K].c' standing in for the fourth row is the commonest way a game writes
			// an affine transform, and demanding four DP4s here is what left 716b0c260da02533
			// (0:DP4>r0.x c32, 1:DP4>r0.y c33, 2:DP4>r0.z c34, 3:MOV>r0.w c0.zzzz) with groups=1:
			// the walk matched the c8..c11 projection, could not see the object matrix behind it,
			// and submitted object space as world space.
			if (writers.size() != 3 && writers.size() != 4)
			{
				return false;
			}

			u32 consts[4] = { umax, umax, umax, umax };
			chain_source source{};
			bool have_source = false;
			u32 first = umax;
			u32 w_slot = umax;
			u32 w_component = 0;

			for (const writer_ref& w : writers)
			{
				const decoded_instr& in = prog[w.instr];

				// The homogeneous w, not a row: it lands on component 3 alone and reads a constant
				// rather than the vector the rows multiply. Taken before the opcode check so a MOV
				// here does not disqualify the group - it is what stands in for the fourth row.
				if (u32 only = 0; single_component(w.mask, only) && only == 3
					&& in.d1.vec_opcode == RSX_VEC_OPCODE_MOV
					&& !in.d3.index_const
					&& in.src[0].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT
					&& !in.src[0].neg)
				{
					if (w_slot != umax)
					{
						// Written twice: which of the two the rows saw is not decidable here.
						return false;
					}

					w_slot = in.d1.const_src;
					w_component = static_cast<u32>(in.src[0].swz_x);
					continue;
				}

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

			// Components 0..rows-1 in order, so a group holding x and z but not y stops at 1 and is
			// refused below rather than being rebuilt out of slots that are not a matrix.
			u32 rows = 0;

			while (rows < 4 && consts[rows] != umax)
			{
				++rows;
			}

			if (!have_source || (rows != 4 && !(rows == 3 && w_slot != umax)))
			{
				return false;
			}

			for (u32 c = 1; c < rows; ++c)
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
			out.instructions = rows;
			out.first_instruction = first;
			out.rows = rows;
			out.xyz_only = (rows == 3);
			out.w_slot = w_slot;
			out.w_component = w_component;
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
		bool match_mad_chain_pass(const program_walker& prog, const std::vector<writer_ref>& writers, chain_result& out, bool allow_indexed, bool relaxed, bool mixed_lanes = false)
		{
			if (writers.empty())
			{
				return false;
			}

			// The instruction that produces the final value writes every component - or, relaxed,
			// the three that carry a position.
			//
			// --- round 7: walk past a predicated constant overwrite -------------------------------
			// Round 6 read seven of the cached .vp leaders behind fail=lay_other offline and found
			// the same idiom in all of them: the LAST full-mask writer to o0 is a MOV from a
			// CONSTANT executed under a condition code (cond_test_enable with cond != always). That
			// is the particle/geometry cull idiom - "if this vertex is culled, park HPOS at a
			// constant off-screen point" - and it is not the transform. Starting the backward walk
			// there finds a MOV whose only operand is a constant, which fails the source test two
			// steps later, so the whole chain is thrown away and the program is refused with
			// 'no matrix chain into HPOS' even though the real chain sits directly behind it.
			//
			// Skipping is safe and narrow because both terms have to hold: the write must be
			// PREDICATED (an unconditional constant write really would be all this program does
			// with HPOS, and refusing it is correct) and every consumed source must be a constant
			// (a predicated write that reads a temp is a real value this matcher must not step
			// over). Nothing else about the walk changes.
			//
			// Expected outcome, stated honestly in advance: these programs should move OFF
			// lay_other and onto an inner-chain refusal reason - the matcher gets further, which is
			// progress and a better error, not necessarily a placed draw.
			// RPCS3_REMIX_SKIPCCCONST=0 restores the old terminal choice exactly.
			const auto predicated_constant_write = [&](const decoded_instr& in)
			{
				if (!skip_cc_const_writer_enabled() || !in.d0.cond_test_enable || in.d0.cond == 0x7)
				{
					return false;
				}

				const u32 mask = vec_source_mask(in.d1.vec_opcode);

				for (u32 s = 0; s < 3; ++s)
				{
					if ((mask & (1u << s)) && in.src[s].reg_type != RSX_VP_REGISTER_TYPE_CONSTANT)
					{
						return false;
					}
				}

				return mask != 0;
			};

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
					if (predicated_constant_write(prog[it->instr]))
					{
						continue;
					}

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

			// Round 9, mixed-lane arm only: the scale operand of each scaled step, kept per step
			// instead of being collapsed into one 'source' the moment it is read. The instruction
			// index is kept too because the chase has to search for writers BEFORE that step.
			chain_source chain_scale_src[4]{};
			u32 chain_scale_lane[4] = { umax, umax, umax, umax };
			u32 chain_step_instr[4] = { umax, umax, umax, umax };
			bool chain_scaled[4] = { false, false, false, false };

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

					// Round 9: kept per step so the mixed-lane arm can chase them individually.
					// Written unconditionally - it costs four stores and keeps the two arms reading
					// the same recorded facts.
					chain_scale_src[step] = other;
					chain_scale_lane[step] = component;
					chain_step_instr[step] = cursor;
					chain_scaled[step] = true;

					if (!have_source)
					{
						source = other;
						have_source = true;
					}
					else if (!(source == other))
					{
						// The single-source rule. This is where the Selva canopy program
						// df46f03b1b7ab8a4 is refused today: its three row scalars are r4.y, r1.w
						// and r1.w - different temps and lanes - even though each of them chases
						// back through additive hops to a distinct lane of one base temp that a
						// clean wdivide wrote. The mixed-lane arm defers the decision to the chase
						// below instead of failing here.
						if (!mixed_lanes)
						{
							return false;
						}
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

			// --- round 9: the mixed-lane chase ----------------------------------------------------
			// Named from DF46F03B1B7AB8A4.vp, decoded slot by slot. Its fused group is
			//   @10  MUL r0      = r4.yyyy * c[1]
			//   @32  MAD r0      = r1.wwww * c[0] + r0
			//   @42  MAD r0      = r1.wwww * c[2] + r0
			//   @44  ADD o0      = r0 + c[3]
			// so the loop above records row scalars r4.y, r1.w, r1.w - three broadcast reads that
			// are not the same operand, which is why the single-source rule refuses it. But the
			// scalars are not arbitrary. Slot 9 is a clean per-vertex divide
			// 'r4.xyz = I0.xyz * RCP@2(I0.w)', and each odd scalar chases back through ADDITIVE
			// hops to a distinct lane of r4: r1.w@27 = r2.x*c467.z + r4.x (x plus a SIN sway term),
			// r4.y direct (a trunk does not sway laterally), r1.w@40 = r1.w*c467.z + r4.z (z plus
			// the COS term). The additive terms are the wind; dropping them is what makes the
			// replay a STATIC canopy, which is the honest trade and exactly the contract round 6
			// anticipated for this family.
			//
			// Two things make this narrow rather than a general widening. The component of each row
			// is taken from its CONSTANT SLOT, not from the broadcast lane - a lane of a scratch
			// temp says nothing about which matrix row it feeds, and reading it as one is how a
			// group gets assembled from the wrong rows. And every scaled row must chase to the SAME
			// base temp with lanes exactly matching its own const-derived component, so a group
			// whose scalars merely happen to come from one register still fails unless the lanes
			// line up.
			if (mixed_lanes)
			{
				// Strict layout only. The relaxed pass exists for indexed palettes and recovers its
				// stride from the data; combining that with a const-derived component mapping would
				// leave nothing checking the layout at all.
				if (relaxed || stride != 1)
				{
					return false;
				}

				// Follow one (temp, lane) through additive accumulator hops until the writer is no
				// longer additive. Bounded at 4 hops. Only the MAD addend and the ADD operands are
				// followed - a multiplicative operand is a different value, not the same value
				// displaced, and stepping through one would fold a scale into the placement.
				const auto chase = [&](chain_source src, u32 lane, u32 before, chain_source& out_src, u32& out_lane) -> bool
				{
					if (src.reg_type != RSX_VP_REGISTER_TYPE_TEMP || lane > 3)
					{
						return false;
					}

					for (u32 hop = 0; hop < 4; ++hop)
					{
						bool from_sca = false;
						const u32 writer = prog.last_component_writer(src.index, lane, before, from_sca);

						// No writer, or the lane came from the scalar half: this is as far back as
						// the value can be followed, and where it stands is the answer.
						if (writer == umax || from_sca)
						{
							break;
						}

						const decoded_instr& w = prog[writer];
						const u32 op = w.d1.vec_opcode;

						if (op != RSX_VEC_OPCODE_MAD && op != RSX_VEC_OPCODE_ADD)
						{
							break;
						}

						// MAD accumulates src2; ADD consumes src0 and src2 and either may be the
						// carried value. Prefer whichever is a temp; if both are, prefer src2,
						// which is the accumulator slot the forward walk above also follows.
						u32 slot = umax;

						if (op == RSX_VEC_OPCODE_MAD)
						{
							slot = 2;
						}
						else
						{
							slot = (w.src[2].reg_type == RSX_VP_REGISTER_TYPE_TEMP) ? 2u : 0u;
						}

						if (w.src[slot].reg_type != RSX_VP_REGISTER_TYPE_TEMP)
						{
							break;
						}

						const SRC& s = w.src[slot];
						const u32 next_lane = (lane == 0) ? u32{s.swz_x}
							: (lane == 1) ? u32{s.swz_y}
							: (lane == 2) ? u32{s.swz_z}
							: u32{s.swz_w};

						chain_source next{};
						next.reg_type = RSX_VP_REGISTER_TYPE_TEMP;
						next.index = u32{s.tmp_src};

						// A self-hop would spin without converging.
						if (next == src && next_lane == lane)
						{
							break;
						}

						src = next;
						lane = next_lane;
						before = writer;
					}

					out_src = src;
					out_lane = lane;
					return true;
				};

				chain_source base_temp{};
				bool have_base = false;

				// Round 17: kept per step so the lane test can run once, AFTER the base temp is
				// known - only then can the divide that wrote it be found and its permutation read.
				u32 step_resolved_lane[4] = { umax, umax, umax, umax };
				u32 step_component[4] = { umax, umax, umax, umax };

				for (u32 step = 0; step < steps_used; ++step)
				{
					if (!chain_scaled[step])
					{
						continue;
					}

					// The row this step supplies, from its constant slot. stride is 1 here.
					const u32 component = chain_consts[step] - base;

					if (component > 3)
					{
						return false;
					}

					chain_source resolved{};
					u32 resolved_lane = umax;

					if (!chase(chain_scale_src[step], chain_scale_lane[step], chain_step_instr[step],
						resolved, resolved_lane))
					{
						return false;
					}

					if (!have_base)
					{
						base_temp = resolved;
						have_base = true;
					}
					else if (!(base_temp == resolved))
					{
						return false;
					}

					step_resolved_lane[step] = resolved_lane;
					step_component[step] = component;
					chain_components[step] = component;
				}

				if (!have_base)
				{
					return false;
				}

				// Round 9's rule: the lane each row resolved to IS the row's own component. Tested
				// first and unchanged, so every program matching at 4b7bdb4 keeps matching through
				// exactly this branch and never reaches the lane map below.
				bool identity_lanes = true;

				for (u32 step = 0; step < steps_used; ++step)
				{
					if (chain_scaled[step] && step_resolved_lane[step] != step_component[step])
					{
						identity_lanes = false;
						break;
					}
				}

				if (!identity_lanes)
				{
					// Round 17. The lanes are a permutation, so ask the divide instruction which
					// lane it put each component in and require the rows to agree with THAT. See
					// the doc block on match_wdivide_lane_map for the two canopy programs this was
					// named from. Refusing here is round 16's behaviour exactly.
					if (!mad_lane_map_enabled())
					{
						return false;
					}

					wdivide_lane_map lanes{};

					if (!match_wdivide_lane_map(prog, base_temp.index, cursor, lanes))
					{
						return false;
					}

					for (u32 step = 0; step < steps_used; ++step)
					{
						if (!chain_scaled[step])
						{
							continue;
						}

						if (step_component[step] > 2
							|| lanes.lane_of_component[step_component[step]] != step_resolved_lane[step])
						{
							return false;
						}
					}

					out.lane_permuted = true;
					out.lane_attribute = lanes.attribute;
					out.lane_of_component[0] = static_cast<u8>(lanes.lane_of_component[0]);
					out.lane_of_component[1] = static_cast<u8>(lanes.lane_of_component[1]);
					out.lane_of_component[2] = static_cast<u8>(lanes.lane_of_component[2]);
				}

				// The unscaled step is the translation column: it has no scalar to chase, so its
				// component comes from its slot the same way.
				for (u32 step = 0; step < steps_used; ++step)
				{
					if (chain_scaled[step])
					{
						continue;
					}

					const u32 component = chain_consts[step] - base;

					if (component > 3)
					{
						return false;
					}

					chain_components[step] = component;
				}

				source = base_temp;
				have_source = true;
				out.mixed_lanes = true;
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

			out = chain_result{};

			if (allow_indexed && indexed_world_enabled())
			{
				if (match_mad_chain_pass(prog, writers, out, true, true) && out.indexed)
				{
					return true;
				}

				out = chain_result{};
			}

			// Round 9: the mixed-lane pass, tried LAST so that every program matching today keeps
			// the matcher and the slots it already had - this pass only ever runs on a group both
			// passes above refused. See the chase inside match_mad_chain_pass for what it accepts
			// and why the component mapping comes from the constant slots.
			if (mad_chain_mixed_lanes_enabled()
				&& match_mad_chain_pass(prog, writers, out, allow_indexed, false, true)
				&& out.mixed_lanes)
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
			// The scale was written as RCP(c[K].<c>) in a scalar slot, so the factor is 1/value
			// rather than the value. Still a transform constant and still evaluable per draw.
			bool scale_is_reciprocal = false;
			// '(attr + b) * s' rather than 'attr * s + b'. Derived from the walk order: walking
			// back from the output, meeting the MUL before the ADD means the program applies the
			// ADD first. build_prescale composes scale-then-bias, so the translation row has to
			// carry b*s in this case.
			bool bias_before_scale = false;
			// Round 10: this decode was only reachable because the walk stepped over an
			// accumulate-in-place decoration, merged a per-lane assembly, or forwarded the scale
			// through a MOV. Recorded so the census can name the programs the new arms rescued
			// rather than leaving the fix indistinguishable from "the program always decoded".
			bool used_accum_walk = false;
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
		// Defined further down beside resolve_writers; declared here because the round-10 arms in
		// match_const_affine must refuse to step over a predicated write and the definition sits
		// below them in the file.
		bool is_conditional(const decoded_instr& in);

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

			// A temp whose value is RCP(c[K].<c>) is still a constant scale - R2 writes its
			// dequantisation divisor that way, in the scalar half of an instruction whose vector
			// half does something unrelated:
			//     6:MOV>r0.w/RCP>r1.x(C0.wwww,I0.xyzw,C0.wwww)c1i0    r1.x = 1 / c1.w
			// with c1.w = 256. The MUL that uses it therefore has two temp operands and no
			// constant, which is exactly what 'mul-no-const' was reporting. Requires the scalar
			// writer to be the only writer: a vector instruction touching the same temp would mean
			// the value is not purely the reciprocal.
			// 'which' is the component actually read, not the register: R2 packs unrelated values
			// into the same temp, and the writes around the reciprocal touch a different lane -
			//     6:MOV>r0.w/RCP>r1.x(...)c1i0   reciprocal into r1.x
			//     7:MUL>r1.y(...)c3i0            unrelated, r1.y
			//     8:FRC>r1.y(...)                unrelated, r1.y
			//     9:MUL>r0.xyzw(T0.xyzw,T1.xxxx) reads r1.x
			// - so a sole-writer test on the register alone rejects every real case.
			// Which clause of the probe below declined, reported as the refusal reason so that
			// 'mul-no-const' names a test rather than a population. Two rounds of edits to this
			// matcher were inert because the census could only say "still mul-no-const", which is
			// consistent with the probe never running, running and finding no writer, and running
			// and rejecting the writer it found - three different bugs. Static literals only; the
			// last operand probed wins, which is unambiguous while the grammar allows one scale.
			const char* recip_why = "mul-no-const";

			const auto reciprocal_of_const = [&](u32 reg, u32 which, u32 bound, u32& slot, u8& comp)
			{
				const u32 bit = 1u << (which & 3);
				bool found = false;

				for (u32 i = 0; i < bound && i < static_cast<u32>(prog.size()); ++i)
				{
					const decoded_instr& w = prog[i];

					if (vec_writes_temp(w, reg) && (vec_writemask(w) & bit))
					{
						recip_why = "mul-recip-vec-lane";
						return false;
					}

					if (!sca_writes_temp(w, reg) || !(sca_writemask(w) & bit))
					{
						continue;
					}

					if (w.d1.sca_opcode != RSX_SCA_OPCODE_RCP)
					{
						recip_why = "mul-recip-not-rcp";
						return false;
					}

					if (w.src[2].reg_type != RSX_VP_REGISTER_TYPE_CONSTANT)
					{
						recip_why = "mul-recip-not-const";
						return false;
					}

					if (w.src[2].neg)
					{
						recip_why = "mul-recip-neg";
						return false;
					}

					if (w.d3.index_const)
					{
						recip_why = "mul-recip-indexed";
						return false;
					}

					slot = w.d1.const_src;
					comp = static_cast<u8>(w.src[2].swz_x);
					found = true;
				}

				if (!found)
				{
					recip_why = "mul-recip-no-writer";
				}

				return found;
			};

			if (sca_touches_xyz(temp, before))
			{
				return refuse("sca-xyz");
			}

			std::vector<u32> writers;
			prog.collect_vec_writers(walk_target{ false, temp }, writers, before);

			u32 cursor = before;

			// --- round 10: state carried across the hop loop ------------------------------------
			// The temp the walk is currently chasing. Tracked because two of the round-10 arms need
			// it (the accumulate-in-place skip re-checks scalar shadowing on it, and the per-lane
			// merge re-checks it on the temp it lands on); every existing hop already re-collects
			// 'writers' for the new temp and now updates this beside it.
			u32 cur_temp = temp;
			// Accumulate-in-place decorations stepped over. Bounded exactly like the hop count and
			// counted apart from it, because skipping a decoration is not progress through the
			// grammar - it is refusing to let a decoration end the walk.
			u32 accum_skips = 0;
			// The per-lane merge fires at most once per walk, so it cannot loop.
			bool lane_merged = false;

			// One MUL and one ADD is the whole grammar; the bound is hop count, not instructions.
			// A while loop rather than a for: the round-10 arms reposition the cursor without
			// consuming a hop, which a for-loop's increment would silently charge them for.
			u32 hop = 0;

			while (hop < 4)
			{
				u32 chosen = umax;
				bool partial_xyz = false;

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
						// not looking. Refuse rather than fold two thirds of a decode - unless the
						// per-lane merge below can prove all three axes converge on ONE full-xyz
						// writer, which is the only case where "somewhere this walk is not
						// looking" turns out to be somewhere it can look after all.
						// 'chosen' is deliberately left unset: nothing below may use this
						// instruction, only the merge may look past it.
						partial_xyz = true;
						break;
					}

					chosen = *it;
					break;
				}

				// --- round 10 (B): the per-lane merge ------------------------------------------
				// Haze's giant Selva flower assembles r0 one lane at a time beneath its wind
				// decoration - 'r0.x@21 = r0.x*c466.x + r4.x', 'r0.y@10 = MOV r4.y',
				// 'r0.z@31 = r0.z*c466.x + r4.z' - and all three lanes carry the SAME-LANE addend
				// from r4, whose xyz is written in one instruction at slot 7. The existing
				// partial-xyz refusal is correct as a default and wrong for exactly that shape.
				//
				// Deliberately narrow, and every clause is load-bearing:
				//   - fires only where the walk would otherwise refuse partial-xyz, and only once;
				//   - each lane is followed through LANE-PRESERVING hops only, bounded at 4;
				//   - all three lanes must reach the SAME instruction, and that instruction must be
				//     a full-xyz writer, so the merge never invents a decode - it only repositions
				//     the cursor and lets the ordinary arms judge what it landed on;
				//   - each lane must arrive on its OWN lane, so a permutation cannot be read as the
				//     identity;
				//   - nothing conditional, indexed, saturating, absolute-valued or negating is ever
				//     stepped over, and scalar shadowing is re-checked on the temp it lands on.
				// Sweep: over all 64 cached + 17 captured programs this fires on ONE program (the
				// flower) and changes no other outcome; 6,752 universal probe points reach
				// partial-xyz across 57 programs and it accepts 21, all in that one program.
				if (partial_xyz && remix_rsx::mad_accumulate_walk_enabled() && !lane_merged)
				{
					u32 merge_target = umax;
					bool merge_ok = true;

					for (u32 lane = 0; lane < 3 && merge_ok; ++lane)
					{
						u32 lane_temp = cur_temp;
						u32 lane_component = lane;
						u32 lane_cursor = cursor;
						u32 landed = umax;

						for (u32 lane_hop = 0; lane_hop < 4; ++lane_hop)
						{
							bool from_sca = false;
							const u32 def = prog.last_component_writer(lane_temp, lane_component, lane_cursor, from_sca);

							if (def == umax || from_sca)
							{
								merge_ok = false;
								break;
							}

							const decoded_instr& lane_in = prog[def];

							if ((vec_writemask(lane_in) & 0x7) == 0x7)
							{
								// A full-xyz writer: this is where the lane converges. Judged by
								// the ordinary arms once the merge repositions the cursor, so it is
								// deliberately NOT vetted here - the arms already refuse what they
								// cannot represent.
								landed = def;
								break;
							}

							// Anything the merge STEPS OVER is vetted, because stepping over it
							// silently drops whatever it did.
							if (is_conditional(lane_in) || lane_in.d3.index_const || lane_in.d0.staturate)
							{
								merge_ok = false;
								break;
							}

							// A lane-preserving hop: 'MOV dst.c = t.<c>', or an accumulate whose
							// carried operand is read on the same lane. The carried operand is
							// src2 for MAD and the temp operand for ADD; anything else ends it.
							u32 carry = umax;

							if (lane_in.d1.vec_opcode == RSX_VEC_OPCODE_MOV)
							{
								carry = 0;
							}
							else if (lane_in.d1.vec_opcode == RSX_VEC_OPCODE_MAD
								&& lane_in.src[0].reg_type != RSX_VP_REGISTER_TYPE_INPUT
								&& lane_in.src[1].reg_type != RSX_VP_REGISTER_TYPE_INPUT)
							{
								carry = 2;
							}
							else if (lane_in.d1.vec_opcode == RSX_VEC_OPCODE_ADD)
							{
								// Whichever operand is the temp being carried; the other is the
								// decoration and must not be an attribute. src2 is preferred when
								// both are temps, matching the order the offline sweep measured.
								if (lane_in.src[2].reg_type == RSX_VP_REGISTER_TYPE_TEMP
									&& lane_in.src[0].reg_type != RSX_VP_REGISTER_TYPE_INPUT)
								{
									carry = 2;
								}
								else if (lane_in.src[0].reg_type == RSX_VP_REGISTER_TYPE_TEMP
									&& lane_in.src[2].reg_type != RSX_VP_REGISTER_TYPE_INPUT)
								{
									carry = 0;
								}
							}

							if (carry == umax
								|| lane_in.src[carry].reg_type != RSX_VP_REGISTER_TYPE_TEMP
								|| lane_in.src[carry].neg
								|| lane_in.d0.src0_abs)
							{
								merge_ok = false;
								break;
							}

							const u32 next_temp = lane_in.src[carry].tmp_src;
							const u32 next_component = swizzle_component(lane_in.src[carry], lane_component);

							// A hop that changes neither the register nor the lane is not a hop; it
							// would spin until the bound and prove nothing.
							if (next_temp == lane_temp && next_component == lane_component)
							{
								merge_ok = false;
								break;
							}

							lane_temp = next_temp;
							lane_component = next_component;
							lane_cursor = def;
						}

						// The landing must be reached AND the lane must have survived the walk
						// unpermuted: reading a permuted assembly as the identity is exactly the
						// silent mis-placement this walk exists to avoid.
						if (landed == umax || lane_component != lane)
						{
							merge_ok = false;
							break;
						}

						if (merge_target == umax)
						{
							merge_target = landed;
						}
						else if (merge_target != landed)
						{
							// The three axes come from different instructions: this really is the
							// partial write the refusal was written for.
							merge_ok = false;
						}
					}

					if (merge_ok && merge_target != umax)
					{
						const u32 merged_temp = prog[merge_target].d0.dst_tmp;

						// Bound is merge_target + 1, i.e. the landing instruction is inside the
						// scan: a scalar write in its own slot shadows the xyz it produces.
						if (!sca_touches_xyz(merged_temp, merge_target + 1))
						{
							lane_merged = true;
							out.used_accum_walk = true;
							cur_temp = merged_temp;
							prog.collect_vec_writers(walk_target{ false, merged_temp }, writers, merge_target + 1);
							cursor = merge_target + 1;
							continue;
						}
					}
				}

				if (partial_xyz)
				{
					return refuse("partial-xyz");
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
					// A scale already found means the walk met the MUL first, i.e. the program
					// applies this ADD before it: '(attr + b) * s'.
					out.bias_before_scale = out.has_scale;

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
					cur_temp = in.src[other_slot].tmp_src;
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
						// Both operands are temps. If exactly one of them is RCP(constant), the
						// scale is constant-derived after all and the other operand is the value
						// being scaled - keep walking back into it. This is R2's
						// '(attr + c18.xyz) * (1 / c1.w)' decode, the whole mul-no-const
						// population: 41c59a3a, a426abcd, c1781a2e, c87769e0, d736a5bd, dbc64826.
						u32 recip_operand = umax;
						u32 value_operand = umax;
						u32 recip_slot = 0;
						u8 recip_comp = 0;

						for (u32 s = 0; s < 2; ++s)
						{
							if (in.src[s].reg_type != RSX_VP_REGISTER_TYPE_TEMP)
							{
								continue;
							}

							u32 slot = 0;
							u8 comp = 0;

							// The scale must be read as a broadcast - one lane splatted across
							// xyz. A per-lane read of a temp is not a scalar factor.
							const SRC& op = in.src[s];
							const bool broadcast = (op.swz_x == op.swz_y) && (op.swz_y == op.swz_z);

							if (broadcast && reciprocal_of_const(op.tmp_src, op.swz_x, chosen, slot, comp))
							{
								// Two reciprocal operands is a product of two constants, not a
								// scale on a vertex. Refuse rather than pick one.
								if (recip_operand != umax)
								{
									return refuse("mul-two-recip");
								}

								recip_operand = s;
								recip_slot = slot;
								recip_comp = comp;
							}
							else
							{
								value_operand = s;
							}
						}

						if (recip_operand == umax || value_operand == umax)
						{
							// 'mul-no-const' now means only that the probe never ran - neither operand
							// was a broadcast temp. Every other value names the clause it stopped
							// on. A reciprocal paired with a non-temp operand is 'attr * (1/c)',
							// which terminates the walk rather than continuing it; not handled here.
							return refuse(recip_operand != umax ? "mul-recip-attr" : recip_why);
						}

						if (in.src[recip_operand].neg || in.src[value_operand].neg)
						{
							return refuse("mul-recip-negated");
						}

						if (!is_identity_xyz_swizzle(in.src[value_operand]))
						{
							return refuse("mul-recip-swizzle");
						}

						if (sca_touches_xyz(in.src[value_operand].tmp_src, chosen))
						{
							return refuse("mul-recip-sca-xyz");
						}

						out.has_scale = true;
						out.scale_is_reciprocal = true;
						out.scale_slot = recip_slot;
						out.scale_component[0] = recip_comp;
						out.scale_component[1] = recip_comp;
						out.scale_component[2] = recip_comp;

						prog.collect_vec_writers(walk_target{ false, in.src[value_operand].tmp_src }, writers, chosen);
						cur_temp = in.src[value_operand].tmp_src;
						cursor = chosen;
						break;
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
						// --- round 10 (M): the accumulate-in-place decoration -------------------
						// 'MAD dst.xyz = a*b + dst.xyz' with neither factor an attribute is not a
						// term of the position at all - it is a decoration added on top of one.
						// Haze's giant flower carries exactly that at slot 52
						// ('MAD r0.xyz = r1.xyzx, r1.wwww, r0.xyzx', a distance-scaled billboard
						// offset), and it is the sole reason the walk never reaches the real base
						// writer beneath it.
						//
						// Skipping it replays the geometry STATIC - the billboard offset and the
						// sway are dropped, exactly the static-replay contract round 9 shipped for
						// the DF46 canopy - and that is stated here rather than hidden, because it
						// is a deliberate loss and not an approximation.
						//
						// GUARDED ordering on purpose: tested only where the arm has already
						// decided to refuse. Tested BEFORE the switch instead, it would also
						// swallow 'ADD dst.xyz = dst.xyz + c[K]', which is a real constant bias the
						// ADD arm folds correctly - 24 such sites exist in this title's programs
						// and the unguarded ordering fires on 12 of them.
						//
						// The skip does not consume a hop: it is not progress through the grammar,
						// it is declining to let a decoration end the walk. Bounded at 4 either way.
						if (remix_rsx::mad_accumulate_walk_enabled()
							&& accum_skips < 4
							&& in.src[1].reg_type != RSX_VP_REGISTER_TYPE_INPUT
							&& in.src[2].reg_type == RSX_VP_REGISTER_TYPE_TEMP
							&& in.src[2].tmp_src == u32{in.d0.dst_tmp}
							&& is_identity_xyz_swizzle(in.src[2])
							&& !in.src[2].neg
							&& !is_conditional(in)
							&& !in.d0.staturate
							&& !sca_touches_xyz(u32{in.d0.dst_tmp}, chosen))
						{
							++accum_skips;
							out.used_accum_walk = true;
							cursor = chosen;
							continue;
						}

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

					// --- round 10 (A): the MOV-forwarded constant scale --------------------------
					// A scale operand that reached this MAD through a register is still a constant
					// scale. Haze's flower writes 'MOV r0.xz = c[467].yyzy' and then reads
					// 'MAD r4.xyz = I0.xyzx, r0.xxxx, c[61].xyzx' - so its factor IS c[467].y, and
					// refusing it as 'mad-scale-not-const' is the last thing standing between that
					// program and a correct decode.
					//
					// This is the chase match_prescale already performs for its own scalar operand
					// ("Chase the scalar back to the constant it was copied from"), tightened in
					// two places: last_component_writer rather than last_temp_writer plus a
					// writemask test, so the definition found is the one actually live at this
					// point rather than merely a writer of the register; and a from_sca refusal,
					// because a SCA writer is RCP/RSQ - a computed scalar, not a constant copy.
					//
					// Broadcast reads only. A per-lane temp read cannot be proven constant by a
					// single MOV, and proving it would take three lookups that must agree; that is
					// a natural extension, not something to assume here.
					u32 scale_slot = umax;
					u8 scale_components[3] = { 0, 0, 0 };

					if (in.src[1].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
					{
						scale_slot = in.d1.const_src;
						scale_components[0] = static_cast<u8>(in.src[1].swz_x);
						scale_components[1] = static_cast<u8>(in.src[1].swz_y);
						scale_components[2] = static_cast<u8>(in.src[1].swz_z);
					}
					else if (u32 factor_component = 0; remix_rsx::mad_accumulate_walk_enabled()
						&& in.src[1].reg_type == RSX_VP_REGISTER_TYPE_TEMP
						&& !in.src[1].neg
						&& is_broadcast_swizzle(in.src[1], factor_component))
					{
						bool factor_from_sca = false;
						const u32 factor_def = prog.last_component_writer(
							in.src[1].tmp_src, factor_component, chosen, factor_from_sca);

						if (factor_def == umax || factor_from_sca)
						{
							return refuse("mad-scale-not-const");
						}

						const decoded_instr& mov = prog[factor_def];

						if (mov.d1.vec_opcode != RSX_VEC_OPCODE_MOV
							|| mov.src[0].reg_type != RSX_VP_REGISTER_TYPE_CONSTANT
							|| mov.d3.index_const
							|| mov.src[0].neg
							|| mov.d0.src0_abs
							|| mov.d0.staturate
							|| is_conditional(mov))
						{
							return refuse("mad-scale-not-const");
						}

						const u8 forwarded = static_cast<u8>(swizzle_component(mov.src[0], factor_component));

						out.used_accum_walk = true;
						scale_slot = mov.d1.const_src;
						scale_components[0] = forwarded;
						scale_components[1] = forwarded;
						scale_components[2] = forwarded;
					}
					else
					{
						return refuse("mad-scale-not-const");
					}

					if (in.src[0].neg || in.src[1].neg)
					{
						return refuse("mad-negated");
					}

					out.has_scale = true;
					out.scale_slot = scale_slot;
					out.scale_component[0] = scale_components[0];
					out.scale_component[1] = scale_components[1];
					out.scale_component[2] = scale_components[2];

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

				// Reached only by a 'break' out of the switch, which is what the arms use to mean
				// "a real hop was taken". The round-10 arms 'continue' instead, so a decoration
				// skipped or a set of lanes merged does not spend one of the four hops the grammar
				// is allowed.
				++hop;
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

		// --- round 11: what the ucode routes into COL0 -------------------------------------------
		//
		// COL0 is not an attribute - it is output register o1 (o0 = HPOS, o1 = COL0, o2 = COL1,
		// o5 = FOGC, o7..o14 = TEX0..7), and the program may pass ATTR3 through, scale it, compute
		// it, or never write it. See the vcol_route doc block in RemixTransforms.h for the five
		// decoded programs this grammar was written from.
		//
		// This mirrors sweep11.py instruction for instruction; that script was run over all 82
		// cached .vp (bin\cache\...\shaders_cache\raw plus bin\remix_ucode) BEFORE this shipped and
		// its named-actor assertions all passed. Any change here must be replayed there first.
		constexpr u32 s_col0_output = 1;
		constexpr u32 s_max_vcol_hops = 3;

		struct vcol_lane_result
		{
			bool ok = false;
			u32 attr_lane = 0;
			u32 count = 0;
			u16 slot[max_vcol_scale_factors] = {};
			u8 comp[max_vcol_scale_factors] = {};
		};

		// One lane of one register, walked backwards. Succeeds only when the lane is I3's
		// corresponding lane multiplied by transform constants and nothing else: MOV/MUL only, at
		// most one varying operand per hop, no negation, no indexed constant, never the SCA half.
		// Everything else is 'computed', which means no replay, which means today's white.
		bool vcol_lane_source(const program_walker& prog, bool is_output, u32 reg, u32 lane,
			u32 before, u32 depth, vcol_lane_result& out)
		{
			if (depth > s_max_vcol_hops)
			{
				return false;
			}

			const u32 bit = 1u << lane;
			u32 writer = umax;

			if (is_output)
			{
				for (u32 i = 0; i < before && i < static_cast<u32>(prog.size()); ++i)
				{
					if (sca_writes_output(prog[i], reg) && (sca_writemask(prog[i]) & bit))
					{
						return false;
					}

					if (vec_writes_output(prog[i], reg) && (vec_writemask(prog[i]) & bit))
					{
						writer = i;
					}
				}
			}
			else
			{
				bool from_sca = false;
				writer = prog.last_component_writer(reg, lane, before, from_sca);

				if (from_sca)
				{
					return false;
				}
			}

			if (writer == umax)
			{
				return false;
			}

			const decoded_instr& in = prog[writer];

			if (in.d3.index_const)
			{
				return false;
			}

			if (in.d1.vec_opcode != RSX_VEC_OPCODE_MOV && in.d1.vec_opcode != RSX_VEC_OPCODE_MUL)
			{
				return false;
			}

			const u32 sources = vec_source_mask(in.d1.vec_opcode);

			bool has_varying = false;
			bool varying_is_input = false;
			u32 varying_index = 0;
			u32 varying_comp = 0;

			u32 pending_slot[max_vcol_scale_factors]{};
			u32 pending_comp[max_vcol_scale_factors]{};
			u32 pending = 0;

			for (u32 s = 0; s < 3; ++s)
			{
				if (!(sources & (1u << s)))
				{
					continue;
				}

				const SRC& src = in.src[s];

				if (src.neg)
				{
					return false;
				}

				const u32 comp = swizzle_component(src, lane);

				if (src.reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
				{
					if (pending >= max_vcol_scale_factors)
					{
						return false;
					}

					pending_slot[pending] = in.d1.const_src;
					pending_comp[pending] = comp;
					++pending;
				}
				else if (src.reg_type == RSX_VP_REGISTER_TYPE_INPUT)
				{
					if (in.d1.input_src != 3 || has_varying)
					{
						return false;
					}

					has_varying = true;
					varying_is_input = true;
					varying_comp = comp;
				}
				else if (src.reg_type == RSX_VP_REGISTER_TYPE_TEMP)
				{
					if (has_varying)
					{
						return false;
					}

					has_varying = true;
					varying_is_input = false;
					varying_index = src.tmp_src;
					varying_comp = comp;
				}
				else
				{
					return false;
				}
			}

			if (!has_varying)
			{
				return false;
			}

			if (varying_is_input)
			{
				out.attr_lane = varying_comp;
			}
			else if (!vcol_lane_source(prog, false, varying_index, varying_comp, writer, depth + 1, out))
			{
				return false;
			}

			// Innermost first, so the fold's product is written in ucode order and the census reads
			// like the program does.
			for (u32 i = 0; i < pending && out.count < max_vcol_scale_factors; ++i)
			{
				out.slot[out.count] = static_cast<u16>(pending_slot[i]);
				out.comp[out.count] = static_cast<u8>(pending_comp[i]);
				++out.count;
			}

			out.ok = true;
			return true;
		}

		void scan_vcol_route(const program_walker& prog, vp_fingerprint& out)
		{
			u32 last = umax;

			for (u32 i = 0; i < static_cast<u32>(prog.size()); ++i)
			{
				if (sca_writes_output(prog[i], s_col0_output))
				{
					// The scalar half wrote the colour output: not a shape this grammar covers.
					out.vcol_route_kind = vcol_route::computed;
					return;
				}

				if (vec_writes_output(prog[i], s_col0_output))
				{
					last = i;
				}
			}

			if (last == umax)
			{
				out.vcol_route_kind = vcol_route::none;
				return;
			}

			const decoded_instr& in = prog[last];

			if (vec_writemask(in) != 0xf || in.d3.index_const)
			{
				// A partial write leaves the other lanes undefined, and an indexed constant is not
				// a fixed factor. 29 of the 82 cached programs land here (masks 0x7 and 0x8).
				out.vcol_route_kind = vcol_route::computed;
				return;
			}

			const auto identity_swizzle = [](const SRC& s)
			{
				return !s.neg && s.swz_x == 0 && s.swz_y == 1 && s.swz_z == 2 && s.swz_w == 3;
			};

			if (in.d1.vec_opcode == RSX_VEC_OPCODE_MOV
				&& in.src[0].reg_type == RSX_VP_REGISTER_TYPE_INPUT
				&& in.d1.input_src == 3
				&& identity_swizzle(in.src[0]))
			{
				out.vcol_route_kind = vcol_route::passthrough;
				out.vcol_alpha_from_attr = true;
				return;
			}

			// --- round 12: the constant route ---------------------------------------------------
			// 'MOV o1.xyzw, c[K]'. The full-mask and !index_const guards above already ran, so K is
			// a fixed slot and every lane of COL0 comes from it. vcol_alpha_from_attr stays FALSE:
			// the alpha does come from the constant, but the replay deliberately submits opaque
			// rather than a constant this backend has never measured - see the
			// vcol_constant_route_enabled() doc block. Leaving the flag false is what makes every
			// alpha consumer refuse it without needing to know this route exists.
			// Classified UNCONDITIONALLY - RPCS3_REMIX_VCOLCONST gates only the REPLAY, in
			// vcol_route_replayable() and in the flat branch of apply_vertex_colour. That way a
			// session with the knob off still prints 'route=constant cval=[...]' and hands over the
			// live colour, instead of hiding the measurement behind the change it is supposed to
			// justify.
			//
			// Round 13: the write must be UNCONDITIONAL to be replayable. A program that selects
			// between two colours with two conditional 'MOV o1.xyzw, c[K]' writes would be
			// classified from whichever is last in program order and then replayed on every vertex -
			// which moves this route's failure direction from WHITE (round 11's honest refusal) to
			// A WRONG COLOUR, the one direction this design block promises it never takes. Same
			// refusal idiom the file already uses at :1374 and :1596. Saturate is deliberately NOT
			// tested: the replay's flat() already clamps to [0,1], so a saturated constant replays
			// identically.
			//
			// Blast radius, measured over all 73 cached .vp (scratchpad/sweep13.py): ZERO programs
			// move - none of them writes COL0 conditionally on its last write, and none saturates
			// it either. This is pure hardening against a shape this title happens not to contain.
			if (in.d1.vec_opcode == RSX_VEC_OPCODE_MOV
				&& in.src[0].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT
				&& !in.src[0].neg
				&& !(in.d0.cond_test_enable && in.d0.cond != 0x7))
			{
				out.vcol_route_kind = vcol_route::constant;

				for (u32 lane = 0; lane < 4; ++lane)
				{
					out.vcol_scale_count[lane] = 1;
					out.vcol_scale_slot[lane][0] = static_cast<u16>(in.d1.const_src);
					out.vcol_scale_comp[lane][0] = static_cast<u8>(swizzle_component(in.src[0], lane));
				}

				return;
			}

			if (in.d1.vec_opcode == RSX_VEC_OPCODE_MUL)
			{
				for (u32 s = 0; s < 2; ++s)
				{
					const SRC& attr = in.src[s];
					const SRC& scale = in.src[1 - s];

					if (attr.reg_type == RSX_VP_REGISTER_TYPE_INPUT
						&& in.d1.input_src == 3
						&& identity_swizzle(attr)
						&& scale.reg_type == RSX_VP_REGISTER_TYPE_CONSTANT
						&& !scale.neg)
					{
						out.vcol_route_kind = vcol_route::scaled;
						out.vcol_alpha_from_attr = true;

						for (u32 lane = 0; lane < 4; ++lane)
						{
							out.vcol_scale_count[lane] = 1;
							out.vcol_scale_slot[lane][0] = static_cast<u16>(in.d1.const_src);
							out.vcol_scale_comp[lane][0] = static_cast<u8>(swizzle_component(scale, lane));
						}

						return;
					}
				}
			}

			vcol_lane_result lanes[4]{};
			bool rgb_ok = true;

			for (u32 lane = 0; lane < 4; ++lane)
			{
				vcol_lane_result r{};

				if (vcol_lane_source(prog, true, s_col0_output, lane, static_cast<u32>(prog.size()), 0, r)
					&& r.attr_lane == lane)
				{
					lanes[lane] = r;
				}

				if (lane < 3 && !lanes[lane].ok)
				{
					rgb_ok = false;
				}
			}

			// Classified independently of the rgb verdict: the flower's rgb IS a chain while its
			// alpha is a computed distance ramp, and conflating the two is the bug this closes.
			out.vcol_alpha_from_attr = lanes[3].ok;

			if (!rgb_ok)
			{
				out.vcol_route_kind = vcol_route::computed;
				return;
			}

			out.vcol_route_kind = vcol_route::scaled_chain;

			for (u32 lane = 0; lane < 4; ++lane)
			{
				out.vcol_scale_count[lane] = static_cast<u8>(lanes[lane].count);

				for (u32 i = 0; i < lanes[lane].count; ++i)
				{
					out.vcol_scale_slot[lane][i] = lanes[lane].slot[i];
					out.vcol_scale_comp[lane][i] = lanes[lane].comp[i];
				}
			}
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
		bool resolve_texcoord_scale_slot(const program_walker& prog, u32 output, u16& out_attributes, u32& out_slot,
			texcoord_scale_refusal& refusal)
		{
			refusal = texcoord_scale_refusal::no_multiply;
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
			u16 attributes = 0;

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
			bool temp_slot_ambiguous = false;
			bool temp_operand_ambiguous = false;

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
							temp_operand_ambiguous = true;
						}
					}

					if (temp_found && temp_slot != u32{in.d1.const_src})
					{
					temp_slot_ambiguous = true;
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

				if (found && slot != u32{in.d1.const_src})
				{
					refusal = texcoord_scale_refusal::two_constants;
					return false;
				}

				found = true;
				slot = u32{in.d1.const_src};
				attributes |= static_cast<u16>(1u << index);
			}

			if (!found)
			{
				// Second chance on the temp form, under its own knob so the two populations can be
				// separated in one run. Every condition below has to hold: exactly one constant
				// slot across the slice's MULs, nothing multiplied by a third operand kind, and
				// exactly one attribute read anywhere in the slice. Any ambiguity keeps the fixed
				// divisor, which is the old behaviour and a known-safe answer.
				if (!texcoord_scale_temp_form() || !temp_found)
				{
					return false;
				}

				if (temp_slot_ambiguous)
				{
					refusal = texcoord_scale_refusal::two_constants;
					return false;
				}

				if (temp_operand_ambiguous)
				{
					refusal = texcoord_scale_refusal::third_operand;
					return false;
				}

				if (slice_attribute_ambiguous)
				{
					refusal = texcoord_scale_refusal::two_attributes;
					return false;
				}

				if (slice_attribute == umax)
				{
					refusal = texcoord_scale_refusal::no_attribute;
					return false;
				}

				out_attributes = static_cast<u16>(1u << slice_attribute);
				out_slot = temp_slot;
				refusal = texcoord_scale_refusal::resolved;
				return true;
			}

			out_attributes = attributes;
			out_slot = slot;
			refusal = texcoord_scale_refusal::resolved;
			return true;
		}

		// -----------------------------------------------------------------------------------
		// The general affine texcoord form
		// -----------------------------------------------------------------------------------
		//
		// resolve_texcoord_scale_slot above answers one question - "what scalar does this program
		// divide its texcoord by" - and the c151 evidence says that is not the whole write. The
		// shape that carries Haze's world programs is
		//
		//     4:MUL>r0.xy (I0.xyxx, C0.xxxx)          c151 i8   ; scaled attribute
		//     6:MUL>r0.zw (T0.yyyy, C0.xxxy)          c45  i0   ; row for attr.y
		//     7:MAD>r2.xy (T0.xxxx, C0.xyxx, T0.zwzz) c44  i0   ; + row for attr.x
		//    10:ADD>r4.yw (T2.xxxy, C0.xxxy)          c47  i0   ; + bias
		//    26:ADD>o7.xy (T4.ywyy, C0.zzzz)          c467 i0   ; + broadcast bias
		//
		// i.e. exactly what RPCS3_REMIX_UVAFFINEVP replays with every slot hardcoded, except that
		// the slots and - decisively - the *component* of the scale slot differ per program and
		// per unit (c151.x for the attr8 unit, c151.y for attr9, c151.z for attr10 in the dumps).
		// So this walks it out of the ucode instead of naming it.
		//
		// The walk is backwards from o[7+unit] over the lane *pair* that carries (u, v), because
		// half these programs compute the pair in .zw of a temp and move it. It refuses on
		// anything it cannot express - a partial write, the scalar unit, a negated operand outside
		// the two biases, an indexed constant, an operand that is neither the attribute, a
		// constant, nor a temp already on the chain - and every refusal names itself for the
		// uvscale-fixed census. A refusal costs nothing: the caller keeps the scalar path.

		// Component of 'src' feeding destination lane 'lane' (0=x..3=w).
		u32 src_component(const SRC& src, u32 lane)
		{
			switch (lane)
			{
			case 0:  return u32{src.swz_x};
			case 1:  return u32{src.swz_y};
			case 2:  return u32{src.swz_z};
			default: return u32{src.swz_w};
			}
		}

		// The latest instruction before 'bound' that touches any lane in 'lane_mask' of the
		// register. Refuses - rather than skipping - when that instruction writes only some of
		// the wanted lanes or writes them through the scalar unit, because either means the pair
		// is assembled from two sources and this matcher does not model that.
		bool find_lane_writer(const program_walker& prog, bool is_output, u32 reg, u32 lane_mask,
			u32 bound, u32& out_index)
		{
			for (u32 i = std::min<u32>(bound, static_cast<u32>(prog.size())); i-- > 0;)
			{
				const decoded_instr& in = prog[i];

				const bool vec_hit = is_output ? vec_writes_output(in, reg) : vec_writes_temp(in, reg);
				const bool sca_hit = is_output ? sca_writes_output(in, reg) : sca_writes_temp(in, reg);

				const u32 vec_mask = vec_hit ? (vec_writemask(in) & lane_mask) : 0;
				const u32 sca_mask = sca_hit ? (sca_writemask(in) & lane_mask) : 0;

				if (vec_mask == 0 && sca_mask == 0)
				{
					continue;
				}

				if (sca_mask != 0 || vec_mask != lane_mask)
				{
					return false;
				}

				out_index = i;
				return true;
			}

			return false;
		}

		// One scalar lane reduced back to 'attribute component * constant component', through any
		// number of MOV hops. This is the innermost term of the form above and the only place the
		// attribute is allowed to enter it.
		bool reduce_scaled_attribute(const program_walker& prog, u32 tmp, u32 component, u32 bound,
			u8& out_attribute, u8& out_attr_component, u16& out_scale_slot, u8& out_scale_component,
			const char*& reason)
		{
			u32 reg = tmp;
			u32 lane = component;
			u32 limit = bound;

			for (u32 hop = 0; hop < 8; ++hop)
			{
				u32 index = 0;

				if (!find_lane_writer(prog, false, reg, 1u << lane, limit, index))
				{
					reason = "affine:scale-no-writer";
					return false;
				}

				const decoded_instr& in = prog[index];

				if (in.d3.index_const)
				{
					reason = "affine:scale-indexed";
					return false;
				}

				if (in.d1.vec_opcode == RSX_VEC_OPCODE_MOV)
				{
					const SRC& src = in.src[0];

					if (src.neg || src.reg_type != RSX_VP_REGISTER_TYPE_TEMP)
					{
						reason = "affine:scale-mov-source";
						return false;
					}

					lane = src_component(src, lane);
					reg = u32{src.tmp_src};
					limit = index;
					continue;
				}

				if (in.d1.vec_opcode != RSX_VEC_OPCODE_MUL)
				{
					reason = "affine:scale-not-mul";
					return false;
				}

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

				if (input_slot == umax || const_slot == umax)
				{
					reason = "affine:scale-operands";
					return false;
				}

				if (in.src[input_slot].neg || in.src[const_slot].neg)
				{
					reason = "affine:scale-negated";
					return false;
				}

				const u32 attribute = u32{in.d1.input_src};

				// Attribute 0 is the position; a texcoord built from it is texgen, which this
				// backend does not evaluate.
				if (attribute == 0 || attribute > 15)
				{
					reason = "affine:scale-attr0";
					return false;
				}

				out_attribute = static_cast<u8>(attribute);
				out_attr_component = static_cast<u8>(src_component(in.src[input_slot], lane));
				out_scale_slot = static_cast<u16>(u32{in.d1.const_src});
				out_scale_component = static_cast<u8>(src_component(in.src[const_slot], lane));
				return true;
			}

			reason = "affine:scale-too-deep";
			return false;
		}

		// One row of the 2x2: 'pair = scaled_attribute_component * cRow', where the temp operand
		// broadcasts a single component into both lanes and the constant supplies one component
		// per lane.
		bool resolve_uv_row(const program_walker& prog, u32 tmp, const u32 (&lanes)[2], u32 bound,
			uv_affine_form& out, u32 row, const char*& reason)
		{
			u32 index = 0;

			if (!find_lane_writer(prog, false, tmp, (1u << lanes[0]) | (1u << lanes[1]), bound, index))
			{
				reason = "affine:row-no-writer";
				return false;
			}

			const decoded_instr& in = prog[index];

			if (in.d1.vec_opcode != RSX_VEC_OPCODE_MUL || in.d3.index_const)
			{
				reason = "affine:row-not-mul";
				return false;
			}

			u32 temp_slot = umax;
			u32 const_slot = umax;

			for (u32 s = 0; s < 2; ++s)
			{
				if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_TEMP)
				{
					temp_slot = s;
				}
				else if (in.src[s].reg_type == RSX_VP_REGISTER_TYPE_CONSTANT)
				{
					const_slot = s;
				}
			}

			if (temp_slot == umax || const_slot == umax
				|| in.src[temp_slot].neg || in.src[const_slot].neg)
			{
				reason = "affine:row-operands";
				return false;
			}

			const u32 broadcast = src_component(in.src[temp_slot], lanes[0]);

			if (broadcast != src_component(in.src[temp_slot], lanes[1]))
			{
				reason = "affine:row-not-broadcast";
				return false;
			}

			out.row_slot[row] = static_cast<u16>(u32{in.d1.const_src});
			out.row_component[row][0] = static_cast<u8>(src_component(in.src[const_slot], lanes[0]));
			out.row_component[row][1] = static_cast<u8>(src_component(in.src[const_slot], lanes[1]));

			u8 attribute = 0xff;
			u8 attr_component = 0;
			u16 scale_slot = s_no_uv_slot;
			u8 scale_component = 0;

			if (!reduce_scaled_attribute(prog, u32{in.src[temp_slot].tmp_src}, broadcast, index,
				attribute, attr_component, scale_slot, scale_component, reason))
			{
				return false;
			}

			// Both rows have to be the same attribute through the same scale *slot*, or the two
			// halves of the 2x2 are not a 2x2 at all. The scale *component* may differ per row:
			// c151 is a vector of per-attribute-set divisors (see uv_affine_form::scale_component),
			// so one row reading c151.x and the other c151.y is a legal program, not a mismatch.
			// UVSCALELANES=0 restores the stricter equality this used to demand.
			if (out.attribute != 0xff
				&& (out.attribute != attribute || out.scale_slot != scale_slot
					|| (!uv_scale_lanes_enabled() && out.scale_component[0] != scale_component)))
			{
				reason = "affine:row-mismatch";
				return false;
			}

			out.attribute = attribute;
			out.scale_slot = scale_slot;
			out.scale_component[row] = scale_component;
			out.attr_component[row] = attr_component;
			return true;
		}

		bool resolve_texcoord_affine(const program_walker& prog, u32 output, uv_affine_form& out)
		{
			out = uv_affine_form{};
			out.reason = "affine:no-writer";

			bool is_output = true;
			u32 reg = output;
			u32 lanes[2] = { 0, 1 };
			u32 bound = static_cast<u32>(prog.size());
			u32 biases = 0;

			for (u32 hop = 0; hop < 12; ++hop)
			{
				u32 index = 0;

				if (!find_lane_writer(prog, is_output, reg, (1u << lanes[0]) | (1u << lanes[1]), bound, index))
				{
					out.reason = hop == 0 ? "affine:no-writer" : "affine:split-write";
					return false;
				}

				const decoded_instr& in = prog[index];

				if (in.d3.index_const)
				{
					out.reason = "affine:indexed";
					return false;
				}

				const u32 opcode = in.d1.vec_opcode;

				if (opcode == RSX_VEC_OPCODE_MOV)
				{
					const SRC& src = in.src[0];

					if (src.neg || src.reg_type != RSX_VP_REGISTER_TYPE_TEMP)
					{
						out.reason = "affine:mov-source";
						return false;
					}

					const u32 next[2] = { src_component(src, lanes[0]), src_component(src, lanes[1]) };
					is_output = false;
					reg = u32{src.tmp_src};
					lanes[0] = next[0];
					lanes[1] = next[1];
					bound = index;
					continue;
				}

				// Operand roles, shared by ADD (src0 + src2), MUL (src0 * src1) and MAD
				// (src0 * src1 + src2). Each opcode consults only the slots it consumes.
				const u32 sources = vec_source_mask(opcode);

				u32 temp_slot = umax;
				u32 const_slot = umax;
				u32 input_slot = umax;
				u32 other_temp_slot = umax;

				for (u32 s = 0; s < 3; ++s)
				{
					if (!(sources & (1u << s)))
					{
						continue;
					}

					switch (in.src[s].reg_type)
					{
					case RSX_VP_REGISTER_TYPE_TEMP:
						if (temp_slot == umax)
						{
							temp_slot = s;
						}
						else
						{
							other_temp_slot = s;
						}
						break;
					case RSX_VP_REGISTER_TYPE_CONSTANT:
						const_slot = s;
						break;
					case RSX_VP_REGISTER_TYPE_INPUT:
						input_slot = s;
						break;
					default:
						out.reason = "affine:operand-kind";
						return false;
					}
				}

				if (opcode == RSX_VEC_OPCODE_ADD)
				{
					// 'pair = pair + constant': one of the two biases. Order does not matter to a
					// sum, so the first one found fills bias2 and the second bias.
					if (temp_slot == umax || const_slot == umax || other_temp_slot != umax
						|| in.src[temp_slot].neg)
					{
						out.reason = "affine:add-operands";
						return false;
					}

					if (biases >= 2)
					{
						out.reason = "affine:three-biases";
						return false;
					}

					const u16 slot = static_cast<u16>(u32{in.d1.const_src});
					const u8 component[2] = {
						static_cast<u8>(src_component(in.src[const_slot], lanes[0])),
						static_cast<u8>(src_component(in.src[const_slot], lanes[1])) };

					if (biases == 0)
					{
						out.bias2_slot = slot;
						out.bias2_component[0] = component[0];
						out.bias2_component[1] = component[1];
						out.bias2_negate = in.src[const_slot].neg != 0;
					}
					else
					{
						out.bias_slot = slot;
						out.bias_component[0] = component[0];
						out.bias_component[1] = component[1];
						out.bias_negate = in.src[const_slot].neg != 0;
					}

					++biases;

					const u32 next[2] = {
						src_component(in.src[temp_slot], lanes[0]),
						src_component(in.src[temp_slot], lanes[1]) };
					is_output = false;
					reg = u32{in.src[temp_slot].tmp_src};
					lanes[0] = next[0];
					lanes[1] = next[1];
					bound = index;
					continue;
				}

				if (opcode == RSX_VEC_OPCODE_MUL)
				{
					if (const_slot == umax)
					{
						out.reason = "affine:mul-no-constant";
						return false;
					}

					if (in.src[const_slot].neg)
					{
						out.reason = "affine:mul-negated";
						return false;
					}

					// The terminal form: attribute * constant, no 2x2 at all.
					if (input_slot != umax)
					{
						if (in.src[input_slot].neg)
						{
							out.reason = "affine:mul-negated";
							return false;
						}

						const u32 attribute = u32{in.d1.input_src};

						if (attribute == 0 || attribute > 15)
						{
							out.reason = "affine:mul-attr0";
							return false;
						}

						const u32 scale[2] = {
							src_component(in.src[const_slot], lanes[0]),
							src_component(in.src[const_slot], lanes[1]) };

						if (scale[0] != scale[1] && !uv_scale_lanes_enabled())
						{
							// Two different components of one constant across the pair is a row,
							// not a scale, and without a second row there is nothing to combine.
							// That reading was wrong: with no 2x2 the pair *is* the output, so the
							// two components are simply u's divisor and v's divisor. Kept behind
							// UVSCALELANES=0 as the one-relaunch revert.
							out.reason = "affine:mul-two-components";
							return false;
						}

						out.attribute = static_cast<u8>(attribute);
						out.scale_slot = static_cast<u16>(u32{in.d1.const_src});
						out.scale_component[0] = static_cast<u8>(scale[0]);
						out.scale_component[1] = static_cast<u8>(scale[1]);
						out.attr_component[0] = static_cast<u8>(src_component(in.src[input_slot], lanes[0]));
						out.attr_component[1] = static_cast<u8>(src_component(in.src[input_slot], lanes[1]));
						out.has_rows = false;
						out.resolved = true;
						out.reason = "affine:resolved";
						return true;
					}

					out.reason = "affine:mul-temp-pair";
					return false;
				}

				if (opcode == RSX_VEC_OPCODE_MAD)
				{
					// The 2x2: 'pair = scaled_attr.c * cRow0 + pair_of_the_other_row'.
					if (temp_slot == umax || const_slot == umax || other_temp_slot == umax
						|| input_slot != umax
						|| in.src[temp_slot].neg || in.src[const_slot].neg || in.src[other_temp_slot].neg)
					{
						out.reason = "affine:mad-operands";
						return false;
					}

					// src2 is the addend; src0/src1 are the product. Whichever temp is in src2 is
					// the other row.
					const u32 product_temp = (temp_slot == 2) ? other_temp_slot : temp_slot;
					const u32 addend_temp = (temp_slot == 2) ? temp_slot : other_temp_slot;

					if (addend_temp != 2 || const_slot == 2)
					{
						out.reason = "affine:mad-shape";
						return false;
					}

					const u32 broadcast = src_component(in.src[product_temp], lanes[0]);

					if (broadcast != src_component(in.src[product_temp], lanes[1]))
					{
						out.reason = "affine:mad-not-broadcast";
						return false;
					}

					out.row_slot[0] = static_cast<u16>(u32{in.d1.const_src});
					out.row_component[0][0] = static_cast<u8>(src_component(in.src[const_slot], lanes[0]));
					out.row_component[0][1] = static_cast<u8>(src_component(in.src[const_slot], lanes[1]));

					const char* reason = "affine:row";

					u8 attribute = 0xff;
					u8 attr_component = 0;
					u16 scale_slot = s_no_uv_slot;
					u8 scale_component = 0;

					if (!reduce_scaled_attribute(prog, u32{in.src[product_temp].tmp_src}, broadcast,
						index, attribute, attr_component, scale_slot, scale_component, reason))
					{
						out.reason = reason;
						return false;
					}

					out.attribute = attribute;
					out.scale_slot = scale_slot;
					out.scale_component[0] = scale_component;
					// Row 1 overwrites this if its own scale component differs; until it runs, the
					// honest default for both entries is row 0's, so a refusal downstream still
					// leaves a self-consistent form behind for the census to print.
					out.scale_component[1] = scale_component;
					out.attr_component[0] = attr_component;

					const u32 addend_lanes[2] = {
						src_component(in.src[addend_temp], lanes[0]),
						src_component(in.src[addend_temp], lanes[1]) };

					if (!resolve_uv_row(prog, u32{in.src[addend_temp].tmp_src}, addend_lanes, index,
						out, 1, reason))
					{
						out.reason = reason;
						return false;
					}

					out.has_rows = true;
					out.resolved = true;
					out.reason = "affine:resolved";
					return true;
				}

				out.reason = "affine:opcode";
				return false;
			}

			out.reason = "affine:too-deep";
			return false;
		}

		struct wdivide_result
		{
			bool found = false;
			u32 attribute = 0;
			// How many w-only writes the walk stepped past to reach the divide MUL. Non-zero means
			// this program decodes ONLY because of the round-8 writer walk - the single tell that
			// separates the rescued population from the 25 that always matched.
			u32 shadowed_writers = 0;
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
		//
		// --- round 8: which write counts as "the one that produced the position" ------------------
		// This matcher used to ask prog.last_temp_writer(temp, before) - the single most recent VEC
		// writer - and then require it to be the MUL. Both of its siblings already state the correct
		// rule instead: match_const_affine's hop loop skips writers whose xyz mask is empty, with the
		// comment "A w-only write (the 'r0.w = c[K].z' that completes the homogeneous vector) does
		// not touch the position and cannot hide a term", and match_prescale walks the full writer
		// list. match_wdivide was the only position matcher trusting a single last_temp_writer, and
		// that was the whole giant-geometry bug.
		//
		// Haze's character/prop compiler emits exactly the shape that breaks it:
		//     4: SCA RCP > r1.z          (1 / ATTR0.w)
		//    11: VEC MUL > r1.xyz        (ATTR0.xyz * r1.zzzz)   <- the divide
		//    12: VEC MOV > r1.w = c[467].z  [cond]               <- w-only, completes the homogeneous
		//    15: VEC MUL > r3 = r1.yyyy * c[1]                   <- the matrix group ('before')
		// The last VEC writer of r1 before slot 15 is slot 12, a w-only predicated MOV, so the
		// opcode/mask test failed and the RCP back-scan below never ran. has_wdivide stayed false,
		// the submit path shipped the raw signed-16-bit quantised attribute, and the mesh rendered
		// at extents of 669-9211 on objects a few units across. The recorded areason was 'sca-xyz',
		// which is just match_const_affine's (correct) refusal for the RCP at slot 4 - the last
		// matcher's exit, masking a plain wdivide program.
		//
		// Sweep of every cached .vp on this title, old semantics vs new: 29 programs carry the
		// wdivide MUL shape, 25 select the IDENTICAL instruction under both (when no w-only write
		// intervenes, the latest xyz-touching writer IS the last writer), and exactly 4 flip from
		// refuse to match - 57A12323F22F4988, 4D5A87BFFBCE0717, 96EDAAED0C27FD05, 1D9A973AF5CD1514,
		// all with the bit-identical idiom above. Zero programs regress.
		//
		// Nothing below the selection changes. The RCP back-scan still starts at the chosen writer
		// and still rejects an intervening VEC write to the scalar lane, so a w-only write cannot
		// smuggle anything past it either. RPCS3_REMIX_WDIVWALK=0 restores the single-last-writer
		// selection bit-exactly - the A/B for the whole round.
		bool match_wdivide(const program_walker& prog, u32 temp, u32 before, wdivide_result& out)
		{
			u32 writer = umax;
			u32 shadowed = 0;

			if (wdivide_walk_enabled())
			{
				std::vector<u32> writers;
				prog.collect_vec_writers(walk_target{ false, temp }, writers, before);

				// collect_vec_writers returns program order, so walking it backwards is
				// last_temp_writer's own answer with the w-only writes stepped over.
				for (auto it = writers.rbegin(); it != writers.rend(); ++it)
				{
					if ((vec_writemask(prog[*it]) & 0x7) == 0)
					{
						++shadowed;
						continue;
					}

					writer = *it;
					break;
				}
			}
			else
			{
				writer = prog.last_temp_writer(temp, before);
			}

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
				out.shadowed_writers = shadowed;
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

	// ---------------------------------------------------------------------------------------------
	// Double-precision gauge algebra
	//
	// These five exist for one call site: world = fused x anchor_inverse. Everything about the
	// backend's transform chain is f32 and stays f32; what is not representable in f32 is the
	// *intermediate* of a division by an ill-conditioned matrix. See gauge_f64_enabled() in
	// RemixTransforms.h for the replayed measurements, and note the critical subtlety proven by the
	// same replay: inverting in f64 and casting the inverse back to f32 does NOT help, because the
	// cast inverse's large entries still cancel catastrophically in an f32 multiply. The multiply
	// has to be f64 too, and only the finished, well-scaled world matrix is narrowed.
	// ---------------------------------------------------------------------------------------------

	mat4d mat4d_from(const mat4& a)
	{
		mat4d result{};

		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				result.m[i][j] = static_cast<f64>(a.m[i][j]);
			}
		}

		return result;
	}

	mat4 mat4d_to_f32(const mat4d& a)
	{
		mat4 result{};

		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				result.m[i][j] = static_cast<f32>(a.m[i][j]);
			}
		}

		return result;
	}

	mat4d mat4d_multiply(const mat4d& a, const mat4d& b)
	{
		mat4d result{};

		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				f64 sum = 0.0;
				for (u32 k = 0; k < 4; ++k)
				{
					sum += a.m[i][k] * b.m[k][j];
				}

				result.m[i][j] = sum;
			}
		}

		return result;
	}

	bool mat4d_invert(const mat4d& a, mat4d& out)
	{
		const f64* m = &a.m[0][0];
		f64 inv[16]{};

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

		f64 det = (m[0] * inv[0]) + (m[1] * inv[4]) + (m[2] * inv[8]) + (m[3] * inv[12]);

		// The same 1e-24 floor as the f32 form. It is a singularity test, not a precision test, so
		// it does not tighten with the working precision.
		if (!std::isfinite(det) || std::abs(det) < 1e-24)
		{
			return false;
		}

		det = 1.0 / det;

		for (u32 i = 0; i < 16; ++i)
		{
			(&out.m[0][0])[i] = inv[i] * det;
		}

		return mat4d_is_finite(out);
	}

	bool mat4d_is_finite(const mat4d& a)
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

	f64 mat4d_identity_residual(const mat4d& a, const mat4d& b)
	{
		const mat4d product = mat4d_multiply(a, b);
		f64 worst = 0.0;

		for (u32 i = 0; i < 4; ++i)
		{
			for (u32 j = 0; j < 4; ++j)
			{
				const f64 expected = (i == j) ? 1.0 : 0.0;
				worst = std::max(worst, std::abs(product.m[i][j] - expected));
			}
		}

		return worst;
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
						result.affine_scale_reciprocal = decode.scale_is_reciprocal;
						result.affine_bias_before_scale = decode.bias_before_scale;
						result.affine_accum_walk = decode.used_accum_walk;
					}
				}
			}
		}

		// Round 11. Before any early return, and independent of the position chain entirely: what
		// this program routes into COL0 is a fact about the colour output, and a program whose
		// position decode is refused still has its vertex colour replayed by the untextured arm of
		// apply_vertex_colour. Once per program - the fingerprint is cached by program hash.
		scan_vcol_route(prog, result);

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
			texcoord_scale_refusal refusal = texcoord_scale_refusal::no_multiply;
			u32 slot = 0;
			u16 attributes = 0;

			if (resolve_texcoord_scale_slot(prog, 7 + unit, attributes, slot, refusal))
			{
				result.texcoord_scale_slot[unit] = static_cast<u8>(std::min<u32>(slot, 0xfeu));
				result.texcoord_scale_inputs[unit] = attributes;
			}

			result.texcoord_scale_refused[unit] = refusal;

			// The full form, resolved independently of both passes above: a program can state a
			// scale the scalar pass finds and still carry a 2x2 and two biases it cannot express,
			// which is the whole c151 population. Refusals leave 'reason' set for the census.
			resolve_texcoord_affine(prog, 7 + unit, result.texcoord_affine[unit]);
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
							result.affine_accum_walk = affine.used_accum_walk;
				result.affine_scale_reciprocal = affine.scale_is_reciprocal;
				result.affine_bias_before_scale = affine.bias_before_scale;
							result.affine_scale_reciprocal = affine.scale_is_reciprocal;
							result.affine_bias_before_scale = affine.bias_before_scale;
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
				// Carried so a draw can say WHY it decodes: >0 is a round-8 rescue, 0 is a program
				// that always matched. The census and wdiv_shadowed_draws both read this.
				result.wdivide_shadowed = static_cast<u8>(std::min<u32>(wdivide.shadowed_writers, 255));
				result.affine_reason = "wdivide";
				reached_input = true;
				break;
			}

			// ...or the same per-vertex divide written into a PERMUTED set of lanes, which is the
			// round-17 canopy family. Gated on this group having matched through the lane map, so
			// the population is exactly the programs that arm rescued and no program decoding at
			// 4b7bdb4 can acquire a divide it did not already have. It has to be here rather than
			// left to match_const_affine below: a group applied to the raw quantised attribute
			// renders blown apart from the inside (the round-8 symptom), so a matched group whose
			// divide is refused is worse than no match at all.
			if (chain.lane_permuted)
			{
				if (wdivide_lane_map lanes{}; match_wdivide_lane_map(prog, source.index, chain.first_instruction, lanes)
					&& lanes.attribute == 0)
				{
					result.has_wdivide = true;
					// Not a round-8 rescue: this one never had a shadowing w-only write, it had a
					// permutation. wdiv_shadowed_draws must not count it.
					result.wdivide_shadowed = 0;
					result.affine_reason = "wdivide-lanemap";
					reached_input = true;
					break;
				}
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
				result.affine_accum_walk = affine.used_accum_walk;
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
			result.group_rows[i] = chains[count - 1 - i].rows;
			result.group_w_slot[i] = chains[count - 1 - i].w_slot;
			result.group_w_component[i] = chains[count - 1 - i].w_component;

			// Round 9: any group that only matched through the mixed-lane chase marks the whole
			// program, so the counter and the census can name it without re-deriving anything.
			result.mad_mixed_lanes |= chains[count - 1 - i].mixed_lanes;

			// Round 17: and which of those groups needed the divide's lane permutation read out of
			// the ucode. Kept separate from mad_mixed_lanes so a census line can tell the round-9
			// population (identity lanes) from the round-17 one, and so a play-test that sees the
			// canopy move can name which arm did it.
			if (chains[count - 1 - i].lane_permuted)
			{
				result.mad_lane_permuted = true;
				result.mad_lane_attribute = static_cast<u8>(chains[count - 1 - i].lane_attribute);
				result.mad_lane_of_component[0] = chains[count - 1 - i].lane_of_component[0];
				result.mad_lane_of_component[1] = chains[count - 1 - i].lane_of_component[1];
				result.mad_lane_of_component[2] = chains[count - 1 - i].lane_of_component[2];
			}
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

			// --- KIL recovery ------------------------------------------------------------------
			// Everything below is read by the cutout-threshold walk only; the colour reachability
			// walk above ignores it. Kept on every instruction rather than only on the KIL because
			// the walk runs backwards from the KIL through instructions it has already decoded, and
			// re-decoding the ucode a second time to answer one question would be the more expensive
			// of the two mistakes available here.
			bool set_cond = false;      // this instruction updates a condition-code register
			u8 cond_mod_reg = 0;        // ...which one (SRC0::cond_mod_reg_index)
			u8 cond_reg = 0;            // which CC register this instruction *reads* (SRC0::cond_reg_index)
			bool exec_lt = false;
			bool exec_eq = false;
			bool exec_gr = false;
			u8 cond_swizzle[4] = {};

			// The literal slot that follows an instruction with a CONSTANT operand, already decoded
			// through the same byteswap the ucode itself needs (write_fragment_constants_to_buffer,
			// ProgramStateCache.cpp:807-830, applies exactly this fixup).
			bool has_constant = false;
			f32 constant[4] = {};

			// All three source slots, unfiltered - reg_type, register id and the x-lane swizzle.
			// 'source' above holds TEMP operands of colour-preserving opcodes only, which is the
			// wrong population for a comparison instruction.
			u8 src_type[3] = {};
			u8 src_reg[3] = {};
			u8 src_swizzle_x[3] = {};
			u8 src_neg = 0;             // bit per slot

			// --- round 9: the output-colour classifier ------------------------------------------
			// The input attribute this instruction reads, for whichever source slots are of type
			// INPUT. RSX fragment programs carry ONE attribute index per instruction, in the dest
			// word (OPDEST::src_attr_reg_num), not per source - so 1 = COL0 and 2 = COL1 name the
			// vertex colour registers for every INPUT operand of this instruction at once.
			u8 attr_reg = 0;

			// All four swizzle lanes packed 2 bits each, x in bits 0-1 .. w in bits 6-7. Identity
			// is 0xE4. 'src_swizzle_x' above keeps only the x lane and the KIL walk needs exactly
			// that, so this is added beside it rather than replacing it.
			u8 src_swizzle[3] = {};

			// Bit per slot. NOT uniform across the three source words: SRC0 carries abs at bit 29
			// (after the condition fields), SRC1 and SRC2 at bit 18 - which is why this is filled
			// from the typed unions and not from SRC_Common, whose layout stops at 'neg'.
			u8 src_abs = 0;
		};

		// The RSX fragment pipeline allows 4096 slots; no shipped PS3 program comes near this, and a
		// program that does gets 'truncated' rather than a wrong answer.
		constexpr u32 s_max_fp_instructions = 512;
	}

	// --- ROUND 41 ------------------------------------------------------------------------------
	// Clamped to 8. A chain of identity copies longer than that is not a compiler artefact, it is a
	// different program shape, and following it would be exactly the "widen until something
	// matches" mistake RETRYUNSUP already cost this project one round for.
	u32 fp_vcol_hop_budget()
	{
		static const u32 value = std::min(env_u32(L"RPCS3_REMIX_FPVCOLHOP", 0), 8u);
		return value;
	}

	// --- ROUND 42 ------------------------------------------------------------------------------
	// Default 0. See the doc block on the declaration for the four ucode listings this is designed
	// from and for the pre-registered refutation.
	bool fp_vcol_deep_enabled()
	{
		static const bool value = env_u32(L"RPCS3_REMIX_FPVCOLDEEP", 0) != 0;
		return value;
	}

	// Default 1, and only reachable at all when the knob above is on - out_vcol_scale stays 1.0
	// unless the deep search set it, so this cannot alter a program the terminal classifier named.
	bool fp_vcol_deep_scale_enabled()
	{
		static const bool value = env_u32(L"RPCS3_REMIX_FPVCOLDEEPSCALE", 1) != 0;
		return value;
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
				const u16 unit_bit = static_cast<u16>(1u << d0.tex_num);

				// A TEX instruction chooses its texture image with tex_num, but its coordinates
				// come from SRC0. Direct varying reads use d0.src_attr_reg_num, whose 4..13
				// range is TEX0..TEX9. Do not guess through a temporary here; 0xf keeps the
				// caller on its measured fallback for programs that transform coordinates.
				if (s0.reg_type == RSX_FP_REGISTER_TYPE_INPUT && !s2.use_index_reg &&
					d0.src_attr_reg_num >= 4 && d0.src_attr_reg_num <= 13)
				{
					const u32 shift = static_cast<u32>(d0.tex_num) * 4;
					const u64 coord = static_cast<u64>(d0.src_attr_reg_num - 4);
					const u64 previous = (result.coord_inputs >> shift) & 0xf;

					if (!(result.coord_ambiguous_mask & unit_bit))
					{
						if (previous == 0xf || previous == coord)
						{
							result.coord_inputs = (result.coord_inputs & ~(0xfull << shift)) | (coord << shift);
						}
						else
						{
							result.coord_ambiguous_mask |= unit_bit;
							result.coord_inputs |= 0xfull << shift;
						}
					}
				}
				else
				{
					// A second sample of the same image through a temporary/dynamic source means
					// there is no single direct varying that represents this unit. Keep the 3D
					// path on its measured fallback instead of applying a partial mapping.
					result.coord_ambiguous_mask |= unit_bit;
				}
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

			// The unfiltered source view the KIL threshold walk needs. Cheap and unconditional: the
			// three words are already decoded, and deciding per opcode which of them "counts" is the
			// judgement that made 'source' the wrong array to reuse here.
			for (u32 s = 0; s < 3; ++s)
			{
				in.src_type[s] = static_cast<u8>(srcs[s].reg_type);
				in.src_reg[s] = static_cast<u8>(fp_reg_id(srcs[s].tmp_reg_index, !!srcs[s].fp16));
				in.src_swizzle_x[s] = static_cast<u8>(srcs[s].swizzle_x);
				in.src_neg |= srcs[s].neg ? static_cast<u8>(1u << s) : 0u;

				in.src_swizzle[s] = static_cast<u8>(
					u32{srcs[s].swizzle_x} |
					(u32{srcs[s].swizzle_y} << 2) |
					(u32{srcs[s].swizzle_z} << 4) |
					(u32{srcs[s].swizzle_w} << 6));
			}

			in.attr_reg = static_cast<u8>(d0.src_attr_reg_num);
			in.src_abs = static_cast<u8>((s0.abs ? 1u : 0u) | (s1.abs ? 2u : 0u) | (s2.abs ? 4u : 0u));

			in.set_cond = !!d0.set_cond;
			in.cond_mod_reg = static_cast<u8>(s0.cond_mod_reg_index);
			in.cond_reg = static_cast<u8>(s0.cond_reg_index);
			in.exec_lt = !!s0.exec_if_lt;
			in.exec_eq = !!s0.exec_if_eq;
			in.exec_gr = !!s0.exec_if_gr;
			in.cond_swizzle[0] = static_cast<u8>(s0.cond_swizzle_x);
			in.cond_swizzle[1] = static_cast<u8>(s0.cond_swizzle_y);
			in.cond_swizzle[2] = static_cast<u8>(s0.cond_swizzle_z);
			in.cond_swizzle[3] = static_cast<u8>(s0.cond_swizzle_w);

			// An instruction with a literal operand is followed by a slot of data, not code. Stepped
			// exactly the way analyse_fragment_program steps it (ProgramStateCache.cpp:682) so
			// sampled_mask lands on the same units its referenced_textures_mask does.
			const bool inline_constant =
				s0.reg_type == RSX_FP_REGISTER_TYPE_CONSTANT ||
				s1.reg_type == RSX_FP_REGISTER_TYPE_CONSTANT ||
				s2.reg_type == RSX_FP_REGISTER_TYPE_CONSTANT;

			if (inline_constant && slot + 1 < slots)
			{
				// Same byteswap as the instruction words. The core writes fragment constants into the
				// uniform buffer with exactly this fixup and no other transform
				// (write_fragment_constants_to_buffer_fallback, ProgramStateCache.cpp:807-830), so a
				// value read here is the value the shader sees.
				in.has_constant = true;

				for (u32 c = 0; c < 4; ++c)
				{
					in.constant[c] = std::bit_cast<f32>(fp_decode_word(words[(slot + 1) * 4 + c]));
				}

				// Kept on the fingerprint too, for the lightpass census. Program order, first eight
				// only - past that a program is doing something other than carrying a small table of
				// light parameters and the census is not the instrument for it.
				if (result.literal_count < std::size(result.literals))
				{
					std::copy(std::begin(in.constant), std::end(in.constant),
						std::begin(result.literals[result.literal_count]));
					++result.literal_count;
				}
			}

			code.push_back(in);

			if (inline_constant)
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

		// --- ROUND 43b: units too NARROW to be a colour ------------------------------------------
		// See fp_fingerprint::narrow_sample_mask for the measurement. A texture unit every sample
		// of which writes fewer than THREE destination channels cannot be carrying RGB - it is a
		// height, gloss, mask or lookup map. Structural, not heuristic: the channels simply are
		// not there.
		//
		// Conservative in the safe direction: a unit is kept the moment ANY sample of it writes
		// three or more channels, so a program that reads one unit both ways keeps it.
		{
			u16 wide = 0;

			for (const fp_instr& in : code)
			{
				if (!in.sample || !in.writes)
				{
					continue;
				}

				if (std::popcount(static_cast<u32>(in.write_mask & 0xf)) >= 3)
				{
					wide |= static_cast<u16>(1u << in.tex_num);
				}
			}

			result.narrow_sample_mask = static_cast<u16>(result.sampled_mask & ~wide);
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

		// --- per-pixel discard ------------------------------------------------------------------
		// The whole reason this exists: on this title 99.6% of materials are created while the RSX
		// alpha test is DISABLED (RemixTextures.cpp:334-352), so neither the material nor the
		// per-draw blend extension can be carrying a cutout - and yet foliage is drawn with holes on
		// real hardware. KIL is the only remaining channel in the ucode, and nothing in Remix/ has
		// ever read it. Detection is measurement-first: has_kil ships whatever the replay does with
		// it, so a run can state which channel the title uses from the bytes rather than from a
		// hypothesis.
		{
			s32 kil_index = -1;

			for (u32 i = 0; i < code.size(); ++i)
			{
				if (code[i].opcode != RSX_FP_OPCODE_KIL)
				{
					continue;
				}

				result.has_kil = true;

				const fp_instr& kil = code[i];
				const bool always = kil.exec_lt && kil.exec_eq && kil.exec_gr;
				const bool never = !kil.exec_lt && !kil.exec_eq && !kil.exec_gr;

				// AddFlowOp's own three cases (FragmentProgramDecompiler.cpp:217-236): all three set
				// is 'discard;' unguarded, none set is a commented-out no-op, everything else is
				// 'if (any(cond)) discard;'. Only the third is a cutout.
				if (!always && !never && kil_index < 0)
				{
					result.kil_conditional = true;
					kil_index = static_cast<s32>(i);
				}
			}

			if (!result.has_kil)
			{
				result.kil_note = "none";
			}
			else if (kil_index < 0)
			{
				result.kil_note = "unconditional";
			}
			else
			{
				// Threshold recovery. Deliberately narrow: only the exact classic cutout shape is
				// trusted, and every other shape leaves kil_ref_estimate at -1 with a reason on the
				// census line. A wrong constant replayed as an alpha reference would clip real
				// pixels, which is a worse failure than falling back to the knob.
				const fp_instr& kil = code[kil_index];
				result.kil_note = "no-cc-writer";

				for (s32 i = kil_index - 1; i >= 0; --i)
				{
					const fp_instr& cc = code[i];

					// The KIL reads condition register SRC0::cond_reg_index; an instruction writes
					// one when OPDEST::set_cond is set, into SRC0::cond_mod_reg_index
					// (FragmentProgramDecompiler.cpp:179-199 vs :275-277).
					if (!cc.set_cond || cc.cond_mod_reg != kil.cond_reg)
					{
						continue;
					}

					if (cc.opcode != RSX_FP_OPCODE_ADD && cc.opcode != RSX_FP_OPCODE_MAD)
					{
						result.kil_note = "cc-not-add-mad";
						break;
					}

					if (!cc.has_constant)
					{
						result.kil_note = "cc-no-constant";
						break;
					}

					s32 const_slot = -1;

					for (u32 s = 0; s < 3; ++s)
					{
						if (cc.src_type[s] == RSX_FP_REGISTER_TYPE_CONSTANT)
						{
							const_slot = static_cast<s32>(s);
							break;
						}
					}

					// MAD is dst = s0*s1 + s2, so only a constant in the addend slot is a threshold;
					// a constant in s0/s1 is a scale and reading it as a threshold would be exactly
					// the mis-match the sentinel discipline exists to refuse.
					if (const_slot < 0 || (cc.opcode == RSX_FP_OPCODE_MAD && const_slot != 2))
					{
						result.kil_note = "const-not-addend";
						break;
					}

					// The other operand has to be a texture sample, or this is not an alpha
					// comparison at all. Walked backwards to whatever last wrote that temp.
					bool from_sample = false;

					for (u32 s = 0; s < 3 && !from_sample; ++s)
					{
						if (static_cast<s32>(s) == const_slot || cc.src_type[s] != RSX_FP_REGISTER_TYPE_TEMP)
						{
							continue;
						}

						for (s32 j = i - 1; j >= 0; --j)
						{
							const fp_instr& writer = code[j];

							if (!writer.writes || writer.dest != cc.src_reg[s])
							{
								continue;
							}

							from_sample = writer.sample;
							break;
						}
					}

					if (!from_sample)
					{
						result.kil_note = "cc-not-from-sample";
						break;
					}

					// Which lane of the literal the comparison reads. The threshold is compared
					// against one component, so the x-lane swizzle of the constant operand names it.
					const f32 value = std::abs(cc.constant[cc.src_swizzle_x[const_slot] & 3]);

					if (!std::isfinite(value) || value > 1.f)
					{
						result.kil_note = "const-out-of-range";
						break;
					}

					// alphaReferenceValue is a uint8_t on the same 0..1 scale RemixTextures.cpp:351
					// uses for the RSX alpha_ref register, so the two are directly comparable.
					result.kil_ref_estimate = static_cast<s16>(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f);
					result.kil_note = (cc.opcode == RSX_FP_OPCODE_MAD) ? "mad-const" : "add-const";
					break;
				}
			}
		}

		// --- round 9: what produces the output colour ------------------------------------------
		// The blend extension states, for every draw, that colour comes from the Texture argument
		// only (rtx_materials.h's LegacyMaterialData defaults, restated at the fill site for
		// parity). For a program whose ucode says 'MOV r0, COL0' that configuration throws the
		// answer away: the vertex colours ARE in the submitted mesh, and the runtime is being told
		// to ignore them. This walk names the two shapes where the ucode proves the colour
		// pipeline, so the fill can state it instead of guessing.
		//
		// Flow control disqualifies the whole classification: "the last writer in program order"
		// is only the last writer executed when there are no branches, and a mis-read output shape
		// would re-colour geometry rather than merely fail to.
		if (!result.has_flow && !result.truncated)
		{
			// Identity swizzle, packed 2 bits per lane: x=0, y=1, z=2, w=3.
			constexpr u8 identity_swizzle = 0xE4;

			// The output register the classifier walks. Normally 'col0' (r0 with 32-bit exports,
			// h0 without). A program that writes neither is degenerate; a program that writes only
			// the OTHER one is telling us the shader-control bit disagrees with its own ucode, and
			// following the ucode is the honest reading - the alternative is silently classifying
			// every such program 'other'. Costs one extra search and cannot widen the shapes.
			const u32 col0_alt = fp_reg_id(0, fp32_outputs);

			const auto last_writer = [&](u32 reg, u8 mask, u32 before) -> s32
			{
				for (u32 i = before; i-- > 0;)
				{
					if (code[i].writes && code[i].dest == reg && (code[i].write_mask & mask))
					{
						return static_cast<s32>(i);
					}
				}

				return -1;
			};

			u32 out_reg = col0;
			s32 rgb_index = last_writer(out_reg, 0x7, static_cast<u32>(code.size()));

			if (rgb_index < 0 && col0_alt != col0)
			{
				out_reg = col0_alt;
				rgb_index = last_writer(out_reg, 0x7, static_cast<u32>(code.size()));
			}

			// --- ROUND 41: step backwards through identity temp copies before classifying --------
			// An identity copy is an UNCONDITIONAL MOV of a TEMP with identity swizzle and no
			// negate and no abs. That is the identity function, so replacing the copy with its own
			// producer cannot change what the program computes, and hopping one therefore cannot
			// admit a shape this classifier did not already recognise - it only reaches programs
			// that build the recognised shape in a temp and then export it. That safety is the
			// whole design: RPCS3_REMIX_RETRYUNSUP is on record in this project as the cost of
			// widening a matcher until something matched, and this widening cannot mis-match.
			//
			// It is also LIMITED, and that limit is the pre-registered way for this to fail: a
			// program that does real arithmetic between the modulate and the export - a fog lerp, a
			// specular add, a saturate through MAD - is NOT reached and stays 'other'. If
			// fpclass= does not move off 1/1/0 with FPVCOLHOP armed, the hop is not the gate and
			// the 'Remix fpother:' census below is the data the next widening has to be built on.
			const auto is_identity_copy = [&](const fp_instr& in) -> bool
			{
				return in.opcode == RSX_FP_OPCODE_MOV
					&& in.writes
					&& in.exec_lt && in.exec_eq && in.exec_gr
					&& in.src_type[0] == RSX_FP_REGISTER_TYPE_TEMP
					&& in.src_swizzle[0] == identity_swizzle
					&& !(in.src_neg & 1u)
					&& !(in.src_abs & 1u);
			};

			u8 hops_used = 0;

			const auto hop_copies = [&](s32 index, u8 mask) -> s32
			{
				const u32 budget = fp_vcol_hop_budget();

				for (u32 h = 0; h < budget && index >= 0; ++h)
				{
					const fp_instr& in = code[index];

					if (!is_identity_copy(in))
					{
						break;
					}

					// Strictly earlier than `index` by last_writer's own contract, so this
					// terminates even without the budget.
					const s32 next = last_writer(in.src_reg[0], mask, static_cast<u32>(index));

					if (next < 0)
					{
						break;
					}

					index = next;
					hops_used = std::max<u8>(hops_used, static_cast<u8>(h + 1));
				}

				return index;
			};

			rgb_index = hop_copies(rgb_index, 0x7);

			// One instruction, classified. 'index' is its position so the modulate arm can walk
			// backwards from it for the sampled temp.
			const auto classify = [&](s32 index) -> std::pair<fp_out_source, u8>
			{
				if (index < 0)
				{
					return { fp_out_source::other, 0 };
				}

				const fp_instr& out = code[index];

				// Unconditional only. All three execution bits set is the "always" encoding both
				// decoded Haze programs use; anything else is a predicated write whose last-writer
				// reading is not sound (same rule the KIL walk applies, from the same three bits).
				if (!(out.exec_lt && out.exec_eq && out.exec_gr))
				{
					return { fp_out_source::other, 0 };
				}

				// A source operand that is a clean, unmodified read of COL0/COL1.
				const auto is_vcol = [&](u32 s) -> bool
				{
					return out.src_type[s] == RSX_FP_REGISTER_TYPE_INPUT
						&& (out.attr_reg == 1 || out.attr_reg == 2)
						&& out.src_swizzle[s] == identity_swizzle
						&& !(out.src_neg & (1u << s))
						&& !(out.src_abs & (1u << s));
				};

				// A source operand that is a clean read of a temp whose own last writer sampled a
				// texture. TXD/TXL/TEXBEM etc. all count - fp_op_is_sample is the same predicate
				// sampled_mask is built from, so "the albedo" here means exactly what it means
				// everywhere else in this file.
				const auto is_sampled_temp = [&](u32 s) -> bool
				{
					if (out.src_type[s] != RSX_FP_REGISTER_TYPE_TEMP
						|| out.src_swizzle[s] != identity_swizzle
						|| (out.src_neg & (1u << s))
						|| (out.src_abs & (1u << s)))
					{
						return false;
					}

					// ROUND 41: the same identity-copy hop. 'MUL out, rB, COL0' where rB was
					// MOV'd from the sampled rA is the same modulate written one temp apart.
					const s32 writer = hop_copies(
						last_writer(out.src_reg[s], 0x7, static_cast<u32>(index)), 0x7);
					return writer >= 0 && code[writer].sample;
				};

				if (out.opcode == RSX_FP_OPCODE_MOV && is_vcol(0))
				{
					return { fp_out_source::vcol_pass, out.attr_reg };
				}

				// Either operand order: 'MUL out, tex, COL0' and 'MUL out, COL0, tex' are the same
				// modulate and a title's compiler is free to emit either.
				if (out.opcode == RSX_FP_OPCODE_MUL)
				{
					if ((is_sampled_temp(0) && is_vcol(1)) || (is_vcol(0) && is_sampled_temp(1)))
					{
						return { fp_out_source::vcol_modulate, out.attr_reg };
					}
				}

				return { fp_out_source::other, 0 };
			};

			const auto [rgb_class, rgb_attr] = classify(rgb_index);
			result.out_rgb_source = rgb_class;

			const s32 alpha_index = hop_copies(
				last_writer(out_reg, 0x8, static_cast<u32>(code.size())), 0x8);
			const auto [alpha_class, alpha_attr] = classify(alpha_index);
			result.out_alpha_source = alpha_class;

			// --- ROUND 41: record WHY, when the answer is 'other' --------------------------------
			// Populated only on the failing arm so a classified program costs nothing. This is the
			// evidence the next widening is designed from - the terminal instruction's opcode and
			// the kind of each of its operands - instead of another round of guessing at shapes.
			result.out_rgb_hops = hops_used;

			if (rgb_class == fp_out_source::other && rgb_index >= 0)
			{
				const fp_instr& term = code[rgb_index];

				result.out_rgb_opcode = static_cast<u8>(term.opcode);
				result.out_rgb_predicated = !(term.exec_lt && term.exec_eq && term.exec_gr);
				result.out_rgb_srccount = term.source_count;

				for (u32 slot = 0; slot < 3; ++slot)
				{
					result.out_rgb_srctype[slot] = term.src_type[slot];

					u8 kind = 0;

					if (term.src_swizzle[slot] != identity_swizzle)
					{
						kind |= 0x08;
					}

					if (term.src_neg & (1u << slot))
					{
						kind |= 0x10;
					}

					if (term.src_abs & (1u << slot))
					{
						kind |= 0x20;
					}

					if (term.src_type[slot] == RSX_FP_REGISTER_TYPE_INPUT
						&& (term.attr_reg == 1 || term.attr_reg == 2))
					{
						kind |= 0x01;
					}

					if (term.src_type[slot] == RSX_FP_REGISTER_TYPE_TEMP)
					{
						kind |= 0x04;

						const s32 writer = hop_copies(
							last_writer(term.src_reg[slot], 0x7, static_cast<u32>(rgb_index)), 0x7);

						if (writer >= 0 && code[writer].sample)
						{
							kind |= 0x02;
						}
					}

					result.out_rgb_srckind[slot] = kind;
				}
			}

			// rgb wins the attribute when both classified; they can only disagree if two different
			// instructions wrote the two halves, which is exactly the asymmetric case the two
			// fields exist to keep visible.
			result.out_vcol_attr = rgb_attr ? rgb_attr : alpha_attr;

			// --- ROUND 42: the modulate is upstream, not terminal --------------------------------
			// Runs only on the failing arm, only with RPCS3_REMIX_FPVCOLDEEP=1, and only after the
			// terminal fields above are populated - so a deep-classified program still carries the
			// 'Remix fpother:' shape that defeated the round-41 walk, and a program the terminal
			// classifier named is never reconsidered. The design, the four ucode listings it is
			// built from and the pre-registered refutation are on the declaration in the header.
			if (rgb_class == fp_out_source::other && fp_vcol_deep_enabled())
			{
				// NOTE, from review: this block calls hop_copies, which writes through to
				// 'hops_used'. It does NOT try to restore it. An earlier draft snapshotted
				// hops_used here and wrote it back at the end, which restored nothing - the
				// round-41 srckind census above ALSO calls hop_copies, so the snapshot was already
				// post-census. 'result.out_rgb_hops' is assigned once, above, from the terminal
				// walk's own value and is never touched again; 'hops_used' is dead from that point
				// on, so nothing here can corrupt the census.

				// A source operand that is an unmodified read of COL0 (ATTR1). ATTR2/COL1 is
				// EXCLUDED by measurement: these very programs carry the packed tangent-space
				// normal there ('MAD H7.xyz, H7, {2,-1}' then NRM), so accepting it would replay a
				// normal map as a colour.
				const auto is_col0_operand = [&](const fp_instr& in, u32 s) -> bool
				{
					return in.src_type[s] == RSX_FP_REGISTER_TYPE_INPUT
						&& in.attr_reg == 1
						&& in.src_swizzle[s] == identity_swizzle
						&& !(in.src_neg & (1u << s))
						&& !(in.src_abs & (1u << s));
				};

				// Follows a temp backwards to a clean COL0 read through nothing but BROADCAST
				// constant scales, returning their product. -1 when the chain is anything else.
				// Broadcast (all three rgb swizzle lanes equal) is required so a per-channel
				// constant - a tint, of which these programs contain several - cannot be collapsed
				// into one scalar. Three steps is the measured depth plus one.
				const auto col0_chain_scale = [&](u8 reg, u32 before) -> f32
				{
					f32 scale = 1.f;
					u8 target = reg;
					u32 cursor = before;

					for (u32 step = 0; step < 3; ++step)
					{
						const s32 w = hop_copies(last_writer(target, 0x7, cursor), 0x7);

						if (w < 0)
						{
							return -1.f;
						}

						const fp_instr& in = code[w];

						if (!in.writes || !(in.exec_lt && in.exec_eq && in.exec_gr))
						{
							return -1.f;
						}

						// The chain's root: 'MOV rX, COL0'.
						if (in.opcode == RSX_FP_OPCODE_MOV && is_col0_operand(in, 0))
						{
							return scale;
						}

						// The only step this walks through: 'MUL rX, <src>, <inline constant>'.
						if (in.opcode != RSX_FP_OPCODE_MUL || !in.has_constant)
						{
							return -1.f;
						}

						u32 kslot = 2;

						for (u32 s = 0; s < 2; ++s)
						{
							if (in.src_type[s] == RSX_FP_REGISTER_TYPE_CONSTANT)
							{
								kslot = s;
								break;
							}
						}

						if (kslot > 1)
						{
							return -1.f;
						}

						const u8 ksw = in.src_swizzle[kslot];
						const u32 lane_x = u32{ksw} & 3u;

						if (lane_x != ((u32{ksw} >> 2) & 3u) || lane_x != ((u32{ksw} >> 4) & 3u))
						{
							return -1.f;
						}

						const f32 k = in.constant[lane_x];

						if (!std::isfinite(k) || k <= 0.f)
						{
							return -1.f;
						}

						scale *= k;

						const u32 other = 1u - kslot;

						if (in.src_swizzle[other] != identity_swizzle
							|| (in.src_neg & (1u << other))
							|| (in.src_abs & (1u << other)))
						{
							return -1.f;
						}

						if (is_col0_operand(in, other))
						{
							return scale;
						}

						if (in.src_type[other] != RSX_FP_REGISTER_TYPE_TEMP)
						{
							return -1.f;
						}

						target = in.src_reg[other];
						cursor = static_cast<u32>(w);
					}

					return -1.f;
				};

				// The same backward reachability 'walk' above uses for colour_mask, restricted to
				// the instructions AFTER 'from' - i.e. "does the value written at 'from' still
				// reach COL0.rgb". No kill, so it is an over-approximation in the safe direction:
				// it can only ever say a register reaches the output when it might not, never the
				// reverse, and the alternative - refusing a real modulate because a later
				// conditional write was mistaken for a redefinition - is the failure mode that
				// costs pixels.
				const auto reaches_output = [&](u32 from, u32 reg, u8 mask) -> bool
				{
					std::array<u8, s_fp_reg_count> live{};
					live[out_reg] = 0x7;

					for (u32 round = 0; round < 8; ++round)
					{
						bool changed = false;

						for (u32 i = static_cast<u32>(code.size()); i-- > from + 1;)
						{
							const fp_instr& in = code[i];

							if (!in.writes || !(live[in.dest] & in.write_mask))
							{
								continue;
							}

							for (u32 s = 0; s < in.source_count; ++s)
							{
								if (!(in.source_valid & (1u << s)))
								{
									continue;
								}

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

					return (live[reg] & mask & 0x7) != 0;
				};

				// Latest match wins: a program that modulates twice (a base layer and a detail
				// layer) has its final colour built by the later one, and the earlier is already
				// folded into it.
				for (u32 i = static_cast<u32>(code.size()); i-- > 0;)
				{
					const fp_instr& in = code[i];

					if (in.opcode != RSX_FP_OPCODE_MUL || !in.writes || !(in.write_mask & 0x7))
					{
						continue;
					}

					if (!(in.exec_lt && in.exec_eq && in.exec_gr) || in.source_count < 2)
					{
						continue;
					}

					f32 scale = -1.f;

					for (u32 s = 0; s < 2 && scale < 0.f; ++s)
					{
						const u32 other = 1u - s;

						// One operand must be the sampled albedo.
						if (in.src_type[s] != RSX_FP_REGISTER_TYPE_TEMP
							|| in.src_swizzle[s] != identity_swizzle
							|| (in.src_neg & (1u << s))
							|| (in.src_abs & (1u << s)))
						{
							continue;
						}

						const s32 tex_writer = hop_copies(
							last_writer(in.src_reg[s], 0x7, i), 0x7);

						if (tex_writer < 0 || !code[tex_writer].sample)
						{
							continue;
						}

						// The other must be COL0, directly or through broadcast constant scales.
						if (is_col0_operand(in, other))
						{
							scale = 1.f;
						}
						else if (in.src_type[other] == RSX_FP_REGISTER_TYPE_TEMP
							&& in.src_swizzle[other] == identity_swizzle
							&& !(in.src_neg & (1u << other))
							&& !(in.src_abs & (1u << other)))
						{
							scale = col0_chain_scale(in.src_reg[other], i);
						}
					}

					if (scale > 0.f && reaches_output(i, in.dest, in.write_mask))
					{
						result.out_rgb_source = fp_out_source::vcol_modulate;
						result.out_vcol_attr = 1;
						result.out_rgb_deep = true;
						result.out_vcol_scale = scale;
						break;
					}
				}

			}
		}

		return result;
	}

	const char* fp_out_source_name(fp_out_source source)
	{
		switch (source)
		{
		case fp_out_source::vcol_pass: return "vcol_pass";
		case fp_out_source::vcol_modulate: return "vcol_modulate";
		default: return "other";
		}
	}

	const char* vcol_route_name(vcol_route route)
	{
		switch (route)
		{
		case vcol_route::passthrough: return "passthrough";
		case vcol_route::scaled: return "scaled";
		case vcol_route::scaled_chain: return "scaled_chain";
		case vcol_route::computed: return "computed";
		case vcol_route::constant: return "constant";
		default: return "none";
		}
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

	bool read_group_matrix(const vp_fingerprint& fp, u32 group, mat4& out)
	{
		if (group >= fp.group_count)
		{
			return false;
		}

		slot_block slots{};

		if (!read_slot_block(fp.group_base[group], slots))
		{
			return false;
		}

		// A group that supplies three rows leaves the fourth to a 'MOV r.w, c[K].c'. c[base + 3] is
		// whatever the program happens to keep above the matrix, so using it as the fourth row is
		// how a correct 3-row transform turns into a wrong 4-row one.
		// dp4 only, and only with a w the matcher actually proved. match_mad_chain also reports
		// rows == 3 (a group whose terminal write is xyz), but it never resolves a w slot and its
		// slots are columns rather than rows - substituting there would rewrite groups that read
		// correctly today.
		if (fp.group_shape[group] == chain_shape::dp4 && fp.group_rows[group] == 3 && fp.group_w_slot[group] != umax)
		{
			f32 w[4]{};

			if (!read_slot(fp.group_w_slot[group], w))
			{
				return false;
			}

			// The row substituted below is (0,0,0,1), which is only what the ucode computes if the
			// constant it moves into w really is 1. Anything else is a w this does not model, and
			// the draw is better refused than silently rescaled.
			if (!(std::abs(w[fp.group_w_component[group] & 3] - 1.f) <= 1e-5f))
			{
				return false;
			}

			// dp4 reads c[base + i] as row i of a column-vector matrix, so slot 3 supplies
			// m[i][3] - the homogeneous column. (0,0,0,1) leaves w untouched at 1.
			slots.v[3][0] = 0.f;
			slots.v[3][1] = 0.f;
			slots.v[3][2] = 0.f;
			slots.v[3][3] = 1.f;
		}

		out = slots_to_matrix(slots, fp.group_shape[group]);
		return true;
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

			f32 axis_scale[3] = { 1.f, 1.f, 1.f };

			if (fp.affine_has_scale)
			{
				f32 slot[4]{};

				if (!read_slot(fp.affine_scale_slot, slot))
				{
					return false;
				}

				for (u32 i = 0; i < 3; ++i)
				{
					f32 s = slot[fp.affine_scale_component[i] & 3];

					// Written as RCP(c[K].<c>) in a scalar slot, so the constant is the divisor.
					// Guarded before the reciprocal is taken, not after: 1/0 is an infinity that
					// the finiteness test below would catch, but 1/1e-30 is a finite number large
					// enough to throw the mesh out of the world.
					if (fp.affine_scale_reciprocal)
					{
						if (!std::isfinite(s) || std::abs(s) < 1e-12f)
						{
							return false;
						}

						s = 1.f / s;
					}

					// A zero or non-finite axis collapses the mesh into a plane or deletes it.
					// Refuse the whole decode rather than submit two thirds of it.
					if (!std::isfinite(s) || std::abs(s) < 1e-12f)
					{
						return false;
					}

					axis_scale[i] = s;
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

					// This matrix applies scale then translation, so '(attr + b) * s' has to put
					// b*s in the translation row - the program adds before it scales. Storing b
					// unscaled would place the mesh 1/s times too far out, which on R2's /256
					// decode is a 256x displacement.
					out.m[3][i] = fp.affine_bias_before_scale ? (slot[i] * axis_scale[i]) : slot[i];
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

	bool fp_albedo_narrow_enabled()
	{
		// ROUND 43b. Let albedo_unit_mask() drop units that cannot carry RGB
		// (fp_fingerprint::narrow_sample_mask) when colour_mask saturates and therefore resolves
		// nothing. Only ever consulted on the path that already declined, AND only when dropping
		// them actually changes which unit is elected - so =0 restores round 42 bit-exactly, and
		// so does =1 for every draw whose elected unit is unaffected.
		// env_u32, not env_flag: the useful setting is the off one.
		static const u32 value = env_u32(L"RPCS3_REMIX_FPALBEDONARROW", 1);
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
		// Measured at 0. Same route twice, scored off the knob echo: uvmad=1 resolved 50.9% of
		// the S32K population and uvmad=0 resolved 53.2%, i.e. the form contributed nothing and
		// the small gap the wrong way is inside the area-to-area variation this title shows.
		// Kept because the search is correct and a title that does state its scale as a MAD will
		// need it, but off until one is measured - shipping it on would be asserting a result the
		// A/B refused to give.
		static const u32 value = env_u32(L"RPCS3_REMIX_UVSCALEMAD", 0);
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

	bool uv_affine_all_enabled()
	{
		// env_u32 for the reason full_chain_enabled gives: the useful setting is the *off* one,
		// and env_flag cannot express "explicitly off".
		static const u32 value = env_u32(L"RPCS3_REMIX_UVAFFINEALL", 1);
		return value != 0;
	}

	bool uv_scale_lanes_enabled()
	{
		// Same env_u32 reason as above: the useful setting is the explicit off.
		static const u32 value = env_u32(L"RPCS3_REMIX_UVSCALELANES", 1);
		return value != 0;
	}

	bool uv_range_census_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_UVRANGECENSUS", 1);
		return value != 0;
	}

	bool notex_census_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_NOTEXCENSUS", 1);
		return value != 0;
	}

	bool camera_relatch_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_CAMRELATCH", 1);
		return value != 0;
	}

	bool gauge_anchor_enabled()
	{
		// env_u32 rather than env_flag, same reason camera_relatch_enabled gives: the useful
		// setting is the explicit *off* one, because 0 has to restore the previous round's
		// behaviour exactly for the A/B to mean anything.
		static const u32 value = env_u32(L"RPCS3_REMIX_GAUGEANCHOR", 1);
		return value != 0;
	}

	u32 gauge_trace_frames()
	{
		// 600 frames is about 10 s at 60 fps: long enough to cover a pan across the room, short
		// enough that the trace cannot become the dominant writer in remix_dump.log. Re-armed by
		// any successful pick, so the measurement is available exactly when the user is looking.
		static const u32 value = env_u32(L"RPCS3_REMIX_GAUGETRACE", 600);
		return value;
	}

	u32 gauge_slot_count()
	{
		// Bounded by the compile-time array in RemixGSRender.h - the knob chooses how many of those
		// entries are live, so 4 reproduces round 2's sizing bit for bit and 16 is the new default.
		// A value outside [1, s_max] is clamped rather than refused: an unusable slot count would
		// silently disable the anchor entirely, which is the one failure this counter set exists to
		// make impossible to miss.
		static const u32 value = std::clamp(env_u32(L"RPCS3_REMIX_GAUGESLOTS", 16), 1u, 16u);
		return value;
	}

	bool gauge_prev_dims_enabled()
	{
		// env_u32 for the reason gauge_anchor_enabled gives: the useful setting is the explicit off.
		static const u32 value = env_u32(L"RPCS3_REMIX_GAUGEPREVDIMS", 1);
		return value != 0;
	}

	bool gauge_cur_dims_enabled()
	{
		// env_u32 for the same reason its prev-frame twin gives: the useful setting is the explicit
		// off, so a bare "1" and an unset variable must not mean different things. No clamp needed -
		// this is a boolean and every non-zero armed value reads back as 1 on the knobs line.
		// The full measured derivation is the doc block on the declaration in RemixTransforms.h.
		static const u32 value = env_u32(L"RPCS3_REMIX_GAUGECURDIMS", 1);
		return value != 0;
	}

	u32 gauge_cam_hold_frames()
	{
		// 30 frames is half a second at 60 fps: long enough to cover a burst of frames whose
		// identity draws were culled, short enough that a real scene cut cannot be held through
		// without gauge_cam_held saying so. 0 disables holding entirely.
		static const u32 value = env_u32(L"RPCS3_REMIX_GAUGECAMHOLD", 30);
		return value;
	}

	u32 anchor_gauge_census_lines()
	{
		// 32 lines per stats window, deduped on the anchor's identity, the elected camera's identity,
		// the magnitude bucket and the dominant axis, so a steady disagreement prints once and a
		// change of donor or of axis prints again. Bounded small because
		// this fires on up to 93% of flips (gauge_cam=57820 of 62315 flips on the round-27 run) and
		// an unbounded line here would be the dominant writer in remix_dump.log within seconds.
		static const u32 value = env_u32(L"RPCS3_REMIX_ANCHORGAUGECENSUS", 32);
		return value;
	}

	bool camera_anchor_eye_enabled()
	{
		// Default ON. This is the only knob in this round that changes behaviour, and it is defaulted
		// on because the value it replaces is measurably in the WRONG FRAME on 2,134 of 66,508 flips
		// by up to 429.96 units, and because the replacement is self-consistent by construction rather
		// than by tuning: the eye and the geometry come out of the same matrix. 0 restores round 29.
		static const u32 value = env_u32(L"RPCS3_REMIX_CAMANCHOREYE", 1);
		return value != 0;
	}

	bool skip_aux_untextured_enabled()
	{
		// RETIRED in round 5 - default flipped 1 -> 0 on the evidence, and this is the one place in
		// the round where a default flip *is* the verdict rather than a behaviour change needing an
		// off switch.
		//
		// Round 4 fired it 99,695 times on exactly its named population (Remix aux-untex: named
		// 6004dce... and 504d2b3a...) and the greyscale wash was still reported. The user then ran it
		// at 0 and the doors/walls were still invisible. So: no demonstrated benefit, no demonstrated
		// harm, and the wash now has a better-supported explanation (the nectar game-value greyscale,
		// RPCS3_REMIX_NECTARMODE). A rule that refuses ~100k draws per run for no measured gain is a
		// standing risk of eating legitimate material-less geometry, so it stops being on by default.
		//
		// The knob and its census stay for archaeology: RPCS3_REMIX_SKIPAUXUNTEX=1 restores the
		// round-3/4 behaviour exactly.
		static const u32 value = env_u32(L"RPCS3_REMIX_SKIPAUXUNTEX", 0);
		return value != 0;
	}

	namespace
	{
		struct watch_albedo_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		const watch_albedo_list& watch_albedos()
		{
			static const watch_albedo_list list = []()
			{
				watch_albedo_list result{};
				wchar_t buffer[160]{};
				const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_WATCHALBEDO", buffer, static_cast<DWORD>(std::size(buffer)));

				if (written == 0 || written >= std::size(buffer))
				{
					return result;
				}

				wchar_t* cursor = buffer;

				while (*cursor && result.count < result.values.size())
				{
					while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
					{
						++cursor;
					}

					wchar_t* end = nullptr;
					const u64 value = ::_wcstoui64(cursor, &end, 16);

					if (end == cursor)
					{
						break;
					}

					if (value != 0)
					{
						result.values[result.count++] = value;
					}

					cursor = end;
				}

				return result;
			}();

			return list;
		}
	}

	bool watch_albedo_matches(u64 hash)
	{
		const watch_albedo_list& list = watch_albedos();
		const auto end = list.values.begin() + list.count;
		return hash != 0 && std::find(list.values.begin(), end, hash) != end;
	}

	u32 watch_albedo_count()
	{
		return watch_albedos().count;
	}

	bool fp_kil_cutout_enabled()
	{
		// env_u32 rather than env_flag, same reason gauge_anchor_enabled gives: the useful setting is
		// the explicit *off* one, because 0 has to restore the always-pass alpha state exactly for
		// the foliage A/B to mean anything.
		static const u32 value = env_u32(L"RPCS3_REMIX_FPKIL", 1);
		return value != 0;
	}

	u32 fp_kil_reference()
	{
		// 128 is the midpoint of the 0..255 alphaReferenceValue range and the conventional cutout
		// threshold. Only used when the ucode walk could not recover the program's own constant
		// (fp_fingerprint::kil_ref_estimate == -1); a recovered value always wins.
		static const u32 value = std::min(env_u32(L"RPCS3_REMIX_FPKILREF", 128), 255u);
		return value;
	}

	bool gauge_f64_enabled()
	{
		// env_u32 rather than env_flag: 0 must restore the f32 divide bit-exactly or the A/B that
		// attributes the wobble to arithmetic rather than to reference *selection* says nothing.
		static const u32 value = env_u32(L"RPCS3_REMIX_GAUGEF64", 1);
		return value != 0;
	}

	f32 viewmodel_anchor_limit()
	{
		// 4 is s_viewmodel_max_anchor, the limit the viewmodel census already prints, so a run
		// taken before this knob existed reads against the same number.
		static const f32 value = env_float(L"RPCS3_REMIX_VIEWMODELANCHOR", 4.f);
		return value;
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

	// --- ROUND 32: the geometry audits are the only REMOVABLE occupant of 'rest' -------------------
	// audit_vertex_extent makes four separate passes over the decoded positions of every sub-draw,
	// and audit_world_extent walks the whole index list transforming each vertex into world space.
	// Both are diagnostics: they feed census lines and the VTXREFUSE / wext gates, neither of which
	// is armed on this title (VTXREFUSE defaults off; wext_refused MEASURED at 0 for the whole
	// round-31 session). They were never timed, and 'rest' - the unattributed part of 'draw' - was
	// MEASURED at 40.96 ms/frame, 80.5% of draw, in that session's worst geometry window.
	//
	// Default 1 = current behaviour bit-for-bit. This ships alongside the 'audit=' timer on the
	// 'Remix timing:' line so the size of the trade is stated before it is taken; do NOT set 0 as a
	// blind perf fix, and re-check wext_refused first on any other title.
	bool draw_audit_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_DRAWAUDIT", 1);
		return value != 0;
	}

	f32 streak_extent_ratio()
	{
		// env_u32 rather than env_flag: 0 is the meaningful setting (it restores 81af315) and
		// env_flag cannot tell "set to 0" from "not set at all".
		static const u32 value = env_u32(L"RPCS3_REMIX_STREAKGATE", 128);
		return static_cast<f32>(value);
	}

	u32 skin_index_span()
	{
		// Slots either side of palette_base a bone index may land on before the draw is refused.
		// 0 (the default) measures without refusing: evaluate_palette_slot already proves the slot
		// is inside the constant file, so an index past the rig's own palette is still legal to
		// read and silently pulls in whatever the previous draw left there - a garbage bone, which
		// is what a correctly-textured mesh with a few vertices flung into streaks looks like.
		// Measure first and pick a span from what the census reports; refusing on a guessed bound
		// drops the mesh instead of streaking it, which is not obviously the better failure.
		static const u32 value = env_u32(L"RPCS3_REMIX_SKINSPAN", 0);
		return value;
	}

	f32 drawn_extent_ratio()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_DRAWNEXT", 8);
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

	bool vertex_alpha_enabled()
	{
		// Lets an alpha-blended draw take its alpha from ATTR3 when the albedo texture has none
		// to give. Default on because the case it fires in is one where the current behaviour is
		// provably a no-op: alpha comes from the texture, the texture's alpha is a constant, so
		// the blend resolves to "source, unmodified" no matter what the factors say. 0 restores
		// the unconditional texture-alpha assertion.
		static const u32 value = env_u32(L"RPCS3_REMIX_VTXALPHA", 1);
		return value != 0;
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

	u64 skip_albedo_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_SKIPALBEDO", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();

		return value;
	}

	u64 skip_pair_vp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_SKIPPAIRVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();

		return value;
	}

	u64 skip_pair_albedo_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_SKIPPAIRALBEDO", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();

		return value;
	}

	u64 skip_untextured_vp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_SKIPUNTEXTUREDVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();

		return value;
	}

	static u64 read_skip_untextured_fp_pair_hash(const wchar_t* name)
	{
		wchar_t buffer[32]{};
		const DWORD written = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));

		if (written == 0 || written >= std::size(buffer))
		{
			return 0;
		}

		return ::_wcstoui64(buffer, nullptr, 16);
	}

	u64 skip_untextured_fp_pair_vp_hash()
	{
		static const u64 value = read_skip_untextured_fp_pair_hash(L"RPCS3_REMIX_SKIPUNTEXTUREDFPPAIRVP");
		return value;
	}

	u64 skip_untextured_fp_pair_fp_hash()
	{
		static const u64 value = read_skip_untextured_fp_pair_hash(L"RPCS3_REMIX_SKIPUNTEXTUREDFPPAIRFP");
		return value;
	}

	u64 skip_unbound_blend_vp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_SKIPUNBOUNDBLENDVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();

		return value;
	}

	bool skip_rt_feedback_vp_matches(u64 hash)
	{
		struct skip_rt_vp_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		static const skip_rt_vp_list list = []()
		{
			skip_rt_vp_list result{};
			wchar_t buffer[160]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_SKIPRTVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return result;
			}

			wchar_t* cursor = buffer;

			while (*cursor && result.count < result.values.size())
			{
				while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
				{
					++cursor;
				}

				wchar_t* end = nullptr;
				const u64 value = ::_wcstoui64(cursor, &end, 16);

				if (end == cursor)
				{
					break;
				}

				if (value != 0)
				{
					result.values[result.count++] = value;
				}

				cursor = end;
			}

			return result;
		}();

		const auto end = list.values.begin() + list.count;
		return std::find(list.values.begin(), end, hash) != end;
	}

	u64 uv_affine_vp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_UVAFFINEVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();

		return value;
	}

	u64 skip_extent_vp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_SKIPEXTENTVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();

		return value;
	}

	f32 skip_extent_min()
	{
		static const f32 value = env_float(L"RPCS3_REMIX_SKIPEXTENTMIN", 0.f);
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

	bool world_vp_matches(u64 hash)
	{
		struct world_vp_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		static const world_vp_list list = []()
		{
			world_vp_list result{};
			wchar_t buffer[160]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_WORLDVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return result;
			}

			wchar_t* cursor = buffer;

			while (*cursor && result.count < result.values.size())
			{
				while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
				{
					++cursor;
				}

				wchar_t* end = nullptr;
				const u64 value = ::_wcstoui64(cursor, &end, 16);

				if (end == cursor)
				{
					break;
				}

				if (value != 0)
				{
					result.values[result.count++] = value;
				}

				cursor = end;
			}

			return result;
		}();

		const auto end = list.values.begin() + list.count;
		return std::find(list.values.begin(), end, hash) != end;
	}

	u64 world_identity_vp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_WORLDIDENTITYVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();

		return value;
	}

	bool world_identity_vp_matches(u64 hash)
	{
		struct world_identity_vp_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		static const world_identity_vp_list list = []()
		{
			world_identity_vp_list result{};
			wchar_t buffer[160]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_WORLDIDENTITYVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return result;
			}

			wchar_t* cursor = buffer;

			while (*cursor && result.count < result.values.size())
			{
				while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
				{
					++cursor;
				}

				wchar_t* end = nullptr;
				const u64 value = ::_wcstoui64(cursor, &end, 16);

				if (end == cursor)
				{
					break;
				}

				if (value != 0)
				{
					result.values[result.count++] = value;
				}

				cursor = end;
			}

			return result;
		}();

		const auto end = list.values.begin() + list.count;
		return std::find(list.values.begin(), end, hash) != end;
	}

	namespace
	{
		struct viewmodel_vp_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		const viewmodel_vp_list& viewmodel_vps()
		{
			static const viewmodel_vp_list list = []()
			{
				viewmodel_vp_list result{};
				wchar_t buffer[160]{};
				const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_VIEWMODELVP", buffer, static_cast<DWORD>(std::size(buffer)));

				if (written == 0 || written >= std::size(buffer))
				{
					return result;
				}

				wchar_t* cursor = buffer;

				while (*cursor && result.count < result.values.size())
				{
					while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
					{
						++cursor;
					}

					wchar_t* end = nullptr;
					const u64 value = ::_wcstoui64(cursor, &end, 16);

					if (end == cursor)
					{
						break;
					}

					if (value != 0)
					{
						result.values[result.count++] = value;
					}

					cursor = end;
				}

				return result;
			}();

			return list;
		}
	}

	bool viewmodel_vp_matches(u64 hash)
	{
		const viewmodel_vp_list& list = viewmodel_vps();
		const auto end = list.values.begin() + list.count;
		return hash != 0 && std::find(list.values.begin(), end, hash) != end;
	}

	u32 viewmodel_vp_count()
	{
		return viewmodel_vps().count;
	}

	bool static_index_vp_matches(u64 hash)
	{
		struct static_index_vp_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		static const static_index_vp_list list = []()
		{
			static_index_vp_list result{};
			wchar_t buffer[160]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_STATICINDEXVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return result;
			}

			wchar_t* cursor = buffer;

			while (*cursor && result.count < result.values.size())
			{
				while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
				{
					++cursor;
				}

				wchar_t* end = nullptr;
				const u64 value = ::_wcstoui64(cursor, &end, 16);

				if (end == cursor)
				{
					break;
				}

				if (value != 0)
				{
					result.values[result.count++] = value;
				}

				cursor = end;
			}

			return result;
		}();

		const auto end = list.values.begin() + list.count;
		return std::find(list.values.begin(), end, hash) != end;
	}

	u32 static_index_rebuild_budget()
	{
		// ROUND 31. The ceiling was 4, which is also the default, so the knob could only ever be
		// turned DOWN and the saturated case could not be tested at all. MEASURED in the round-30
		// play-test (60,211 flips): 'Remix static-index:' read peak=4 budget=4 on all 811 lines with
		// deferred=915,419 = 15.2 deferrals per frame against a budget of 4 - the budget is the
		// binding constraint for the whole session, and every deferral either renders a previous
		// frame's subset or drops the draw entirely. Raised to 64 so the constraint can be relieved
		// and measured. The default is unchanged, so a launcher that says 4 behaves exactly as before.
		// --- ROUND 38: the clamp trap, a THIRD time. The launcher arms 64 and the ceiling WAS 64 ---
		// This is the same defect round 31 hit (ceiling == default, so the knob could only be turned
		// down) and round 33 hit again on SUNSPRITEHOLD. MEASURED in the last run of
		// bin\remix_dump.log: "Remix static-index: entries=11498 ... deferred=841 stale=10
		// dropped=831 peak=64 budget=64 resident=115 evicted=11376 nomesh=7". peak == budget ==
		// the clamp ceiling means the budget was saturated and COULD NOT BE RAISED - "peak=64
		// budget=64" was being read as a tuning result when it was the clamp reporting itself.
		// Round 37's relieved reading (peak=56 budget=64 dropped=0) came from a 388-entry window;
		// the 11,498-entry windows in the same log all read peak=64 budget=64 with dropped in the
		// hundreds to thousands.
		// 512 so a ladder exists. The launcher moves to 128 - one conservative step, matching the
		// 4 -> 8 -> 32 -> 64 progression - NOT to the new ceiling.
		static const u32 value = std::clamp(env_u32(L"RPCS3_REMIX_STATICINDEXBUDGET", 4), 1u, 512u);
		return value;
	}

	u64 world_identity_pair_vp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_WORLDIDENTITYPAIRVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();
		return value;
	}

	u64 world_identity_pair_albedo_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_WORLDIDENTITYPAIRALBEDO", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();
		return value;
	}

	u64 world_identity_pair2_vp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_WORLDIDENTITYPAIR2VP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();
		return value;
	}

	u64 world_identity_pair2_albedo_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_WORLDIDENTITYPAIR2ALBEDO", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();
		return value;
	}

	namespace
	{
		u64 read_hash_env(const wchar_t* name)
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
			return (written > 0 && written < std::size(buffer)) ? ::_wcstoui64(buffer, nullptr, 16) : 0;
		}
	}

	u64 world_identity_pair3_vp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_WORLDIDENTITYPAIR3VP");
		return value;
	}

	u64 world_identity_pair3_albedo_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_WORLDIDENTITYPAIR3ALBEDO");
		return value;
	}

	u64 world_identity_pair4_vp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_WORLDIDENTITYPAIR4VP");
		return value;
	}

	u64 world_identity_pair4_albedo_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_WORLDIDENTITYPAIR4ALBEDO");
		return value;
	}

	u64 world_identity_pair5_vp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_WORLDIDENTITYPAIR5VP");
		return value;
	}

	u64 world_identity_pair5_fp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_WORLDIDENTITYPAIR5FP");
		return value;
	}

	u64 world_identity_pair5_albedo_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_WORLDIDENTITYPAIR5ALBEDO");
		return value;
	}

	u64 guest_light_pair_vp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_GUESTLIGHTVP");
		return value;
	}

	u64 guest_light_pair_fp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_GUESTLIGHTFP");
		return value;
	}

	u64 guest_light_pair_albedo_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_GUESTLIGHTALBEDO");
		return value;
	}

	namespace
	{
		struct guest_light_albedo_list
		{
			// Raised 8 -> 16 in round 5. The albedo content hash is now the *only* required key for
			// light injection (the vp/fp pair became optional narrowing), so one fixture type per
			// entry is no longer enough for a level whose lamps are drawn by several programs.
			std::array<u64, 16> values{};
			u32 count = 0;
		};

		const guest_light_albedo_list& guest_light_albedos()
		{
			static const guest_light_albedo_list list = []()
			{
				guest_light_albedo_list result{};
				wchar_t buffer[400]{};
				const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_GUESTLIGHTALBEDO", buffer, static_cast<DWORD>(std::size(buffer)));

				if (written == 0 || written >= std::size(buffer))
				{
					return result;
				}

				wchar_t* cursor = buffer;

				while (*cursor && result.count < result.values.size())
				{
					while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
					{
						++cursor;
					}

					wchar_t* end = nullptr;
					const u64 value = ::_wcstoui64(cursor, &end, 16);

					if (end == cursor)
					{
						break;
					}

					if (value != 0)
					{
						result.values[result.count++] = value;
					}

					cursor = end;
				}

				return result;
			}();

			return list;
		}
	}

	bool guest_light_albedo_matches(u64 hash)
	{
		const guest_light_albedo_list& list = guest_light_albedos();
		const auto end = list.values.begin() + list.count;
		return hash != 0 && std::find(list.values.begin(), end, hash) != end;
	}

	bool guest_light_albedo_any()
	{
		return guest_light_albedos().count != 0;
	}

	u32 guest_light_albedo_count()
	{
		// Echoed on the live line. A typo'd or over-long GUESTLIGHTALBEDO parses to fewer entries
		// than were written, and this is the only place that difference is visible without a debugger.
		return guest_light_albedos().count;
	}

	u64 guest_light_pair2_albedo_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_GUESTLIGHTALBEDO2");
		return value;
	}

	f32 guest_light_radius()
	{
		static const f32 value = env_float(L"RPCS3_REMIX_GUESTLIGHTRADIUS", 0.2f);
		return value;
	}

	f32 guest_light_radiance()
	{
		static const f32 value = env_float(L"RPCS3_REMIX_GUESTLIGHTRADIANCE", 30.f);
		return value;
	}

	f32 guest_light_radius_scale()
	{
		// The fixed 0.2 radius put the emitter at the bbox *centre* of a 2.6-unit lamp housing, i.e.
		// inside its own shade, where the path tracer occludes it with the very mesh it was derived
		// from. Scaling the sphere with the draw's own extent pokes it back out of the housing while
		// staying proportional to how big the fixture actually is.
		//
		// env_float refuses <= 0, so the explicit off is spelled by the caller: 0 (or any unparsable
		// value) falls back here and the caller then takes max(fixed, 0) = the fixed radius, which is
		// round-4 behaviour exactly.
		static const f32 value = env_float(L"RPCS3_REMIX_GUESTLIGHTRADIUSSCALE", 0.35f);
		return value;
	}

	bool guest_light_color_enabled()
	{
		// env_u32 rather than env_flag: the useful setting is the explicit *off* one, because 0 has
		// to restore the fixed warm constant exactly for the photometry A/B to mean anything.
		static const u32 value = env_u32(L"RPCS3_REMIX_GUESTLIGHTCOLOR", 1);
		return value != 0;
	}

	u32 guest_light_idle_frames()
	{
		// 0 = keep every light forever, which is round-4 behaviour and stays the default: a lamp that
		// leaves the view frustum has not stopped existing, and destroying it on sight would make the
		// room go dark whenever the player looks away. Non-zero is for levels where the fixture list
		// is broad enough that stale lights fill the cap.
		static const u32 value = env_u32(L"RPCS3_REMIX_GUESTLIGHTIDLE", 0);
		return value;
	}

	u32 guest_light_max()
	{
		// Was a hardcoded 64. Clamped rather than refused for the same reason gauge_slot_count gives.
		static const u32 value = std::clamp(env_u32(L"RPCS3_REMIX_GUESTLIGHTMAX", 64), 1u, 4096u);
		return value;
	}

	namespace
	{
		// ROUND 30. Was std::array<u64, 16>. The hash keeps its old meaning and its old parse; the
		// intensity is new and 0 means "this entry said nothing, use the global", which is how a
		// colon-free list stays bit-identical to every round before this one.
		struct emissive_albedo_entry
		{
			u64 hash = 0;
			f32 intensity = 0.f;
		};

		struct emissive_albedo_list
		{
			std::array<emissive_albedo_entry, 16> values{};
			u32 count = 0;
			u32 with_intensity = 0;
		};

		const emissive_albedo_list& emissive_albedos()
		{
			static const emissive_albedo_list list = []()
			{
				emissive_albedo_list result{};
				// 512, not 400: sixteen entries carrying ':<intensity>' can reach 415 characters and
				// the guard below refuses an over-long value WHOLESALE rather than truncating it, so
				// the old size would have turned a full list into an empty one.
				wchar_t buffer[512]{};
				const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_EMISSIVE", buffer, static_cast<DWORD>(std::size(buffer)));

				if (written == 0 || written >= std::size(buffer))
				{
					return result;
				}

				wchar_t* cursor = buffer;

				while (*cursor && result.count < result.values.size())
				{
					while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
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
						break;
					}

					cursor = end;

					// ROUND 30: the optional per-entry intensity. _wcstoui64 above stops at the colon
					// because ':' is not a hex digit, so the existing hash parse is untouched.
					f32 intensity = 0.f;

					// Whitespace before the colon is allowed, matching what SUNMAP already accepts, and
					// it is PEEKED rather than consumed: cursor only moves if a colon is really there.
					// That is what keeps a colon-free list bit-identical - 'AAAA BBBB' must still parse
					// as two hashes, and it does, because nothing was consumed when the peek failed.
					// FOUND BY TEST: without this, 'AAAA : 30' silently kept the global AND dropped
					// every later entry, which is the worst possible failure for a knob typed by hand.
					wchar_t* colon = cursor;

					while (*colon == L' ' || *colon == L'\t')
					{
						++colon;
					}

					if (*colon == L':')
					{
						cursor = colon + 1;

						wchar_t* intensity_end = nullptr;
						const double parsed = ::wcstod(cursor, &intensity_end);

						if (intensity_end != cursor && std::isfinite(parsed) && parsed > 0.0)
						{
							intensity = static_cast<f32>(std::min(parsed, 1.0e6));
							cursor = intensity_end;
						}

						// Skip whatever is left of THIS entry, whether the intensity parsed or not, and
						// stop at a separator OR at whitespace. Both halves matter and both were review
						// findings:
						//   * without this on the SUCCESS path, "71D1:30E,C2F3" left the cursor on "E",
						//     the outer loop parsed that as hex, and 0xE was silently added to the
						//     emissive list as a PHANTOM ALBEDO consuming one of the 16 slots. "71D1:30:40"
						//     and "71D1:30x" instead broke out of the loop and dropped the rest of the
						//     list - the exact failure the comment below says must not happen.
						//   * stopping at whitespace as well as at ',' / ';' is what keeps
						//     "71D1:30 C2F3" parsing as two hashes; a skip that only stopped at
						//     separators would eat the second one.
						// One typo must not swallow the rest of the list, and must not invent an entry.
						while (*cursor && *cursor != L',' && *cursor != L';'
							&& *cursor != L' ' && *cursor != L'\t')
						{
							++cursor;
						}
					}

					if (value != 0)
					{
						emissive_albedo_entry& slot = result.values[result.count++];
						slot.hash = value;
						slot.intensity = intensity;

						if (intensity > 0.f)
						{
							++result.with_intensity;
						}
					}
				}

				return result;
			}();

			return list;
		}

		// One linear scan of at most 16 entries, shared by the membership test and the intensity
		// lookup so the two can never disagree about what is on the list.
		const emissive_albedo_entry* emissive_albedo_find(u64 hash)
		{
			if (hash == 0)
			{
				return nullptr;
			}

			const emissive_albedo_list& list = emissive_albedos();

			for (u32 i = 0; i < list.count; ++i)
			{
				if (list.values[i].hash == hash)
				{
					return &list.values[i];
				}
			}

			return nullptr;
		}
	}

	bool emissive_albedo_matches(u64 hash)
	{
		return emissive_albedo_find(hash) != nullptr;
	}

	u32 emissive_albedo_count()
	{
		return emissive_albedos().count;
	}

	f32 emissive_intensity()
	{
		static const f32 value = env_float(L"RPCS3_REMIX_EMISSIVEINTENSITY", 1.f);
		return value;
	}

	f32 emissive_intensity_for(u64 hash)
	{
		const emissive_albedo_entry* entry = emissive_albedo_find(hash);

		// > 0 rather than != 0: the parse above stores nothing else, and this way a future
		// hand-written 0 can never turn a fixture black by accident.
		return (entry && entry->intensity > 0.f) ? entry->intensity : emissive_intensity();
	}

	u32 emissive_albedo_intensity_count()
	{
		return emissive_albedos().with_intensity;
	}

	namespace
	{
		// Round 13. Byte-for-byte the emissive_albedos() latch above, on its own variable and with
		// its own bound: the two lists must be independently editable because one is a set of lamp
		// fixtures and the other is one sky dome, and merging them would tie their intensities
		// together. 8 rather than 16 - a title has one sky.
		struct sky_emissive_albedo_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		const sky_emissive_albedo_list& sky_emissive_albedos()
		{
			static const sky_emissive_albedo_list list = []()
			{
				sky_emissive_albedo_list result{};
				wchar_t buffer[400]{};
				const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_SKYEMISSIVE", buffer, static_cast<DWORD>(std::size(buffer)));

				if (written == 0 || written >= std::size(buffer))
				{
					return result;
				}

				wchar_t* cursor = buffer;

				while (*cursor && result.count < result.values.size())
				{
					while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
					{
						++cursor;
					}

					wchar_t* end = nullptr;
					const u64 value = ::_wcstoui64(cursor, &end, 16);

					if (end == cursor)
					{
						break;
					}

					if (value != 0)
					{
						result.values[result.count++] = value;
					}

					cursor = end;
				}

				return result;
			}();

			return list;
		}
	}

	bool sky_emissive_listed(u64 hash)
	{
		// ROUND 39. The ENV LIST alone - what the user actually typed into RPCS3_REMIX_SKYEMISSIVE.
		// Split out from sky_emissive_albedo_matches() because that predicate now also answers
		// "...or the classifier promoted it", and a census field labelled 'listed' that silently
		// took the wider answer would make the play-test's ground-truth check unfalsifiable.
		if (hash == 0)
		{
			return false;
		}

		const sky_emissive_albedo_list& list = sky_emissive_albedos();
		const auto end = list.values.begin() + list.count;
		return std::find(list.values.begin(), end, hash) != end;
	}

	bool sky_emissive_albedo_matches(u64 hash)
	{
		if (hash == 0)
		{
			return false;
		}

		const sky_emissive_albedo_list& list = sky_emissive_albedos();
		const auto end = list.values.begin() + list.count;

		if (std::find(list.values.begin(), end, hash) != end)
		{
			return true;
		}

		// ROUND 39. The runtime-learned half. Deliberately folded in HERE rather than at the two
		// call sites, because this one predicate is what three separate features already ask:
		// texture_cache::upload's emissive material (RemixTextures.cpp, the sky_emissive local),
		// texture_cache::upload's peak_uv walk (the gate on entry.peak_uv, which is in turn the
		// gate on derive_sky_sun) and the 'Remix skyemissive:' census. Extending the predicate
		// gives all three to a classified dome at once; extending the call sites would have given
		// them to one and left the other two silently keyed on the hand-written list.
		return sky_emissive_promoted(hash);
	}

	u32 sky_emissive_albedo_count()
	{
		return sky_emissive_albedos().count;
	}

	f32 sky_emissive_intensity()
	{
		// 2.0, not 1.0: rtx_instance_manager.cpp:1105 sets exactly 2.0f on every WorldUI instance,
		// which is the value the dome renders at today. Matching it makes the conf-list -> knob
		// handover invisible, so the first thing the user judges is placement and occlusion rather
		// than a brightness change nobody asked for.
		static const f32 value = env_float(L"RPCS3_REMIX_SKYEMISSIVEINT", 2.f);
		return value;
	}

	bool sky_emissive_blend_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_SKYEMISSIVEBLEND", 1);
		return value != 0;
	}

	// --- round 39: the RUNTIME-LEARNED half of the sky-emissive set ---------------------------
	// Derivation, the measurement behind every threshold and the pre-registered refutation are all
	// in RemixTransforms.h on sky_classify_mode(). This is only the storage.
	//
	// A fixed array and not an unordered_set for the same reason every other list in this file is
	// one: the read happens on the texture upload path and on the sky census, both of which run on
	// the RSX thread, and a bounded linear scan of at most 64 u64s is cheaper than a hash lookup at
	// this size. 64 is 4x the 16 domes haze_domes.csv says the whole title has, so the bound is not
	// a limit anything is expected to reach - and if it IS reached, sky_promote_overflow says so
	// rather than the set silently going quiet.
	namespace
	{
		struct sky_promoted_list
		{
			std::array<u64, 64> values{};
			u32 count = 0;
			u32 overflow = 0;
		};

		sky_promoted_list& sky_promoted()
		{
			static sky_promoted_list list{};
			return list;
		}
	}

	bool sky_emissive_promoted(u64 hash)
	{
		if (hash == 0)
		{
			return false;
		}

		const sky_promoted_list& list = sky_promoted();
		return std::find(list.values.begin(), list.values.begin() + list.count, hash)
			!= list.values.begin() + list.count;
	}

	bool sky_emissive_promote(u64 hash)
	{
		if (hash == 0 || sky_emissive_promoted(hash))
		{
			return false;
		}

		sky_promoted_list& list = sky_promoted();

		if (list.count >= list.values.size())
		{
			++list.overflow;
			return false;
		}

		list.values[list.count++] = hash;
		return true;
	}

	bool sky_emissive_unpromote(u64 hash)
	{
		// ROUND 39. Used on exactly one path: the promotion was recorded, every resident texture
		// entry then REFUSED the material rebuild, and leaving the hash in the set would re-key
		// every later mesh under an identity whose material never actually changed - a permanent
		// black dome that the counters would report as a success. Undoing it costs nothing
		// because the mesh key fold is read on the NEXT frame, never within the draw that
		// promoted, so no mesh can have moved yet.
		//
		// This is the ONLY way an entry leaves the set, and it runs before any consumer has seen
		// the membership - so sky_emissive_promoted() stays monotone from the point of view of
		// the mesh key, which is what makes folding it into a cache key safe.
		if (hash == 0)
		{
			return false;
		}

		sky_promoted_list& list = sky_promoted();
		const auto end = list.values.begin() + list.count;
		const auto it = std::find(list.values.begin(), end, hash);

		if (it == end)
		{
			return false;
		}

		*it = list.values[list.count - 1];
		list.values[--list.count] = 0;
		return true;
	}

	u32 sky_emissive_promoted_count()
	{
		return sky_promoted().count;
	}

	u32 sky_emissive_promote_overflow()
	{
		return sky_promoted().overflow;
	}

	u32 sky_classify_mode()
	{
		// Default 1 = measure and name, tag nothing, promote nothing: image-identical to round 38
		// by construction. 0 is the hard off (not even the census). 2 promotes. 3 promotes while
		// ignoring the disqualification, which is a real fourth behaviour and not a reserved value
		// - the ceiling is deliberately ABOVE the value the launcher arms, because a clamp that
		// equals the armed value makes a saturated knob and a tuned one print identically. That
		// trap has now cost this project five rounds (STATICINDEXBUDGET most recently, [1,64]
		// against an armed 64).
		static const u32 value = std::min<u32>(env_u32(L"RPCS3_REMIX_SKYCLASSIFY", 1), 3u);
		return value;
	}

	f32 sky_classify_max_extent()
	{
		// Negative sentinel for "unset", like every other float knob here - env_float rejects 0
		// outright, so 0 could never mean "no ceiling" through that path.
		static const f32 env = env_float(L"RPCS3_REMIX_SKYCLASSIFYMAXEXT", -1.f);
		return env >= 0.f ? env : 4.0e6f;
	}

	u32 sky_classify_max_vertices()
	{
		static const u32 value = std::min<u32>(env_u32(L"RPCS3_REMIX_SKYCLASSIFYMAXVTX", 1024), 65535u);
		return value;
	}

	u32 sky_classify_min_vertices()
	{
		// Floor, not ceiling, and it exists because the census named one concrete false positive:
		// albedo AC936E2F25F147B0 on vp=3c9186d8e026cec5 is a FOUR-VERTEX quad spanning 32,331
		// world units with the camera inside it. That is a full-screen backdrop card, not a dome.
		// MEASURED: the smallest vertex count on any row carrying a hand-listed dome hash is 33,
		// so 16 removes that quad and touches nothing else - replaying every gate over every
		// 'Remix sky-census:' row in the 969 MB log takes the admitted set from 19 distinct hashes
		// to 18, and the one it drops is exactly AC936E2F25F147B0.
		static const u32 value = std::min<u32>(env_u32(L"RPCS3_REMIX_SKYCLASSIFYMINVTX", 16), 4096u);
		return value;
	}

	f32 sky_classify_units_per_vertex()
	{
		static const u32 value = std::min<u32>(env_u32(L"RPCS3_REMIX_SKYCLASSIFYUPV", 60), 1000000u);
		return static_cast<f32>(value);
	}

	u32 sky_classify_min_draws()
	{
		static const u32 value = std::clamp<u32>(env_u32(L"RPCS3_REMIX_SKYCLASSIFYMIN", 8), 1u, 4096u);
		return value;
	}

	u32 sky_classify_settle_frames()
	{
		static const u32 value = std::min<u32>(env_u32(L"RPCS3_REMIX_SKYCLASSIFYSETTLE", 60), 1000000u);
		return value;
	}

	namespace
	{
		// Same latched-comma-list shape as sky_emissive_albedos() above. Bound 4: this is a
		// diagnostic aimed at one or two programs at a time, and a census that can be pointed at
		// sixteen programs at once is a census nobody reads.
		struct fp_census_vp_list
		{
			std::array<u64, 4> values{};
			u32 count = 0;
		};

		const fp_census_vp_list& fp_census_vps()
		{
			static const fp_census_vp_list list = []()
			{
				fp_census_vp_list result{};
				wchar_t buffer[400]{};
				const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_FPCENSUSVP", buffer, static_cast<DWORD>(std::size(buffer)));

				if (written == 0 || written >= std::size(buffer))
				{
					return result;
				}

				wchar_t* cursor = buffer;

				while (*cursor && result.count < result.values.size())
				{
					while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
					{
						++cursor;
					}

					wchar_t* end = nullptr;
					const u64 value = ::_wcstoui64(cursor, &end, 16);

					if (end == cursor)
					{
						break;
					}

					if (value != 0)
					{
						result.values[result.count++] = value;
					}

					cursor = end;
				}

				return result;
			}();

			return list;
		}
	}

	bool fp_census_vp_matches(u64 hash)
	{
		const fp_census_vp_list& list = fp_census_vps();
		const auto end = list.values.begin() + list.count;
		return hash != 0 && std::find(list.values.begin(), end, hash) != end;
	}

	u32 fp_census_vp_count()
	{
		return fp_census_vps().count;
	}


	f32 viewmodel_camera_fov_x()
	{
		static const f32 value = env_float(L"RPCS3_REMIX_VMCAMFOVX", 0.f);
		return value;
	}

	f32 viewmodel_camera_fov_y()
	{
		static const f32 value = env_float(L"RPCS3_REMIX_VMCAMFOVY", 0.f);
		return value;
	}

	bool defer_pre_anchor_enabled()
	{
		// env_u32, same reason gauge_anchor_enabled gives: 0 has to restore the immediate submit
		// bit-exactly for the anchor_prev A/B to mean anything.
		static const u32 value = env_u32(L"RPCS3_REMIX_DEFERPREANCHOR", 1);
		return value != 0;
	}

	// --- ROUND 32 -------------------------------------------------------------------------------
	// Restrict pre-anchor deferral to the anchor_prev branch, excluding gauge_anchor_absent. See the
	// long note at the increment site of defer_absent_declined for the measurement. Default 1;
	// 0 reproduces round 31's deferral population exactly. Inert while DEFERPREANCHOR=0.
	bool defer_prev_only_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_DEFERPREVONLY", 1);
		return value != 0;
	}

	// --- ROUND 46 -------------------------------------------------------------------------------
	// Admit tag-only viewmodel draws to the pre-anchor deferral. Full derivation and the four
	// measurements behind it are on the declaration in RemixTransforms.h. env_u32, not env_flag, for
	// the same reason DEFERPREANCHOR uses it: 0 has to restore the round-45 build bit-exactly or the
	// A/B on "does the gun still shake" means nothing.
	bool defer_viewmodel_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_DEFERVIEWMODEL", 0);
		return value != 0;
	}

	u32 defer_pre_anchor_max()
	{
		// Hard cap on the per-frame deferral buffer. A frame that buffers more than this spills the
		// remainder through the old immediate path and counts defer_spilled, so a pathological frame
		// degrades to round-4 behaviour instead of growing without bound.
		static const u32 value = env_u32(L"RPCS3_REMIX_DEFERPREANCHORMAX", 4096);
		return value;
	}

	u32 fp_a2c_mode()
	{
		// env_u32 for the same reason FPKIL uses it: 0 has to restore the always-pass alpha state
		// exactly, because this knob is the bisect for holes appearing in solid geometry.
		//
		// Round 6: 1 (default) = ctrl && reg, the hardware/core semantics; 2 = round 5's ctrl-only
		// replay, kept solely so the see-through regression can be reproduced on demand; 0 = off.
		// See the RemixTransforms.h block: the ctrl term is not an A2C signal at all, it is bit 24
		// of the fragment program's used-temp-register count, which is why mode 2 fired on 658,012
		// draws in a title whose A2C register is never set.
		static const u32 value = env_u32(L"RPCS3_REMIX_FPA2C", 1);
		return value;
	}

	bool fp_a2c_enabled()
	{
		return fp_a2c_mode() != 0;
	}

	bool notex_material_enabled()
	{
		// env_u32, not env_flag: 0 has to restore the material-less submit bit-exactly, because this
		// knob is the bisect for "grey looks worse than white anywhere".
		static const u32 value = env_u32(L"RPCS3_REMIX_NOTEXMAT", 1);
		return value != 0;
	}

	bool skip_shadow_only_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_SKIPSHADOWONLY", 1);
		return value != 0;
	}

	bool walk_sampled_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_WALKSAMPLED", 1);
		return value != 0;
	}

	bool tail_rescue_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_TAILRESCUE", 1);
		return value != 0;
	}

	// --- round 7 ---------------------------------------------------------------------------------

	bool skip_cc_const_writer_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_SKIPCCCONST", 1);
		return value != 0;
	}

	bool main_clip_max_enabled()
	{
		// env_u32, not env_flag: 0 has to restore the previous-frame main clip everywhere, because
		// this changes which draws a "smaller than the main pass" rule refuses.
		static const u32 value = env_u32(L"RPCS3_REMIX_MAINCLIPMAX", 1);
		return value != 0;
	}

	// --- round 8 ---------------------------------------------------------------------------------

	bool wdivide_walk_enabled()
	{
		// env_u32, not env_flag: the useful setting is the off one. 0 restores the single
		// last_temp_writer selection inside match_wdivide bit-exactly, which is the A/B that
		// attributes the giant-geometry fix - with 0 the ship, the "incorrect bones" character and
		// the NPC weapons must go back to being enormous.
		static const u32 value = env_u32(L"RPCS3_REMIX_WDIVWALK", 1);
		return value != 0;
	}

	// --- round 9 ---------------------------------------------------------------------------------

	bool fp_vertex_colour_replay_enabled()
	{
		// The effects fix. 0 restores the parity blend-extension fill for every draw bit-exactly -
		// i.e. colour = Texture only, isVertexColorBakedLighting = 1 - which is the A/B that
		// attributes the nectar pulse regaining its yellow. env_u32 rather than env_flag because
		// the useful setting is the off one.
		static const u32 value = env_u32(L"RPCS3_REMIX_FPVCOL", 1);
		return value != 0;
	}

	// --- ROUND 41 -------------------------------------------------------------------------------
	// Refuse a CONSTANT-route vertex colour whose resolved RGB is (near) black on a TEXTURED draw.
	// Default ON. Full derivation at the guard site in apply_vertex_colour; the short version is
	// that `Remix vcolroute:` measures cval=[0 0 0 1] on vp=b01bfce3fc580e3b, and modulating an
	// albedo by that can only delete it. 0 restores round-40 behaviour on that branch exactly.
	bool vcol_constant_black_guard_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_VCOLCONSTBLACK", 1);
		return value != 0;
	}

	bool vertex_colour_modulate_enabled()
	{
		// Severable independently of FPVCOL because it is the half that changes MESH CONTENT
		// HASHES: apply_vertex_colour writes into the scratch vertices, and the vertex colour is
		// hashed into the mesh key, so an animated vertex colour makes a new mesh per animation
		// step. That is already true today for the material-less population (the pulse); this
		// extends it to textured draws whose fragment program PROVES the modulate. 'mesh_created'
		// on the live line is the watch; 0 restores the '!material' gate exactly.
		static const u32 value = env_u32(L"RPCS3_REMIX_VCOLMOD", 1);
		return value != 0;
	}

	bool ucode_store_enabled()
	{
		// Write the raw vertex ucode of every program this backend refuses to decode into
		// bin\remix_ucode\. The shader cache under bin\cache\...\shaders_cache\raw is populated
		// ONLY by pipeline compilation (rsx_cache.h::store, called from the GL/Vulkan backends),
		// and the Remix backend never compiles a pipeline - so every program first met under Remix
		// has been unreadable offline. Once per program per run, size-checked, own directory.
		static const u32 value = env_u32(L"RPCS3_REMIX_UCODESTORE", 1);
		return value != 0;
	}

	bool mad_chain_mixed_lanes_enabled()
	{
		// The Selva canopy arm. 0 restores match_mad_chain_pass's single-source rule bit-exactly,
		// which is what a mis-placed tree canopy bisects to.
		static const u32 value = env_u32(L"RPCS3_REMIX_MADCHAINMIX", 1);
		return value != 0;
	}

	bool mad_lane_map_enabled()
	{
		// Round 17, the Selva tree tops. Accepts a mixed-lane group whose per-vertex divide parked
		// the position components in lanes other than x, y, z - but only when that divide's own
		// writemask and source swizzle PROVE which lane carries which component, so nothing is
		// guessed. Also lets the same permuted divide satisfy the wdivide step, without which the
		// rescued group would be applied to the raw quantised attribute and the canopy would render
		// blown apart. 0 restores round 9's identity-lane rule bit-exactly, which is what a
		// mis-placed or exploded tree canopy bisects to.
		static const u32 value = env_u32(L"RPCS3_REMIX_MADLANEMAP", 1);
		return value != 0;
	}

	namespace
	{
		struct clamp_albedo_list
		{
			std::array<u64, 16> values{};
			u32 count = 0;
		};

		const clamp_albedo_list& clamp_albedos()
		{
			static const clamp_albedo_list list = []()
			{
				clamp_albedo_list result{};
				wchar_t buffer[320]{};
				const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_CLAMPALBEDO", buffer, static_cast<DWORD>(std::size(buffer)));

				if (written == 0 || written >= std::size(buffer))
				{
					return result;
				}

				wchar_t* cursor = buffer;

				while (*cursor && result.count < result.values.size())
				{
					while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
					{
						++cursor;
					}

					wchar_t* end = nullptr;
					const u64 value = ::_wcstoui64(cursor, &end, 16);

					if (end == cursor)
					{
						break;
					}

					if (value != 0)
					{
						result.values[result.count++] = value;
					}

					cursor = end;
				}

				return result;
			}();

			return list;
		}
	}

	bool clamp_albedo_matches(u64 hash)
	{
		const clamp_albedo_list& list = clamp_albedos();
		const auto end = list.values.begin() + list.count;
		return hash != 0 && std::find(list.values.begin(), end, hash) != end;
	}

	u32 clamp_albedo_count()
	{
		return clamp_albedos().count;
	}

	u32 tail_rescue_age()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_TAILRESCUEAGE", 30);
		return value;
	}

	f32 guest_light_lum()
	{
		// env_float rejects non-positive values, so 0 cannot be expressed here - which is correct:
		// a luminance floor of 0 would census every submitted draw in the scene.
		static const f32 value = env_float(L"RPCS3_REMIX_GUESTLIGHTLUM", 0.7f);
		return value;
	}

	f32 guest_light_max_extent()
	{
		static const f32 value = env_float(L"RPCS3_REMIX_GUESTLIGHTMAXEXT", 6.f);
		return value;
	}

	// --- ROUND 41: the extent ceiling now applies to an explicitly LISTED albedo too -------------
	// It always applied to the census and to GUESTLIGHTAUTO; it did not apply to a hash the user
	// named on GUESTLIGHTALBEDO/ALBEDO2, and nothing in the file ever argued for that exemption.
	// MEASURED why it matters, round-40 play-test, all 31 'Remix guest-light:' lines, one albedo
	// (71D189E9B559A7F9) on one (vp, fp) pair: world extent spans 0.5385 .. 83.18, a 154x range.
	// Nine rows sit at 0.5385 .. 1.731 (the bulbs; 1.731 is the user's own pick) and twenty-two sit
	// at 7.893 .. 83.18 (a large mesh sharing the texture). The gap 1.731 -> 7.893 is 4.6x and the
	// default ceiling of 6 lands inside it, so this separates the two populations on measured data
	// rather than on taste. The 83.18 row produced radius=29.1 - the "too big, not aligned with the
	// bulbs" light in the user's screenshot.
	//
	// Default ON because the round-40 behaviour is wrong on this title and on any title where one
	// texture is shared between a fixture and a larger prop. 0 restores round 40 bit-exactly.
	bool guest_light_list_extent_enabled()
	{
		static const bool value = env_u32(L"RPCS3_REMIX_GUESTLIGHTLISTEXT", 1) != 0;
		return value;
	}

	// --- ROUND 41: mode 2 is "glow cards only", and it is the discriminator ----------------------
	// 0 = off (default, round-40 behaviour). 1 = the whole census population, fixtures AND glow
	// cards - the round-6 meaning, unchanged. 2 = glow cards only.
	//
	// Why 2 exists, MEASURED over the round-40 play-test's 'Remix light-candidate:' rows grouped by
	// (albedo, state):
	//
	//   F613BD83DAF2B4E2  glowcard  n=82  vtx=[22,23,26]   lum=0.8155  rgb=[0.8424 0.8314 0.5781]
	//   0F86FCEDC4226D3B  glowcard  n=64  vtx=[4]          lum=0.9375  rgb=[0.9375 0.9375 0.9375]
	//   A0D0AB03F0BB1D3C  glowcard  n=59  vtx=[200,36,64]  lum=1
	//   099D2DA136CEA2C4  glowcard  n=53  vtx=[40]         lum=0.9938
	//   9CA366166DF12A90  glowcard  n=27  vtx=[116,12,16]  lum=0.9813  rgb=[1 1 0.7412]
	//   9E95F66ECE26BE29  fixture   n=23  vtx=[216,8,846]  lum=0.7226
	//   16B46EA28EEDFC8E  fixture   n=22  vtx=[8]          lum=0.7223
	//
	// Two separable populations. The glow cards are small additive billboards (4..200 vertices)
	// carrying the game's own lamp tints - warm white, yellow, pure white. The 'fixture' rows are
	// the large opaque housings (216, 846 vertices) at a flat lum ~0.72, which is the housing's
	// metal, not a light.
	//
	// THE HYPOTHESIS THIS MODE TESTS, and it is INFERRED, not measured: Haze draws an additive glow
	// card over a fixture that is LIT and omits it for one that is not. If that holds, "create a
	// light where a glow card is drawn" reproduces the raster reference's "only some bulbs are lit"
	// with no per-bulb list at all, and gives each light the card's own measured mean_rgb and its
	// own extent-derived radius for free.
	//
	// It also retires the (vp, fp) key for this population: F613BD83DAF2B4E2 alone is drawn by FIVE
	// vertex programs and TWELVE fragment programs, so no single pair can name it. Mode 2 does not
	// consult vp_ok/fp_ok at all - those gate trigger_match only.
	//
	// REFUTING READING, pre-registered: if the plant ends up with many more lights than it has
	// visibly lit fixtures, or guest_light_capped starts climbing, the card is not the "is lit"
	// signal and the discriminator is something else. guest_lights= and guest_light_capped= on
	// 'Remix live:' size it directly.
	u32 guest_light_auto_mode()
	{
		static const u32 value = std::min(env_u32(L"RPCS3_REMIX_GUESTLIGHTAUTO", 0), 2u);
		return value;
	}

	// --- ROUND 48: the STATIC-FIXTURE gate, and it is what makes GUESTLIGHTAUTO usable ----------
	// Round 41 armed GUESTLIGHTAUTO=2 (glow cards only) and had to disable it again for two faults
	// that are one fault: the trigger lit SOLDIERS as they walked, and scattered spheres at
	// positions no fixture occupies. Both are the same thing - the AUTO population is keyed on
	// render state (blended, no depth write, bright texture) and nothing in that key says the draw
	// is STANDING STILL. A lamp is bolted to a wall; a suit panel on a walking NPC and a
	// camera-adjacent billboard are not.
	//
	// So require the candidate to re-appear in the SAME quantised cell on N distinct frames before
	// it may mint a light. A static fixture hits its cell every frame it is drawn and confirms in N
	// frames; a moving emitter leaves a trail of cells and never reaches N in any of them. The cell
	// quantisation is the one the light hash already uses (0.25 world units), so "same cell" is the
	// same equivalence the dedup has always applied - this adds a time axis to it, nothing else.
	//
	// 0 = OFF, which is round-41 behaviour bit-exactly: every AUTO candidate mints a light on first
	// sight. The knob is the whole mechanism; there is no second switch.
	//
	// NOTE THE INTERACTION WITH FLICKER, because it is the user's other request and it is easy to
	// break here. A confirmed cell STAYS confirmed (see m_guest_light_cells) so that a bulb which
	// flickers off, loses its light to GUESTLIGHTIDLE, and comes back on re-lights on the FIRST
	// frame its card returns rather than serving the N-frame apprenticeship again. Without that,
	// any stability window longer than the flicker period would make a flickering bulb permanently
	// dark - the exact opposite of what was asked for.
	// --- ROUND 49: the camera sanity gate ------------------------------------------------------
	// Round 48 shipped DRAWAUDIT=0 and the next play-test came back as "the entire area is warping
	// like crazy" - a field of untextured fragments in a black void. The dev menu named the cause
	// outright: MAIN camera at FOV 114.6 with near/far 3.7 / 6.1, against this title's usual 72.0
	// and 8.1 / 13991.5. A 6.1-unit far plane clips the whole level.
	//
	// That run turns out NOT to have been a DRAWAUDIT failure - it ran with camlock=0 and every
	// other knob at its built-in default, i.e. the launcher environment was never applied - but the
	// FAILURE MODE is real and is documented for this backend: remixapi's SetupCamera performs none
	// of the shear/FOV rejection the D3D9 path applies, so a degenerate or ortho UI matrix that wins
	// the election becomes a live world camera and NOTHING says so. The scene is destroyed and the
	// log is silent.
	//
	// 0 = CENSUS ONLY and it is the shipped value: measure, name it on 'Remix camera-sanity:',
	// count it as cam_insane, and submit the camera anyway. 1 = refuse, taking the same
	// no-camera path submit_camera already has for cam_fallback.
	//
	// SHIPPED OFF DELIBERATELY. This file's own round-6 note says "ship the measurement armed and
	// the behaviour off", and round 48 ignored that and shipped a behaviour change on an argument
	// rather than on a reading. A refusal that is too tight replaces a warped scene with a frozen
	// one, which is not obviously better; the census costs nothing and says whether the thresholds
	// are right before anything acts on them.
	u32 camera_sanity_mode()
	{
		static const u32 value = std::min(env_u32(L"RPCS3_REMIX_CAMSANITY", 0), 1u);
		return value;
	}

	// Fractional deviation of vertical FOV from the title's own latched reference before a camera is
	// called insane. 0.5 = 50%. The measured failure is 72.0 -> 114.6, i.e. +59%, so 0.5 catches it
	// with margin; a rifle ADS narrows the FOV (72 -> ~50, i.e. -31%) and must NOT trip it, which is
	// what sets the floor. A RELATIVE test is used rather than absolute limits on purpose: 114.6 is
	// not an absurd FOV for some titles, so only the title's own history separates it from a
	// legitimate wide angle. env_float rejects 0.
	f32 camera_sanity_tolerance()
	{
		static const f32 value = env_float(L"RPCS3_REMIX_CAMSANITYTOL", 0.5f);
		return value;
	}

	// A MOTION test, where GUESTLIGHTSTABLE is a DWELL test, and the difference is the whole point.
	//
	// The dwell gate asks "did this stay put?", and a soldier on an idle animation stays put. Round
	// 48 reasoned that "a lamp DOES NOT MOVE", which is true -- but so is "an idle NPC does not
	// move", so no threshold on that axis separates them. It can only make the NPC stand still for
	// longer before it lights. Measured on Haze at STABLE=30, every mint grouped by trigger albedo:
	//
	//   F613BD83DAF2B4E2 (suit glow card)  26 distinct cells, 35 mints
	//   422F2F6C911FC378 (fixture)          1 distinct cell,   4 mints
	//   0F86FCEDC4226D3B (fixture)          1 distinct cell,   1 mint
	//   06201102B0E4566E (fixture)          1 distinct cell,   2 mints
	//
	// Every real fixture: one cell. The thing on legs: twenty-six. Unlike dwell time this is not a
	// property an idle NPC can fake -- faking it would mean having never been anywhere else, which
	// for something that walked into the room is impossible.
	//
	// 4 leaves a 6x margin over the fixtures and disqualifies the suit card six cells into its walk.
	// Default 0 so this lands dark and is armed from a config, per this project's standing rule.
	//
	// THE PAYOFF IS NOT ONLY THE SOLDIERS. With this on, GUESTLIGHTSTABLE can go back to 0 and real
	// fixtures light on FIRST SIGHT again, as in round 41, instead of serving a dwell apprenticeship
	// that at 600 frames is ten seconds of standing still before a lamp comes on.
	u32 guest_light_max_cells()
	{
		static const u32 value = std::min(env_u32(L"RPCS3_REMIX_GUESTLIGHTCELLS", 0), 64u);
		return value;
	}

	u32 guest_light_stable_frames()
	{
		static const u32 value = std::min(env_u32(L"RPCS3_REMIX_GUESTLIGHTSTABLE", 0), 600u);
		return value;
	}

	bool guest_light_auto_enabled()
	{
		// Default OFF, deliberately: the last generalisation of fixture identity put lights on
		// doors. The census ships armed, the behaviour does not.
		return guest_light_auto_mode() != 0;
	}

	u32 nectar_mode()
	{
		// 1 = round-4 behaviour: publish haze.nectar_disruption=1 on every frame the pass is
		// submitted. 0 = never publish, which kills the fork-side greyscale outright. 2 is reserved
		// for "publish on a discriminator" once the lightpass census names one.
		static const u32 value = env_u32(L"RPCS3_REMIX_NECTARMODE", 1);
		return value;
	}

	bool light_pass_census_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_LIGHTPASSCENSUS", 1);
		return value != 0;
	}

	u64 nectar_disruption_vp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_NECTARVP");
		return value;
	}

	u64 nectar_disruption_fp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_NECTARFP");
		return value;
	}

	u64 world_identity_fp_pair_vp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_WORLDIDENTITYFPPAIRVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();
		return value;
	}

	u64 world_identity_fp_pair_fp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_WORLDIDENTITYFPPAIRFP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();
		return value;
	}

	u64 world_identity_opaque_fp_pair_vp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIRVP");
		return value;
	}

	u64 world_identity_opaque_fp_pair_fp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIRFP");
		return value;
	}

	u64 world_identity_opaque_fp_pair2_vp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIR2VP");
		return value;
	}

	u64 world_identity_opaque_fp_pair2_fp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIR2FP");
		return value;
	}

	u64 world_identity_opaque_fp_pair3_vp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIR3VP");
		return value;
	}

	u64 world_identity_opaque_fp_pair3_fp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIR3FP");
		return value;
	}

	u64 world_identity_opaque_fp_pair4_vp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIR4VP");
		return value;
	}

	u64 world_identity_opaque_fp_pair4_fp_hash()
	{
		static const u64 value = read_hash_env(L"RPCS3_REMIX_WORLDIDENTITYOPAQUEFPPAIR4FP");
		return value;
	}

	u64 trace_albedo_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_TRACEALBEDO", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();

		return value;
	}

	u64 trace_albedo_vp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_TRACEALBEDOVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();
		return value;
	}

	u64 camera_lock_vp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_CAMLOCKVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();

		return value;
	}

	u64 camera_fallback_vp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_CAMFALLBACKVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();

		return value;
	}

	u64 camera_fallback_vp2_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_CAMFALLBACKVP2", buffer, static_cast<DWORD>(std::size(buffer)));

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

	bool smooth_normals_enabled()
	{
		static const bool env = env_flag(L"RPCS3_REMIX_SMOOTHNORMALS");
		return env || g_cfg.video.remix.smooth_normals;
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

	// --- round 36: which quantity the anchor test compares. Derivation in RemixTransforms.h -------
	//
	// env_u32 and NOT env_float, and that is the whole reason this knob is not simply folded into
	// sky_max_anchor(): env_float rejects 0 outright, so a knob whose 0 means "legacy" would
	// silently read as something else - which is the exact SKYANCHOR=0 defect round 32 measured.
	//
	// Clamp stated against the value the launcher arms: SKYANCHORMODE=2 against a ceiling of 3, so
	// the armed value is inside the clamp and the knobs= block reports it back unchanged. The
	// ceiling is deliberately ABOVE the armed value - a ceiling that equals what the launcher sets
	// is the round-31 trap, a knob that can only be turned one way.
	u32 sky_anchor_mode()
	{
		static const u32 value = std::min<u32>(env_u32(L"RPCS3_REMIX_SKYANCHORMODE", 0), 3u);
		return value;
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
		// Default 2 (tag).
		//
		// --- ROUND 34: THE CLAIM THIS BLOCK USED TO MAKE IS MEASURED FALSE. DELETED. ------------
		// It said "this cannot currently change a pixel: the runtime this backend targets has no
		// bit 26 to receive", reasoning from remix_c.h stopping at SMOOTH_NORMALS = 1 << 24 and
		// toRtCategories() mapping bits 0..24 under a static_assert on
		// InstanceCategories::Count == 25. Every one of those observations is true and the
		// conclusion drawn from them is wrong, because bit 26 is deliberately NOT an
		// InstanceCategories member - it is consumed by categoryToCameraType() and never reaches
		// toRtCategories() at all. Commit 6476faea's own message says so.
		//
		// MEASURED by disassembling the DEPLOYED bin\remix\d3d9.dll (sha256
		// 16a0b512f33ebb66a89ac703e75289d9e008558a13d2c9a6a5455b0be7c40858, size 240657408, fnv1a
		// 09653f484ec94dc0). In toRtDrawState at RVA 0x001ED290:
		//     0x001ED309  mov   eax,[rdx+0x10]   ; remixapi_InstanceInfo::categoryFlags
		//     0x001ED30C  bt    eax,0x1a         ; bit 26 = VIEW_MODEL
		//     0x001ED312  mov   ebx,1            ; CameraType::ViewModel
		//     0x001ED34D  mov   [rbp+0x1f4],eax  ; DrawCallState::cameraType  <- load-bearing
		// The +0x10 operand is verified against remix_c.h's layout (sType@0, pNext@8,
		// categoryFlags@0x10, mesh@0x18, transform@0x20) by the transform rows loaded from
		// +0x20/0x30/0x40 immediately after. The build also carries the Sky->Main clamp
		// (cmp ebx,4 / cmove eax,0) and the API version bump to 0.1000.1
		// (mov eax,0x03E80001 at RVA 0x000EDCD1), both introduced by 6476faea specifically.
		// toRtDrawState is byte-identical across all 15,328 bytes to
		// dxvk-remix-numos3\_output\d3d9.dll.
		//
		// SO THE CATEGORY PATH IS LIVE, and the remaining gate is the CAMERA, not the bit:
		// InstanceManager::createViewModelInstances early-returns on
		// !cameraManager.isCameraValid(CameraType::ViewModel) after
		// cleanupAllPersistentViewModelInstances() - it does NOT mask the instance, so a tagged
		// draw with no viewmodel camera renders as ordinary world geometry (which is exactly what
		// round 20 measured on 123 tagged draws). The mask=0 path belongs to gate 3,
		// RtxOptions::PlayerModel::enableInPrimarySpace(), which is false and absent from rtx.conf.
		// Do not repeat the launcher's claim that a mis-tagged draw is removed from the world pass;
		// on this configuration it is not.
		//
		// Also measured: an external draw can reach CameraType::ViewModel ONLY through this bit.
		// remixapi_SetupCamera's REMIXAPI_CAMERA_TYPE_VIEW_MODEL registers matrices in a camera
		// slot and cannot make any instance reference it, and submitExternalDraw touches
		// ExternalDrawState::cameraType at exactly one site - getCamera(state.cameraType) - and
		// never copies it into state.drawCall.cameraType.
		//
		// RPCS3_REMIX_VIEWMODEL=0 restores 81af315's *tagging* exactly; it no longer
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

	// --- round 23: a PER-LEVEL sun, keyed on the sky dome's own albedo -------------------------
	//
	// sun_direction() above is ONE vector for the whole process, which is the defect: Haze's sky is
	// per-area and the backend has always aimed the same sun at every level. The key used here is
	// the sky dome's albedo hash, because that is a per-area value the backend already sees change
	// (RPCS3_REMIX_SKYEMISSIVE lists two of them today) and it needs no level-change signal from the
	// guest at all.
	//
	// MEASURED, offline, on the dumped Selva dome (bin\remix_tex\unit0_D1A6D1B27ADE6232_2048x1024.bmp,
	// written by dump_texture): the sun IS painted into the dome texture. Peak Rec.709 luma 249.2
	// (rgb 255,251,214) at texel (1340,513); 112 texels sit within 2 % of that peak and their
	// centroid is uv (0.6769, 0.5015); the whole top half of the image (v < 0.5) is black, i.e. the
	// panorama occupies v in [0.5, 1]. The bright region is a broad warm glow, not a hard disc
	// (zero texels reach luma 250, 3060 reach 220), so the derivation below uses the luminance-
	// weighted centroid of the near-peak texels rather than the single brightest one.
	struct sun_map_entry
	{
		u64 albedo = 0;
		f32 travel[3] = { 0.f, 0.f, 0.f };
	};

	struct sun_map_list
	{
		std::array<sun_map_entry, 8> values{};
		u32 count = 0;
	};

	namespace
	{
		const sun_map_list& sun_map_entries()
		{
			// Latched once, same shape as every other list knob in this file.
			// RPCS3_REMIX_SUNMAP=<skyalbedo>:<x>,<y>,<z>;<skyalbedo>:<x>,<y>,<z>
			// The vector is the direction the light TRAVELS (sun -> scene), the same convention
			// RPCS3_REMIX_SUNDIR uses, and it is normalised here so a hand-typed value need not be.
			static const sun_map_list list = []()
			{
				sun_map_list result{};

				wchar_t buffer[512]{};
				const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_SUNMAP", buffer,
					static_cast<DWORD>(std::size(buffer)));

				if (written == 0 || written >= std::size(buffer))
				{
					return result;
				}

				const wchar_t* cursor = buffer;

				while (*cursor && result.count < result.values.size())
				{
					while (*cursor == L';' || *cursor == L' ' || *cursor == L'\t')
					{
						++cursor;
					}

					if (!*cursor)
					{
						break;
					}

					wchar_t* end = nullptr;
					const unsigned long long albedo = ::wcstoull(cursor, &end, 16);

					if (end == cursor)
					{
						break;
					}

					cursor = end;

					while (*cursor == L' ')
					{
						++cursor;
					}

					if (*cursor != L':')
					{
						break;
					}

					++cursor;

					f32 parsed[3]{};
					u32 count = 0;

					while (count < 3 && *cursor)
					{
						wchar_t* vend = nullptr;
						const double v = ::wcstod(cursor, &vend);

						if (vend == cursor)
						{
							break;
						}

						parsed[count++] = static_cast<f32>(v);
						cursor = vend;

						while (*cursor == L',' || *cursor == L' ')
						{
							++cursor;
						}
					}

					if (count == 3 && albedo != 0 && normalize3(parsed))
					{
						sun_map_entry& slot = result.values[result.count++];
						slot.albedo = static_cast<u64>(albedo);
						slot.travel[0] = parsed[0];
						slot.travel[1] = parsed[1];
						slot.travel[2] = parsed[2];
					}

					while (*cursor && *cursor != L';')
					{
						++cursor;
					}
				}

				return result;
			}();

			return list;
		}
	}

	u32 sun_map_count()
	{
		return sun_map_entries().count;
	}

	bool sun_map_lookup(u64 albedo_hash, f32 (&out)[3])
	{
		const sun_map_list& list = sun_map_entries();

		for (u32 i = 0; i < list.count; ++i)
		{
			if (list.values[i].albedo == albedo_hash)
			{
				out[0] = list.values[i].travel[0];
				out[1] = list.values[i].travel[1];
				out[2] = list.values[i].travel[2];
				return true;
			}
		}

		return false;
	}

	bool sun_sky_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_SUNSKY", 0);
		return value != 0;
	}

	u32 sun_sky_min_vertices()
	{
		// A 4-vertex HUD quad must never win the UV match: the derivation needs a dome, and a dome
		// is a tessellated shell. 64 is below every sky-dome vertex count seen in the census
		// (134, 180, 288, 1446) and far above the 4-vertex cards that share the dome's program.
		static const u32 value = env_u32(L"RPCS3_REMIX_SUNSKYMINVTX", 64);
		return value;
	}

	u32 sun_sky_peak_percent()
	{
		// Clamped, not validated-and-refused: this only widens or narrows a centroid window, so an
		// out-of-range value has a sane nearest meaning and refusing it would silently disable the
		// feature. 100 is "peak texels only", 50 is as broad as the header's measurement supports.
		static const u32 value = std::clamp<u32>(env_u32(L"RPCS3_REMIX_SUNSKYPEAKFRAC", 98), 50, 100);
		return value;
	}

	bool sun_sky_downward_required()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_SUNSKYDOWN", 1);
		return value != 0;
	}

	// --- round 23: force a listed vertex program onto the 2D compositor ------------------------
	//
	// is_screen_space_draw() decides UI-vs-world from exactly two live per-draw inputs: the outer
	// constant block read out of RSX constant memory, and depth_write_enabled(). For a program that
	// is not statically fingerprinted screen_space, the SAME vertex program therefore flips between
	// the compositor and world geometry frame to frame - which is precisely the reported HUD defect.
	//
	// MEASURED on the round-22 run (bin\log\RPCS3.log, title=BLUS30094, pid 20048): the nectar/health
	// gauge program 2f64c2f8ffd6add1 appears on BOTH paths in one session - 277 'Remix uiwrap:' lines
	// with route=2d (composited) AND 'Remix sky-census:'/'Remix fpcandidate:' lines that place it as
	// world geometry 1.5 units in front of the eye
	// (raw=[-0.02 -0.765 0]..[0.02 -0.725 0], origin=[-5.2743 -0.000998 -40.476],
	// cam=[-5.2743 0 -41.982], eye_dist=1.679, dw=0, blend=1). Its raw vertices are already inside
	// the NDC cube with z = 0, so the world placement is the wrong answer for every one of them.
	//
	// The depth-write clause is kept as the guard: it is the one input that says "this draw wants to
	// occlude", and every gauge draw measured reads depth_write=0.
	namespace
	{
		struct ui_force_vp_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		const ui_force_vp_list& ui_force_vps()
		{
			static const ui_force_vp_list list = []()
			{
				ui_force_vp_list result{};
				wchar_t buffer[400]{};
				const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_UIFORCEVP", buffer,
					static_cast<DWORD>(std::size(buffer)));

				if (written == 0 || written >= std::size(buffer))
				{
					return result;
				}

				wchar_t* cursor = buffer;

				while (*cursor && result.count < result.values.size())
				{
					while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
					{
						++cursor;
					}

					wchar_t* end = nullptr;
					const u64 value = ::_wcstoui64(cursor, &end, 16);

					if (end == cursor)
					{
						break;
					}

					if (value != 0)
					{
						result.values[result.count++] = value;
					}

					cursor = end;
				}

				return result;
			}();

			return list;
		}
	}

	bool ui_force_vp_matches(u64 vp_hash)
	{
		const ui_force_vp_list& list = ui_force_vps();
		const auto end = list.values.begin() + list.count;
		return vp_hash != 0 && std::find(list.values.begin(), end, vp_hash) != end;
	}

	u32 ui_force_vp_count()
	{
		return ui_force_vps().count;
	}

	// --- round 36: the (vp, fp) pair form of the same route. Derivation in RemixTransforms.h -----
	//
	// Deliberately a straight copy of hide_pair_vp_hash()/hide_pair_fp_hash() in shape: same buffer
	// size, same "written == 0 || written >= size" refusal, same base-16 parse, same "empty either
	// half disarms" rule. Two gates that mean the same thing should not have two different failure
	// modes for a mistyped hash.
	u64 ui_force_pair_vp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_UIFORCEPAIRVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();
		return value;
	}

	u64 ui_force_pair_fp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_UIFORCEPAIRFP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();
		return value;
	}

	bool ui_force_pair_matches(u64 vp_hash, u64 fp_hash)
	{
		const u64 want_vp = ui_force_pair_vp_hash();
		const u64 want_fp = ui_force_pair_fp_hash();

		// An empty EITHER half disarms the route, and this is the line that makes the play-test
		// card's one-line revert true: blank RPCS3_REMIX_UIFORCEPAIRVP and nothing is forced.
		if (want_vp == 0 || want_fp == 0 || vp_hash == 0 || fp_hash == 0)
		{
			return false;
		}

		return vp_hash == want_vp && fp_hash == want_fp;
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

	// --- round 10 --------------------------------------------------------------------------------

	bool camera_clip_gate_enabled()
	{
		// env_u32, not env_flag: 0 has to restore today's admission bit-exactly, because this knob
		// is the attribution A/B for the far-away "portal" frames coming back.
		static const u32 value = env_u32(L"RPCS3_REMIX_CAMCLIPGATE", 1);
		return value != 0;
	}

	bool camera_fallback_relatch_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_CAMFBRELATCH", 1);
		return value != 0;
	}

	bool gauge_anchor_sticky_enabled()
	{
		// The Selva follow-the-camera fix. 0 restores first-draw-wins bit-exactly, which is the one
		// relaunch that has to bring the symptom back for the attribution to mean anything.
		static const u32 value = env_u32(L"RPCS3_REMIX_ANCHORSTICKY", 1);
		return value != 0;
	}

	u32 gauge_donor_best()
	{
		// ROUND 44b. Whole world units, 0 = OFF = round 43 byte for byte. Replaces round 44's
		// RPCS3_REMIX_GAUGEDONORMAXT, which is REMOVED FROM THE BUILD after its play-test: refusing
		// an off-origin donor parked it, promotion then installed it one frame LATE, and a late
		// gauge warps the whole scene under camera rotation. This never refuses and never parks -
		// the frame's first donor installs immediately as always, and a later donor of the same
		// frame may replace it only when at least twice as close to the world origin.
		//
		// env_u32, not env_float: 0 has to mean OFF and be reachable, and env_float treats any
		// non-positive value as unset.
		static const u32 value = env_u32(L"RPCS3_REMIX_GAUGEDONORBEST", 0);
		return value;
	}

	bool vertex_colour_bgra_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_VCOLBGRA", 1);
		return value != 0;
	}

	bool fp_vcol_alpha_gate_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_FPVCOLALPHAGATE", 1);
		return value != 0;
	}

	f32 fp_vcol_emissive()
	{
		// Parsed here rather than through env_float on purpose: env_float treats any non-positive
		// value as "unset" and returns its fallback, so RPCS3_REMIX_FPVCOLEMISSIVE=0 would silently
		// mean 1.0 and the documented sever would not exist. 0 must be expressible.
		static const f32 value = []() -> f32
		{
			wchar_t buffer[64]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_FPVCOLEMISSIVE", buffer,
				static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 1.f;
			}

			const f32 parsed = static_cast<f32>(::_wtof(buffer));
			return (std::isfinite(parsed) && parsed >= 0.f) ? parsed : 1.f;
		}();

		return value;
	}

	bool fp_vcol_additive_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_FPVCOLADDITIVE", 1);
		return value != 0;
	}

	namespace
	{
		struct viewmodel_albedo_list
		{
			std::array<u64, 16> values{};
			u32 count = 0;
		};

		// Same latched-comma-list shape as emissive_albedos() / clamp_albedos(): parsed once, bounded,
		// separators comma / semicolon / space / tab so a value pasted straight out of a pick line or
		// the dev menu works as typed.
		const viewmodel_albedo_list& viewmodel_albedos()
		{
			static const viewmodel_albedo_list list = []()
			{
				viewmodel_albedo_list result{};
				wchar_t buffer[400]{};
				const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_VIEWMODELALBEDO", buffer,
					static_cast<DWORD>(std::size(buffer)));

				if (written == 0 || written >= std::size(buffer))
				{
					return result;
				}

				wchar_t* cursor = buffer;

				while (*cursor && result.count < result.values.size())
				{
					while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
					{
						++cursor;
					}

					wchar_t* end = nullptr;
					const u64 value = ::_wcstoui64(cursor, &end, 16);

					if (end == cursor)
					{
						break;
					}

					if (value != 0)
					{
						result.values[result.count++] = value;
					}

					cursor = end;
				}

				return result;
			}();

			return list;
		}
	}

	bool viewmodel_albedo_matches(u64 hash)
	{
		const viewmodel_albedo_list& list = viewmodel_albedos();
		const auto end = list.values.begin() + list.count;
		return hash != 0 && std::find(list.values.begin(), end, hash) != end;
	}

	u32 viewmodel_albedo_count()
	{
		return viewmodel_albedos().count;
	}

	bool viewmodel_anchor_geometry_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_VMANCHORGEO", 1);
		return value != 0;
	}

	bool viewmodel_albedo_camera_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_VMALBEDOCAM", 1);
		return value != 0;
	}

	bool mad_accumulate_walk_enabled()
	{
		// The flower arm. 0 restores the 'mad-src0-not-input' refusal bit-exactly, which is what a
		// wrongly-placed decoration bisects to.
		static const u32 value = env_u32(L"RPCS3_REMIX_MADACCUM", 1);
		return value != 0;
	}

	// --- round 11 --------------------------------------------------------------------------------

	bool fp_vcol_route_enabled()
	{
		// The vertex-colour route gate. 0 restores round 10's route-blind replay bit-exactly, which
		// is what invisible foliage under VCOLMOD=1 bisects to.
		static const u32 value = env_u32(L"RPCS3_REMIX_FPVCOLROUTE", 1);
		return value != 0;
	}

	bool vcol_fold_enabled()
	{
		// Severable from the route gate in both directions: with this off the scaled routes still
		// replay, but with the raw attribute (round 10's value), so "is the route right" and "is the
		// fold right" are separately answerable from one session each.
		static const u32 value = env_u32(L"RPCS3_REMIX_VCOLFOLD", 1);
		return value != 0;
	}

	bool fp_vcol_sky_gate_enabled()
	{
		// 0 is the regression repro, not a setting to leave off: it lets the 82-vertex sky dome take
		// the emissive additive material the first time it draws with a camera present.
		static const u32 value = env_u32(L"RPCS3_REMIX_FPVCOLSKYGATE", 1);
		return value != 0;
	}

	bool ucode_store_fp_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_UCODESTOREFP", 1);
		return value != 0;
	}

	bool haze_fade_enabled()
	{
		// The backdrop plane's fade. 0 restores the opaque card - i.e. the black wall - which is the
		// A/B that attributes the veil.
		static const u32 value = env_u32(L"RPCS3_REMIX_HAZEFADE", 1);
		return value != 0;
	}

	bool fx_ref_probe_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_FXREFPROBE", 1);
		return value != 0;
	}

	// --- round 18 --------------------------------------------------------------------------------

	namespace
	{
		struct fx_ref_vp_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		const fx_ref_vp_list& fx_ref_list()
		{
			static const fx_ref_vp_list list = []()
			{
				fx_ref_vp_list result{};
				wchar_t buffer[160]{};
				const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_FXREFVP", buffer, static_cast<DWORD>(std::size(buffer)));

				if (written == 0 || written >= std::size(buffer))
				{
					return result;
				}

				wchar_t* cursor = buffer;

				while (*cursor && result.count < result.values.size())
				{
					while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
					{
						++cursor;
					}

					wchar_t* end = nullptr;
					const u64 value = ::_wcstoui64(cursor, &end, 16);

					if (end == cursor)
					{
						break;
					}

					if (value != 0)
					{
						result.values[result.count++] = value;
					}

					cursor = end;
				}

				return result;
			}();

			return list;
		}
	}

	bool fx_ref_vp_listed()
	{
		return fx_ref_list().count != 0;
	}

	bool fx_ref_vp_matches(u64 hash)
	{
		const fx_ref_vp_list& list = fx_ref_list();

		// Empty list = match everything, which is the pre-round-18 behaviour of the census.
		if (list.count == 0)
		{
			return true;
		}

		const auto end = list.values.begin() + list.count;
		return std::find(list.values.begin(), end, hash) != end;
	}

	u32 fx_ref_max_lines()
	{
		static const u32 value = std::min<u32>(env_u32(L"RPCS3_REMIX_FXREFMAX", 8), 64);
		return value;
	}

	bool proj_split_enabled()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_PROJSPLIT", 0);
		return value != 0;
	}

	f32 proj_split_max_error()
	{
		// Scaled by 1000 so it can travel as an integer env var, exactly as AFFINETOL does:
		// 50 is 0.05.
		static const u32 milli = env_u32(L"RPCS3_REMIX_PROJSPLITERR", 50);
		return static_cast<f32>(milli) / 1000.f;
	}

	// --- round 19: the gate round 18 believed it had ---------------------------------------------
	//
	// REFUTATION, from the source rather than from a run. Round 18 shipped proj_split behind
	// is_affine(tol=0.02) and wrote "a wrong premise shows up as proj_split_refused rather than as
	// garbage in the scene". That gate cannot refuse. try_split_once builds view_to_world from an
	// ORTHONORMAL basis (right/up/forward via two cross products, RemixTransforms.cpp:6502-6524),
	// so vp_split::view is RIGID by construction, for the draw and for the anchor alike. The cross
	// is V_a * P_d and the recovered world is
	//
	//     fused * cross^-1 = (V_d * P_d) * P_d^-1 * V_a^-1 = V_d * V_a^-1
	//
	// a product of two rigid matrices, hence rigid, hence affine, ALWAYS. is_affine is therefore
	// vacuous on this construction and proj_split_refused is structurally pinned at 0 - which is
	// exactly what the two measured runs report (74552/0/0 and 170115/0/0). The equality with
	// tail_split_ok is not a mislabelled counter; it is the gate being unable to say no.
	//
	// What the construction actually assumes is V_d == V_a - the draw and the anchor share a view.
	// Nothing tested that, so this is the number that does: the L1 distance between the two rigid
	// views. Zero means the draw lands where the guest drew it; large means proj_split is placing
	// it somewhere else in the world with a bogus rigid offset, which looks identical on every
	// existing counter.
	//
	// Default 0 = NO GATE = round 18's behaviour bit for bit, so the first run measures the
	// distribution before anything is refused on it. Scaled by 1000 to travel as an integer.
	f32 proj_split_view_delta_max()
	{
		static const u32 milli = env_u32(L"RPCS3_REMIX_PROJSPLITVDELTA", 0);
		return static_cast<f32>(milli) / 1000.f;
	}

	// --- round 19: the viewmodel basis correction ------------------------------------------------
	//
	// The viewmodel renders (first time in nineteen rounds) but arrives mirrored, upside down and
	// displaced. "Backwards AND upside down" together is a two-axis sign error, not a translation
	// error, and this backend recovers its matrices from guest constants rather than choosing a
	// handedness - so the axis pair is not derivable from the source and must be selected by
	// measurement.
	//
	// The correction is applied in the CAMERA'S OWN FRAME, about the eye, which is the only frame
	// in which "backwards" and "upside down" are even defined. In world space that is
	//
	//     C = sum_k d_k * a_k a_k^T          (a_k = the camera's world-space right/up/forward)
	//     p' = cam + C * (p - cam)
	//
	// with d_k in {+1,-1}. Because the a_k come straight from the columns of the row-vector
	// worldToView they are orthonormal for any rigid view, so C is an exact reflection/rotation and
	// carries no scale - it cannot change the size of the viewmodel, only its orientation and its
	// position relative to the eye.
	//
	// Bitmask, so all eight combinations are reachable without a rebuild:
	//   bit0 (1) = negate the camera's RIGHT axis   -> mirrors left/right
	//   bit1 (2) = negate the camera's UP axis      -> flips upside down
	//   bit2 (4) = negate the camera's FORWARD axis -> puts it behind the eye
	//
	// 0 = OFF = today's behaviour bit for bit. 6 (up+forward, a 180 degree rotation about the
	// camera's right axis) is the round-19 launcher's explicit first guess, because it is the only
	// single operator that produces all three reported symptoms at once: upside down (up negated),
	// backwards/facing away (forward negated), and displaced UP while staying on the same side
	// (right preserved). If the report was instead "mirrored left-right", 3 is the pair to try.
	u32 viewmodel_basis_flip()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_VMBASIS", 0);
		return value & 7u;
	}

	u32 viewmodel_basis_census_max()
	{
		// ROUND 37: ceiling raised 96 -> 65536. The old ceiling was sized for the per-(vp, albedo)
		// dedup, where 96 lines is more distinct objects than the title has. VMBASISEVERY drops that
		// dedup to get a time series, and at ~1.5 KB a line 65536 is ~98 MB of dump - large, but the
		// dump is already ~950 MB and a session that needs the whole budget is one deliberately armed
		// for it. The launcher arms 4000. Round 31/32's lesson - a ceiling equal to the default, and
		// a value armed 277x over its ceiling - is why this is stated against the armed value here:
		// 4000 is inside 65536, so the knobs line must report 4000 back unchanged.
		static const u32 value = env_u32(L"RPCS3_REMIX_VMBASISMAX", 24);
		return std::min<u32>(value, 65536u);
	}

	// --- round 34: the pivot, and the decoupling of the tag from the placement --------------------
	//
	// Both doc blocks in RemixTransforms.h carry the measured derivation. The clamps here are stated
	// against the values the launcher arms, per the round-31/32 lesson that cost two rounds
	// (STATICINDEXBUDGET had ceiling == default; SUNSPRITEHOLD was armed 277x over its ceiling):
	// the launcher arms VMBASISPIVOT=1 against a ceiling of 2, and VMTAGONLY=1 against a boolean, so
	// both armed values are inside their clamps and the knobs= block will report them back unchanged.
	u32 viewmodel_basis_pivot()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_VMBASISPIVOT", 0);
		return std::min<u32>(value, 2u);
	}

	// --- round 36: the real rotation. Derivation and pre-registered readings in RemixTransforms.h -
	//
	// Clamps stated against the values the launcher arms, per the round-31/32 lesson that has now
	// cost four rounds: VMROTAXIS=1 against a ceiling of 6, VMROTDEG=180 against a modulus of 360,
	// VMROTPIVOT=0 against a ceiling of 3. All three armed values are inside their clamps, so the
	// knobs= block reports them back unchanged - which is the only way to see a typo here.
	//
	// env_u32 and not env_float on all three, deliberately: env_float rejects 0, and 0 is the OFF
	// value for two of them. That is the SKYANCHOR=0 defect (round 32) and it is not repeated here.
	// Note the modulus makes VMROTDEG=360 mean 0, i.e. OFF - write 180, never 360.
	u32 viewmodel_rotate_axis()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_VMROTAXIS", 0);
		return std::min<u32>(value, 6u);
	}

	u32 viewmodel_rotate_degrees()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_VMROTDEG", 0);
		return value % 360u;
	}

	u32 viewmodel_rotate_pivot()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_VMROTPIVOT", 0);
		return std::min<u32>(value, 3u);
	}

	// env_u32, not env_float: env_float rejects 0 outright (round 32 found SKYANCHOR=0 silently
	// yielding 4 through exactly that filter), and a boolean knob whose 0 does not mean off would be
	// the same defect a third time.
	bool viewmodel_tag_only()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_VMTAGONLY", 0);
		return value != 0;
	}

	// --- round 19: the depth rule is NOT dead on this title --------------------------------------
	//
	// REFUTATION, measured. RemixGSRender.cpp:5313 says "Haze reports the same scale_z/offset_z for
	// every draw in the scene including the sky dome, so the depth rule below selects nothing at
	// all and there is no threshold to retune". The round-18 run's own viewmodel census refutes it:
	// of 38 censused programs, 35 report scale_z=0.49875 offset_z=0.50125 and THREE report
	// scale_z=0.00125 offset_z=0.00125 - and those three are 830d7d1b9681c475 (the first-person
	// arms, vtx=3649), 57a12323f22f4988 and 9f591b6a6b825612 (the visor). A separate near depth
	// slice exists and it selects exactly the viewmodel.
	//
	// The built-in threshold is 1e-3 and the viewmodel's offset is 1.25e-3, so the rule misses by
	// 25%. This exposes it. 0 = use the built-in constant = today's behaviour bit for bit; the
	// launcher may raise it to 2 (=0.002) to let the depth rule select the three programs the
	// albedo list currently cannot separate from the sky dome and the effects family.
	//
	// Scaled by 1000, like every other tolerance knob here.
	f32 viewmodel_depth_offset_max()
	{
		static const u32 milli = env_u32(L"RPCS3_REMIX_VMDEPTHOFFSET", 0);
		return static_cast<f32>(milli) / 1000.f;
	}

	// --- round 35: the (vertex program, FRAGMENT program) hide gate ------------------------------
	//
	// Round 35's brief proposed the (vp, albedo) pair route for the player's legs. MEASURED over the
	// whole of bin\remix_dump.log (757 MB, every 'Remix fpcandidate:' row), that route OVER-MATCHES
	// and the fragment program does not:
	//
	//   vp=f39f504649b6f442 + albedo=0721D150DF278E7D  selects the LEGS (fp=b64dc06f79b8b42b,
	//     vtx=952) AND the ARMS/weapon (fp=d6f00cddfb5c6e0a, vtx=2140) - both appear on the user's
	//     own Ctrl+Click picks with exactly that vp and that albedo.
	//   vp=f39f504649b6f442 + fp=b64dc06f79b8b42b      is 1381 census rows, 1381 of them at vtx=952,
	//     across 12 albedos (the skin variants). Not one row is anything else.
	//
	// So the FP is the discriminator for the player body here. That does not contradict round 27,
	// which measured that the fp does NOT separate the weapon from the arms - both of those are
	// d6f00cddfb5c6e0a. It separates the BODY from the first-person rig, which is a different cut.
	//
	// Both halves default empty and an empty either half disarms the route, the same rule VMPAIR and
	// the triple use. mode selects what the match does:
	//
	//   1 = REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN. MEASURED in the deployed runtime's source
	//       (dxvk-remix-numos3 rtx_instance_manager.cpp, "if (currentInstance.m_isHidden)" ->
	//       "mask = 0"): the instance leaves the TLAS entirely, so it is gone from PRIMARY rays and
	//       from shadow rays alike, and rtx_accel_manager.cpp skips its BLAS. No shadow, no cost.
	//   2 = REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL. Sets OBJECT_MASK_PLAYER_MODEL
	//       (bit 6), which is NOT in OBJECT_MASK_ALL, so primary visibility needs
	//       rtx.playerModel.enableInPrimarySpace = True and shadow casting is then switched by
	//       rtx.playerModel.enablePrimaryShadows (default True -> set it False). That pair is the
	//       ONLY per-instance "visible but casts no shadow" route in this runtime - there is no
	//       castShadow flag anywhere in the tree. TWO WARNINGS: both conf lines are required (with
	//       neither, the legs become invisible AND still cast), and enableInPrimarySpace = True also
	//       masks every VIEW_MODEL candidate to zero inside createViewModelInstances, which would
	//       take the arms with it the moment a ViewModel camera becomes valid.
	//
	// Only an EXPLICIT 0 disables the flag. Unset is 1 (HIDDEN) and an out-of-range value clamps UP
	// to 2, never down to 0 - so the mode knob is not the safety here, the two hash halves are: the
	// mode is read only after hide_pair_matches() has already required both of them.
	u64 hide_pair_vp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_HIDEPAIRVP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();
		return value;
	}

	u64 hide_pair_fp_hash()
	{
		static const u64 value = []() -> u64
		{
			wchar_t buffer[32]{};
			const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_HIDEPAIRFP", buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return 0;
			}

			return ::_wcstoui64(buffer, nullptr, 16);
		}();
		return value;
	}

	// env_u32, not env_float: env_float rejects 0 outright, and 0 here has to mean "no flag, but
	// still counted" - see the cat_hide_pair increment in classify_draw.
	//
	// Default 1, ceiling 2, so the ceiling is deliberately ABOVE the default and every value in the
	// range is reachable - the round-31 trap was a ceiling that EQUALLED the default, which made the
	// knob turnable in one direction only. Note this knob alone cannot hide anything: it is read
	// only after hide_pair_matches() has already required BOTH hash halves to be set.
	u32 hide_pair_mode()
	{
		static const u32 value = std::min<u32>(env_u32(L"RPCS3_REMIX_HIDEPAIRMODE", 1), 2u);
		return value;
	}

	bool hide_pair_matches(u64 vp_hash, u64 fp_hash)
	{
		const u64 want_vp = hide_pair_vp_hash();
		const u64 want_fp = hide_pair_fp_hash();

		// An empty EITHER half disarms the route. This is the load-bearing line for "preserve
		// today's behaviour exactly when the new knob is empty", and it is also the one-line revert
		// the play-test card quotes: blank either half and nothing is hidden.
		if (want_vp == 0 || want_fp == 0 || vp_hash == 0 || fp_hash == 0)
		{
			return false;
		}

		return vp_hash == want_vp && fp_hash == want_fp;
	}

	// --- round 20: the (vp, albedo) pair gate for the viewmodel ----------------------------------
	//
	// MEASURED, from the round-19 build's own 'Remix vmbasis:' census (3 lines, one per program, cap
	// 24 never reached, so this is the complete list of programs that took the viewmodel branch):
	// BOTH pinned albedos are shared across the SAME three programs, and af06f6d32ec048ee is the sky
	// dome while f39f504649b6f442 is the effects family. A VIEW_MODEL-tagged instance has its
	// m_vkInstance.mask set to 0 and is removed from the world pass entirely
	// (rtx_instance_manager.cpp:1561, deployed numos3), so an albedo-only pin was deleting world
	// geometry. Narrowing the pin from two albedos to one changed nothing, because the over-match is
	// in the PROGRAM axis, not the albedo axis.
	//
	// Two independent lists, matched as an intersection, exactly like sun_card_vps() x
	// sun_card_albedos(). Both parsed once with the shared latched-comma-list shape.
	namespace
	{
		struct viewmodel_pair_vp_list
		{
			std::array<u64, 4> values{};
			u32 count = 0;
		};

		struct viewmodel_pair_albedo_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		// NOT named parse_hash_list: there is already a non-template
		// std::vector<u64> parse_hash_list(const wchar_t*) at :6216, and every 'namespace {' in this
		// file is unnamed at one-tab depth inside remix_rsx, so the two would share a scope. It
		// would compile (these calls give explicit template arguments), but two same-name,
		// same-signature parsers with different return types and different buffer sizes (4096 vs
		// 400 wchar_t) is a footgun for whoever edits this next.
		template <typename List>
		List parse_bounded_hash_list(const wchar_t* name)
		{
			List result{};
			wchar_t buffer[400]{};
			const DWORD written = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return result;
			}

			wchar_t* cursor = buffer;

			while (*cursor && result.count < result.values.size())
			{
				while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
				{
					++cursor;
				}

				wchar_t* end = nullptr;
				const u64 value = ::_wcstoui64(cursor, &end, 16);

				if (end == cursor)
				{
					break;
				}

				if (value != 0)
				{
					result.values[result.count++] = value;
				}

				cursor = end;
			}

			return result;
		}

		const viewmodel_pair_vp_list& viewmodel_pair_vps()
		{
			static const viewmodel_pair_vp_list list =
				parse_bounded_hash_list<viewmodel_pair_vp_list>(L"RPCS3_REMIX_VMPAIRVP");
			return list;
		}

		const viewmodel_pair_albedo_list& viewmodel_pair_albedos()
		{
			static const viewmodel_pair_albedo_list list =
				parse_bounded_hash_list<viewmodel_pair_albedo_list>(L"RPCS3_REMIX_VMPAIRALBEDO");
			return list;
		}

		// --- round 37: the extra UI-force pairs --------------------------------------------------
		// Paired POSITIONALLY: slot i of UIFORCEPAIRVP2 goes with slot i of UIFORCEPAIRFP2, and a
		// slot whose partner is missing is not a pair and is ignored. NOT a cross product of the two
		// lists, which is the failure mode round 20 hit when it keyed a route on a vp alone: the
		// helmet's own vp 830d7d1b9681c475 draws 31 distinct fragment programs, so crossing two
		// three-entry lists would force nine (vp, fp) combinations of which six are other geometry.
		struct ui_force_pair_extra_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		const ui_force_pair_extra_list& ui_force_pair_extra_vps()
		{
			static const ui_force_pair_extra_list list =
				parse_bounded_hash_list<ui_force_pair_extra_list>(L"RPCS3_REMIX_UIFORCEPAIRVP2");
			return list;
		}

		const ui_force_pair_extra_list& ui_force_pair_extra_fps()
		{
			static const ui_force_pair_extra_list list =
				parse_bounded_hash_list<ui_force_pair_extra_list>(L"RPCS3_REMIX_UIFORCEPAIRFP2");
			return list;
		}
	}

	// --- round 37: the helmet is drawn by THREE (vp, fp) pairs and the round-36 knob held ONE -----
	//
	// MEASURED against the round-36 play-test's own log, not inferred. `ui_forced_pair` reads 14664
	// and the cadence is EXACTLY one per frame over four consecutive 'Remix stats:' intervals
	// (101/101, 99/99, 105/105, 104/104 frames vs increments), and 138 'Remix uiwrap:' lines carry
	// vp=830d7d1b9681c475 fp=479890ff55f1d96e albedo=C61753D31FB96507 route=2d. So the round-36 route
	// matched, composited and was never refused - `ui_skipped` froze at 5077 at frame 9678 and did
	// not move for the following 13,800 frames. The mechanism works.
	//
	// What it does not do is cover the whole helmet. Albedo C61753D31FB96507 is submitted by three
	// distinct pairs and two of them went down the world path untouched, which is the clipping the
	// user has now reported five times:
	//
	//   830d7d1b9681c475 / 479890ff55f1d96e   138 lines   UI, forced          (round 36's pair)
	//   f39f504649b6f442 / 4afa02b3dbbe9b7e    33 lines   WORLD, submitted    ALL 33 the helmet
	//   830d7d1b9681c475 / 609a4216b89e296a     2 lines   WORLD, submitted
	//
	// The second pair is dedicated to the helmet - all 33 of its 'Remix fpcandidate:' lines carry
	// that albedo and no other - and reads dw=0, so the existing !depth_write_enabled() guard admits
	// it. It was never excluded on the merits; it was simply not in a key with one slot.
	//
	// This is the OR of the round-36 single pair and the positional list, so blanking
	// RPCS3_REMIX_UIFORCEPAIRVP2 restores round 36 exactly and blanking UIFORCEPAIRVP as well
	// restores round 35. Two independent reverts, which is what the play-test card needs.
	bool ui_force_pair_any_matches(u64 vp_hash, u64 fp_hash)
	{
		if (ui_force_pair_matches(vp_hash, fp_hash))
		{
			return true;
		}

		const ui_force_pair_extra_list& vps = ui_force_pair_extra_vps();
		const ui_force_pair_extra_list& fps = ui_force_pair_extra_fps();

		// An empty EITHER half disarms the extra route, and a short half bounds the pairing - so a
		// list of three vps against two fps forces two pairs, never three with a zero partner.
		const u32 pairs = std::min<u32>(vps.count, fps.count);

		if (pairs == 0 || vp_hash == 0 || fp_hash == 0)
		{
			return false;
		}

		for (u32 i = 0; i < pairs; ++i)
		{
			if (vp_hash == vps.values[i] && fp_hash == fps.values[i])
			{
				return true;
			}
		}

		return false;
	}

	// For the knobs line, so a list that failed to parse is visible as a count rather than as an
	// absence of behaviour. Reports the number of COMPLETE extra pairs, which is the number the
	// route can actually match - a mismatched pair of lists reads short here and nowhere else.
	u32 ui_force_pair_extra_count()
	{
		return std::min<u32>(ui_force_pair_extra_vps().count, ui_force_pair_extra_fps().count);
	}

	bool viewmodel_pair_matches(u64 vp_hash, u64 albedo_hash)
	{
		const viewmodel_pair_vp_list& vps = viewmodel_pair_vps();
		const viewmodel_pair_albedo_list& albedos = viewmodel_pair_albedos();

		// An empty EITHER half disarms the route. This is the load-bearing line for "preserve
		// today's behaviour exactly when the new knob is empty": with no pair configured this
		// returns false for every draw and the two call sites reduce to their round-19 form.
		if (vps.count == 0 || albedos.count == 0 || vp_hash == 0 || albedo_hash == 0)
		{
			return false;
		}

		const auto vp_end = vps.values.begin() + vps.count;

		if (std::find(vps.values.begin(), vp_end, vp_hash) == vp_end)
		{
			return false;
		}

		const auto albedo_end = albedos.values.begin() + albedos.count;
		return std::find(albedos.values.begin(), albedo_end, albedo_hash) != albedo_end;
	}

	u32 viewmodel_pair_vp_count()
	{
		return viewmodel_pair_vps().count;
	}

	u32 viewmodel_pair_albedo_count()
	{
		return viewmodel_pair_albedos().count;
	}

	// --- round 21: the alpha-state census ---------------------------------------------------------

	u32 alpha_state_census_max()
	{
		// Default 0 = OFF = no new output and no new work on the submit path. Clamped so a fat-fingered
		// launcher cannot turn a per-draw path into a log flood: the census is deduped per albedo and
		// this title shows 187 distinct albedos in a five-minute run, so 256 covers all of them.
		static const u32 value = std::min<u32>(env_u32(L"RPCS3_REMIX_ALPHACENSUS", 0), 256);
		return value;
	}

	// --- round 22: the WORLDIDENTITYVP override, measured -----------------------------------------

	u32 world_identity_census_max()
	{
		// Slots printed on 'Remix worldid-census:'. Default 0 = OFF = no new output. The backend
		// keeps 12 slots, so a fat-fingered launcher cannot ask for more than exist.
		static const u32 value = std::min<u32>(env_u32(L"RPCS3_REMIX_WORLDIDCENSUS", 0), 12);
		return value;
	}

	u32 world_identity_keep_translation()
	{
		// Whole world units. 0 = OFF, and OFF is the round-21 behaviour exactly: every draw of a
		// WORLDIDENTITYVP program is pinned to the world origin. Measured separation on this title is
		// wide enough that the threshold is not delicate - AD7CE9D672A0BF6B puts 652 draws at |t|<=1,
		// 127 in (1,32], and 3875 above 32, with nothing interesting in the gap.
		static const u32 value = env_u32(L"RPCS3_REMIX_WORLDIDMAXT", 0);
		return value;
	}

	namespace
	{
		// Same bounded latched-comma-list shape as world_identity_vp_matches() above, and the same
		// bound of 8 - the exemption cannot name more programs than WORLDIDENTITYVP itself can.
		struct world_identity_keep_exempt_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		const world_identity_keep_exempt_list& world_identity_keep_exempts()
		{
			static const world_identity_keep_exempt_list list =
				parse_bounded_hash_list<world_identity_keep_exempt_list>(L"RPCS3_REMIX_WORLDIDMAXTEXEMPTVP");
			return list;
		}
	}

	bool world_identity_keep_exempt_matches(u64 hash)
	{
		const world_identity_keep_exempt_list& list = world_identity_keep_exempts();
		const auto end = list.values.begin() + list.count;
		return hash != 0 && std::find(list.values.begin(), end, hash) != end;
	}

	u32 world_identity_keep_exempt_count()
	{
		return world_identity_keep_exempts().count;
	}

	u32 world_identity_keep_basis_milli()
	{
		// Thousandths of a unit of 3x3 deviation, same idiom as RPCS3_REMIX_AFFINETOL. 0 = OFF.
		// Separate from the translation test because 0214281B9A7A412D discards a basis_delta of 1.98
		// on 96 draws - a real rotation, not a rounding residue - while most of its translations are
		// small.
		static const u32 value = env_u32(L"RPCS3_REMIX_WORLDIDMAXB", 0);
		return value;
	}

	// --- round 12 --------------------------------------------------------------------------------

	bool diag_lines_enabled()
	{
		// Deliberately NOT dump_enabled(). That gate is what silenced all four of round 11's
		// acceptance instruments for a whole session; see the doc block in the header for the
		// measured partition. 0 restores that silence.
		static const u32 value = env_u32(L"RPCS3_REMIX_DIAGLINES", 1);
		return value != 0;
	}

	bool vcol_constant_route_enabled()
	{
		// Read once. Note what this knob does NOT do: it does not gate the classifier. scan_vcol_route
		// names the constant route unconditionally so the census can print the live c[K] with the
		// knob at 0; only the REPLAY is gated, in vcol_route_replayable() and again in
		// apply_vertex_colour's flat branch (which must re-test it, because vcol_route_replayable
		// short-circuits to 'true' when RPCS3_REMIX_FPVCOLROUTE=0).
		static const u32 value = env_u32(L"RPCS3_REMIX_VCOLCONST", 1);
		return value != 0;
	}

	// --- round 14: aim the backend's own distant sun at the game's own sun ------------------------
	//
	// The light this scene is lit by is NOT Remix's fallback light in the sense round 13's inbox
	// implied. The backend creates its OWN distant light through the API - ensure_sun_light(),
	// remixapi_LightInfoDistantEXT, light_info.hash = 0x4 - aimed by sun_direction() below, whose
	// built-in default {-0.35, -0.9, -0.25} is a generic mid-morning key light unrelated to where
	// Haze puts its sun. Everything here retargets THAT light; nothing here touches
	// rtx.fallbackLightDirection.
	//
	// MEASURED SIGN CONVENTION (numos3, verified this round): remixapi_LightInfoDistantEXT::direction
	// is the direction the light TRAVELS, sun -> scene. rtx_remix_api.cpp:714-721 hands it straight
	// to RtDistantLight::tryCreate; rtx_lights.cpp:808-825 stores it and maps +Z onto it; and
	// distant_light.slangh:90 then places the virtual light sample at
	//   position + (-direction) * 100000
	// i.e. the shadow ray travels along -direction. An overhead sun is therefore (0, -1, 0), which is
	// what the C++ wrapper's own default says too (remix.h:1016). So the vector to publish is
	// NEGATED from "camera -> sun card": get this backwards and the sun lands exactly 180 degrees
	// wrong, which is the single easiest thing to get backwards here.
	namespace
	{
		// Same latched-comma-list shape as sky_emissive_albedos(). Bound 4: a title has one sun, and
		// a list that can hold sixteen invites pasting a whole census into it.
		struct sun_card_albedo_list
		{
			std::array<u64, 4> values{};
			u32 count = 0;
		};

		const sun_card_albedo_list& sun_card_albedos()
		{
			static const sun_card_albedo_list list = []()
			{
				sun_card_albedo_list result{};
				wchar_t buffer[200]{};
				const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_SUNCARDALBEDO", buffer, static_cast<DWORD>(std::size(buffer)));

				if (written == 0 || written >= std::size(buffer))
				{
					return result;
				}

				wchar_t* cursor = buffer;

				while (*cursor && result.count < result.values.size())
				{
					while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
					{
						++cursor;
					}

					wchar_t* end = nullptr;
					const u64 value = ::_wcstoui64(cursor, &end, 16);

					if (end == cursor)
					{
						break;
					}

					if (value != 0)
					{
						result.values[result.count++] = value;
					}

					cursor = end;
				}

				return result;
			}();

			return list;
		}
	}

	bool sun_card_albedo_matches(u64 hash)
	{
		const sun_card_albedo_list& list = sun_card_albedos();
		const auto end = list.values.begin() + list.count;
		return hash != 0 && std::find(list.values.begin(), end, hash) != end;
	}

	u32 sun_card_albedo_count()
	{
		return sun_card_albedos().count;
	}

	namespace
	{
		// Round 16. Same latched-comma-list shape and the same bound as sun_card_albedos() above,
		// because it is the other half of one pin.
		struct sun_card_vp_list
		{
			std::array<u64, 4> values{};
			u32 count = 0;
		};

		const sun_card_vp_list& sun_card_vps()
		{
			static const sun_card_vp_list list = []()
			{
				sun_card_vp_list result{};
				wchar_t buffer[200]{};
				const DWORD written = GetEnvironmentVariableW(L"RPCS3_REMIX_SUNCARDVP", buffer, static_cast<DWORD>(std::size(buffer)));

				if (written == 0 || written >= std::size(buffer))
				{
					return result;
				}

				wchar_t* cursor = buffer;

				while (*cursor && result.count < result.values.size())
				{
					while (*cursor == L',' || *cursor == L';' || *cursor == L' ' || *cursor == L'\t')
					{
						++cursor;
					}

					wchar_t* end = nullptr;
					const u64 value = ::_wcstoui64(cursor, &end, 16);

					if (end == cursor)
					{
						break;
					}

					if (value != 0)
					{
						result.values[result.count++] = value;
					}

					cursor = end;
				}

				return result;
			}();

			return list;
		}
	}

	bool sun_card_vp_matches(u64 hash)
	{
		const sun_card_vp_list& list = sun_card_vps();
		const auto end = list.values.begin() + list.count;
		return hash != 0 && std::find(list.values.begin(), end, hash) != end;
	}

	u32 sun_card_vp_count()
	{
		return sun_card_vps().count;
	}

	u32 sun_track_mode()
	{
		// 0 (default) is today's behaviour BIT-EXACTLY: sun_direction()'s latched value is created
		// once and never touched again. 1 tracks only an albedo the user has PINNED in
		// SUNCARDALBEDO. 2 additionally lets the census's own best candidate drive the light, which
		// is the mode that can be wrong on its own and is therefore not the default.
		static const u32 value = env_u32(L"RPCS3_REMIX_SUNTRACK", 0);
		return value;
	}

	bool sun_card_census_enabled()
	{
		// Census only - it aims nothing. On by default because it costs one bounded log line per
		// (vp, albedo) per window and it is the ONLY way the sun card gets named: no artefact in
		// this repository identifies it. See the round-14 report.
		static const u32 value = env_u32(L"RPCS3_REMIX_SUNCARDCENSUS", 1);
		return value != 0;
	}

	u32 sun_card_max_vertices()
	{
		// A sun sprite is a quad, sometimes a small flare chain. 64 is deliberately loose - the
		// census exists to be read, and a bound that excludes the answer is worse than a few extra
		// lines. Election (SUNTRACK=2) applies the same bound.
		static const u32 value = env_u32(L"RPCS3_REMIX_SUNCARDMAXVTX", 64);
		return value;
	}

	namespace
	{
		// env_float rejects every value <= 0 and silently returns the fallback (:5907-5908), which is
		// right for a radiance but WRONG for the two knobs below: "0 = retarget on any movement" and
		// "negative = no elevation gate" are both real, useful bisect values, and with env_float they
		// would have been silent no-ops that did not do what their documentation says. That is the
		// exact defect class round 13's review caught twice, so it is fixed here rather than papered
		// over in the comment. wcstod with an end pointer is what separates a genuine "0" from an
		// unparseable string; _wtof cannot.
		f32 env_float_signed(const wchar_t* name, f32 fallback)
		{
			wchar_t buffer[64]{};
			const DWORD written = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return fallback;
			}

			wchar_t* end = nullptr;
			const f32 parsed = static_cast<f32>(::wcstod(buffer, &end));

			if (end == buffer || !std::isfinite(parsed))
			{
				return fallback;
			}

			return parsed;
		}
	}

	// --- round 37: the pivot offset, the basis lock and the census time series --------------------
	//
	// Defined HERE and not beside viewmodel_rotate_axis() at the top of this file for one mechanical
	// reason: env_float_signed lives in the anonymous namespace that closes immediately above, so a
	// definition placed with its siblings would not see it and would silently fall back to env_float,
	// which rejects 0 - the exact defect (round 32's SKYANCHOR=0) that env_float_signed exists to
	// prevent. Declarations for all five are with the round-36 block in RemixTransforms.h, which is
	// where the derivations and the pre-registered readings are written.
	//
	// No clamp on the three offsets. They are signed world units of order one, a wrong value is
	// visible in the first second of a play-test, and every clamp this project has added to a knob of
	// this shape has since cost a round to unpick.
	f32 viewmodel_rotate_pivot_right()
	{
		static const f32 value = env_float_signed(L"RPCS3_REMIX_VMROTPIVOTRIGHT", 0.f);
		return value;
	}

	f32 viewmodel_rotate_pivot_up()
	{
		static const f32 value = env_float_signed(L"RPCS3_REMIX_VMROTPIVOTUP", 0.f);
		return value;
	}

	f32 viewmodel_rotate_pivot_fwd()
	{
		static const f32 value = env_float_signed(L"RPCS3_REMIX_VMROTPIVOTFWD", 0.f);
		return value;
	}

	// env_u32, not env_float_signed: 0 is OFF and the value is an enum, not a measurement.
	// Clamp stated against the armed value, per the round-31/32 lesson: the launcher arms 0 (off)
	// against a ceiling of 2, so the knobs line must report 0 back unchanged.
	u32 viewmodel_rotate_lock()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_VMROTLOCK", 0);
		return std::min<u32>(value, 2u);
	}

	// env_u32: 0 is OFF (keep the round-19..36 per-(vp, albedo) dedup) and must survive the reader.
	// No ceiling - VMBASISMAX is the bound that matters and it has one. The launcher arms 1.
	u32 viewmodel_basis_census_every()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_VMBASISEVERY", 0);
		return value;
	}

	// 0 = any vertex count. No ceiling: a vertex count is an identity, not a magnitude.
	u32 viewmodel_basis_census_vtx()
	{
		static const u32 value = env_u32(L"RPCS3_REMIX_VMBASISVTX", 0);
		return value;
	}

	f32 sun_card_min_elevation()
	{
		// Degrees above the horizon. A sun below the horizon is not the sun, and without this term
		// every blended ground decal in front of the camera is a candidate. Negative disables the
		// test; the census prints the elevation either way so the gate can be judged from the log.
		static const f32 value = env_float_signed(L"RPCS3_REMIX_SUNCARDMINELEV", 2.f);
		return value;
	}

	f32 sun_card_min_distance()
	{
		// Round 16, and deliberately NOT part of sun_shape: this floor is spent on the census line
		// budget alone, so a badly chosen value can waste lines but can never hide a pin or change
		// suncard_seen. 0 (the default) is round 14's behaviour exactly.
		static const f32 value = env_float_signed(L"RPCS3_REMIX_SUNCARDMINDIST", 0.f);
		return value;
	}

	f32 viewmodel_pair_max_distance()
	{
		// Round 23. An eye-distance CEILING on the VMPAIRVP+VMPAIRALBEDO route only - it does not
		// touch the hash route, the albedo route or the depth route, and 0 (the default) reproduces
		// round 22 exactly. It exists so a first-person albedo that is SHARED with distant character
		// meshes can be admitted safely: distance is the discriminator the pair lists do not have.
		//
		// MEASURED, round-22 run, from 'Remix fpcandidate:' eye_dist on the shared character program
		// 830d7d1b9681c475 (listedvp=1, so its rows are NOT truncated by FPCENSUSMAXDIST):
		//   albedo 86885A0E60751491 (vtx=3649, the first-person body)  0.465 .. 0.624   n=6
		//   albedo 0721D150DF278E7D (vtx=2140/952, the weapon)         0.429 .. 1.090   n=6
		//   albedo EC3C2D7AC0AB2938 (vtx=554)                         20.708 .. 38.764  n=8
		//   albedo 19177730D341388A (vtx=92)                          20.871 .. 37.257  n=7
		// A 20x gap. Anything in 2..20 separates the first-person half from everything else that
		// program drew in that sample; 2 is the value the launcher documents.
		//
		// It cannot live inside viewmodel_pair_matches(): that is a pure free function with no
		// camera or geometry access. It is applied at the two tagging sites, which already measure
		// the geometry centre for viewmodel_anchor_rejects().
		static const f32 value = env_float_signed(L"RPCS3_REMIX_VMPAIRMAXDIST", 0.f);
		return value;
	}

	// Defined here rather than beside fp_census_vp_count(), which is where it belongs by subject:
	// env_float_signed is declared in the anonymous namespace above this line and nothing earlier
	// in this file can call it.
	f32 fp_census_max_distance()
	{
		// Round 17. 0 (the default) is round 13's census bit-exactly.
		//
		// Why it exists: every one of the 242 'Remix fpcandidate:' lines in the log carries
		// vp=830d7d1b9681c475, because that is the only hash on FPCENSUSVP and the census returns
		// at the list test before it measures anything. The first-person rig is not one program -
		// the 1446-vertex head-locked mesh is drawn by 830d7d1b9681c475, af06f6d32ec048ee AND
		// f39f504649b6f442, and f39f504649b6f442 additionally draws a 952-vertex mesh whose
		// instance origin sits 1.8194 to 1.8256 BELOW the eye in three different runs at three
		// different world positions (from viewmodel-census, which is not program-gated). The census
		// that is supposed to find the arms and the weapon has never measured a single draw of it.
		//
		// Non-zero admits an UNLISTED program too, provided its geometry centre is measured and
		// within this many world units of the eye, and re-arms the census once per stats window so
		// the same (vp, albedo) reports repeatedly. That second half matters as much as the first:
		// round 13's census samples a pair ONCE for the life of the process, so "stays near the eye
		// all run" was never answerable from it, and round 16 had to settle the visor question from
		// the suncard census instead - using ranges, not snapshots.
		//
		// Census only. It tags nothing, refuses nothing and moves no counter.
		static const f32 value = env_float_signed(L"RPCS3_REMIX_FPCENSUSMAXDIST", 0.f);
		return value;
	}

	f32 sun_track_hysteresis()
	{
		// Degrees of angular movement required before the light is destroyed and re-created. The
		// Remix C API has no "update light" - CreateLight/DestroyLight/DrawLightInstance is the whole
		// surface (remix_c.h:728-740) - so retargeting costs a destroy+create, exactly as the camera
		// fill light already pays every frame. This makes that cost proportional to real movement
		// instead of to frame rate. 0 re-creates whenever the direction changes at all.
		static const f32 value = env_float_signed(L"RPCS3_REMIX_SUNTRACKDEG", 1.f);
		return value;
	}

	bool sun_card_emissive_enabled()
	{
		// ITEM 2's mechanism, and deliberately the SAME one round 13 shipped for the sky dome rather
		// than a second implementation: a listed albedo gets an emissive material with the albedo as
		// its per-texel emissive texture AND BlendType::kEmissive, which sets m_isUnordered and moves
		// the instance to OBJECT_MASK_UNORDERED_ALL_EMISSIVE. For a glare card that is the whole fix:
		// opacity -> 0, so the card's black/dark texels stop being drawn and its hard edges stop
		// occluding, while the bright core still emits. Off by default because it does nothing at all
		// until SUNCARDALBEDO names a hash.
		static const u32 value = env_u32(L"RPCS3_REMIX_SUNCARDEMISSIVE", 0);
		return value != 0;
	}

	f32 sun_card_emissive_intensity()
	{
		// Its own knob rather than sharing SKYEMISSIVEINT: the dome and the sun card want different
		// numbers and tying them together would make each untunable. 2.0 matches the dome's shipped
		// value so the first judgement is shape, not brightness.
		static const f32 value = env_float(L"RPCS3_REMIX_SUNCARDINT", 2.f);
		return value;
	}

	// --- round 27 ---------------------------------------------------------------------------------

	namespace
	{
		// Bound 8 on each list: six programs are known and the bound leaves room for a sibling found
		// by a later pick without letting a fat-fingered launcher turn the hot path into a long scan.
		struct particle_billboard_vp_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		struct particle_ribbon_vp_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		const particle_billboard_vp_list& particle_billboard_vps()
		{
			static const particle_billboard_vp_list list =
				parse_bounded_hash_list<particle_billboard_vp_list>(L"RPCS3_REMIX_PARTICLEBILLBOARDVP");
			return list;
		}

		const particle_ribbon_vp_list& particle_ribbon_vps()
		{
			static const particle_ribbon_vp_list list =
				parse_bounded_hash_list<particle_ribbon_vp_list>(L"RPCS3_REMIX_PARTICLERIBBONVP");
			return list;
		}
	}

	bool particle_billboard_vp_matches(u64 hash)
	{
		const particle_billboard_vp_list& list = particle_billboard_vps();
		const auto end = list.values.begin() + list.count;
		return hash != 0 && std::find(list.values.begin(), end, hash) != end;
	}

	u32 particle_billboard_vp_count()
	{
		return particle_billboard_vps().count;
	}

	bool particle_ribbon_vp_matches(u64 hash)
	{
		const particle_ribbon_vp_list& list = particle_ribbon_vps();
		const auto end = list.values.begin() + list.count;
		return hash != 0 && std::find(list.values.begin(), end, hash) != end;
	}

	u32 particle_ribbon_vp_count()
	{
		return particle_ribbon_vps().count;
	}

	u32 particle_census_max()
	{
		// Clamped to 64: the census is deduped per (program, outcome) and there are six programs and
		// five outcomes, so 64 covers every distinct pair with room to spare.
		static const u32 value = std::min<u32>(env_u32(L"RPCS3_REMIX_PARTICLECENSUS", 0), 64);
		return value;
	}

	bool particle_uv_enabled()
	{
		// Default ON, unlike the two program lists, because with the lists empty this reaches nothing.
		// It exists so that "the particles are in the right place but look wrong" can be separated
		// from "the particles are in the wrong place" without a rebuild.
		static const u32 value = env_u32(L"RPCS3_REMIX_PARTICLEUV", 1);
		return value != 0;
	}

	namespace
	{
		struct particle_flipbook_vp_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		const particle_flipbook_vp_list& particle_flipbook_vps()
		{
			static const particle_flipbook_vp_list list =
				parse_bounded_hash_list<particle_flipbook_vp_list>(L"RPCS3_REMIX_PARTICLEFLIPBOOKVP");
			return list;
		}
	}

	bool particle_flipbook_vp_matches(u64 hash)
	{
		const particle_flipbook_vp_list& list = particle_flipbook_vps();
		const auto end = list.values.begin() + list.count;
		return hash != 0 && std::find(list.values.begin(), end, hash) != end;
	}

	u32 particle_flipbook_vp_count()
	{
		return particle_flipbook_vps().count;
	}

	bool particle_flipbook_phase_per_quad()
	{
		// Default ON. The header states the argument; the short form is that v9.z is a per-particle
		// age read out of an ordinary vertex stream, and reading it once per DRAW was a round-27
		// assumption ("a per-effect constant fed identically to every vertex") that the batching
		// measurements contradict. 0 restores that assumption for a one-line A/B.
		static const u32 value = env_u32(L"RPCS3_REMIX_PARTICLEFLIPPHASE", 1);
		return value != 0;
	}

	namespace
	{
		struct sun_sprite_albedo_list
		{
			std::array<u64, 4> values{};
			u32 count = 0;
		};

		const sun_sprite_albedo_list& sun_sprite_albedos()
		{
			static const sun_sprite_albedo_list list =
				parse_bounded_hash_list<sun_sprite_albedo_list>(L"RPCS3_REMIX_SUNSPRITE");
			return list;
		}
	}

	bool sun_sprite_albedo_matches(u64 hash)
	{
		const sun_sprite_albedo_list& list = sun_sprite_albedos();
		const auto end = list.values.begin() + list.count;
		return hash != 0 && std::find(list.values.begin(), end, hash) != end;
	}

	u32 sun_sprite_albedo_count()
	{
		return sun_sprite_albedos().count;
	}

	f32 sun_sprite_max_span()
	{
		// MEASURED: the sprite's texture is 64x64 and its 'Remix uiwrap:' rows read subrect=1/1, i.e.
		// one untiled quad. 0.5 NDC is a quarter of the screen on each axis - far larger than a sun
		// sprite ever is, and small enough that a full-screen draw sharing the albedo is rejected.
		static const f32 value = env_float(L"RPCS3_REMIX_SUNSPRITEMAXSPAN", 0.5f);
		return value;
	}

	u32 sun_sprite_census_max()
	{
		static const u32 value = std::min<u32>(env_u32(L"RPCS3_REMIX_SUNSPRITECENSUS", 0), 64);
		return value;
	}

	u32 sun_sprite_hold_frames()
	{
		// --- ROUND 32: the ceiling was BELOW the value the launcher was setting -------------------
		// The old clamp was std::min(env, 3600) and launch-haze-remix.cmd sets 999999, so the
		// effective hold was 3600 frames. MEASURED in the round-31 run's own 'Remix live:' line:
		// the knobs block reads `sunspritehold=3600` while the launcher says 999999 - the instrument
		// was already reporting the clamp and nobody read it. 3600 frames is ~90 s at the measured
		// 38-47 fps, and the user's report is "changed back to old position a few MINUTES into
		// playing the mission". The hold did not fail; it expired, on schedule, at the ceiling.
		//
		// The old comment's reasoning was sound but the arithmetic was not: it argued 3600 is "far
		// short of a level's lifetime", and a Haze mission is minutes long, so 3600 frames is well
		// INSIDE a level. Raised to 216000 (~1 hour at 60 fps, ~95 min at the measured rate), which
		// is longer than any single Haze level and still finite, so the per-level property the old
		// comment wanted is preserved in substance: a hold cannot outlive a session.
		//
		// This is the round-31 clamp trap a second time (STATICINDEXBUDGET had ceiling == default).
		// Grep any new knob's clamp against BOTH its default and the value the launcher arms.
		static const u32 value = std::min<u32>(env_u32(L"RPCS3_REMIX_SUNSPRITEHOLD", 0), 216000);
		return value;
	}

	namespace
	{
		struct viewmodel_triple_vp_list
		{
			std::array<u64, 4> values{};
			u32 count = 0;
		};

		struct viewmodel_triple_fp_list
		{
			std::array<u64, 4> values{};
			u32 count = 0;
		};

		struct viewmodel_triple_albedo_list
		{
			std::array<u64, 8> values{};
			u32 count = 0;
		};

		const viewmodel_triple_vp_list& viewmodel_triple_vps()
		{
			static const viewmodel_triple_vp_list list =
				parse_bounded_hash_list<viewmodel_triple_vp_list>(L"RPCS3_REMIX_VMTRIPLEVP");
			return list;
		}

		const viewmodel_triple_fp_list& viewmodel_triple_fps()
		{
			static const viewmodel_triple_fp_list list =
				parse_bounded_hash_list<viewmodel_triple_fp_list>(L"RPCS3_REMIX_VMTRIPLEFP");
			return list;
		}

		const viewmodel_triple_albedo_list& viewmodel_triple_albedos()
		{
			static const viewmodel_triple_albedo_list list =
				parse_bounded_hash_list<viewmodel_triple_albedo_list>(L"RPCS3_REMIX_VMTRIPLEALBEDO");
			return list;
		}
	}

	bool viewmodel_triple_matches(u64 vp_hash, u64 fp_hash, u64 albedo_hash)
	{
		const viewmodel_triple_vp_list& vps = viewmodel_triple_vps();
		const viewmodel_triple_fp_list& fps = viewmodel_triple_fps();
		const viewmodel_triple_albedo_list& albedos = viewmodel_triple_albedos();

		// Any empty list disarms the route, exactly as viewmodel_pair_matches() does. This is the line
		// that makes the shipped default byte-for-byte identical to round 26.
		if (vps.count == 0 || fps.count == 0 || albedos.count == 0
			|| vp_hash == 0 || fp_hash == 0 || albedo_hash == 0)
		{
			return false;
		}

		const auto vp_end = vps.values.begin() + vps.count;

		if (std::find(vps.values.begin(), vp_end, vp_hash) == vp_end)
		{
			return false;
		}

		const auto fp_end = fps.values.begin() + fps.count;

		if (std::find(fps.values.begin(), fp_end, fp_hash) == fp_end)
		{
			return false;
		}

		const auto albedo_end = albedos.values.begin() + albedos.count;
		return std::find(albedos.values.begin(), albedo_end, albedo_hash) != albedo_end;
	}

	u32 viewmodel_triple_vp_count()
	{
		return viewmodel_triple_vps().count;
	}

	u32 viewmodel_triple_fp_count()
	{
		return viewmodel_triple_fps().count;
	}

	u32 viewmodel_triple_albedo_count()
	{
		return viewmodel_triple_albedos().count;
	}
}

#endif
