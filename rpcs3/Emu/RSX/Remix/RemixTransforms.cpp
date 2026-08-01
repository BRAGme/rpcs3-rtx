#include "stdafx.h"
#include "RemixTransforms.h"

#ifdef _WIN32

#include "Emu/RSX/Program/RSXVertexProgram.h"
#include "Emu/RSX/rsx_methods.h"

#include <algorithm>
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

		bool vec_writes_temp(const decoded_instr& in, u32 tmp)
		{
			return in.d1.vec_opcode != RSX_VEC_OPCODE_NOP
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

		struct chain_result
		{
			bool found = false;
			chain_shape shape = chain_shape::none;
			u32 base = 0;
			chain_source source{};
			u32 instructions = 0;
			// Earliest instruction of the matched chain: the bound for the next search back.
			u32 first_instruction = 0;
		};

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
		bool match_dp4_chain(const program_walker& prog, const std::vector<u32>& writers, chain_result& out)
		{
			if (writers.size() != 4)
			{
				return false;
			}

			u32 consts[4] = { umax, umax, umax, umax };
			chain_source source{};
			bool have_source = false;

			for (const u32 i : writers)
			{
				const decoded_instr& in = prog[i];

				if (in.d1.vec_opcode != RSX_VEC_OPCODE_DP4 && in.d1.vec_opcode != RSX_VEC_OPCODE_DPH)
				{
					return false;
				}

				if (in.d3.index_const)
				{
					return false;
				}

				u32 component = 0;
				if (!single_component(vec_writemask(in), component) || consts[component] != umax)
				{
					return false;
				}

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
			out.first_instruction = writers.front();
			return true;
		}

		// MUL + MAD/ADD accumulation through a temp: c[base+k] scaled by pos_k.
		bool match_mad_chain(const program_walker& prog, const std::vector<u32>& writers, chain_result& out)
		{
			if (writers.empty())
			{
				return false;
			}

			// The instruction that produces the final value writes every component.
			u32 cursor = umax;
			for (auto it = writers.rbegin(); it != writers.rend(); ++it)
			{
				if (vec_writemask(prog[*it]) == 0xf)
				{
					cursor = *it;
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

			for (u32 step = 0; step < 4; ++step)
			{
				const decoded_instr& in = prog[cursor];
				const u32 opcode = in.d1.vec_opcode;

				if (in.d3.index_const)
				{
					return false;
				}

				const bool terminal = (step == 3);

				if (terminal)
				{
					if (opcode != RSX_VEC_OPCODE_MUL && opcode != RSX_VEC_OPCODE_MOV)
					{
						return false;
					}
				}
				else if (opcode != RSX_VEC_OPCODE_MAD && opcode != RSX_VEC_OPCODE_ADD)
				{
					return false;
				}

				// Find the constant operand and, for the scaled forms, the position operand.
				u32 const_slot = umax;
				u32 scale_slot = umax;
				const u32 mask = vec_source_mask(opcode);

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

				// Follow the accumulator into the previous step.
				if (in.src[2].reg_type != RSX_VP_REGISTER_TYPE_TEMP)
				{
					return false;
				}

				const u32 prev = prog.last_temp_writer(in.src[2].tmp_src, cursor);
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

			// The four constants must be one consecutive group, with c[base+k] scaling
			// position component k.
			u32 base = chain_consts[0];
			for (u32 step = 1; step < 4; ++step)
			{
				if (chain_consts[step] == umax)
				{
					return false;
				}

				base = std::min(base, chain_consts[step]);
			}

			bool seen[4] = { false, false, false, false };

			for (u32 step = 0; step < 4; ++step)
			{
				const u32 offset = chain_consts[step] - base;

				if (offset > 3 || seen[offset] || chain_components[step] != offset)
				{
					return false;
				}

				seen[offset] = true;
			}

			out.found = true;
			out.shape = chain_shape::mad;
			out.base = base;
			out.source = source;
			out.instructions = 4;
			out.first_instruction = cursor;
			return true;
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

		chain_result find_chain(const program_walker& prog, const walk_target& target, u32 before, u32 depth = 0)
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

			if (match_dp4_chain(prog, writers, result))
			{
				return result;
			}

			result = chain_result{};

			if (match_mad_chain(prog, writers, result))
			{
				return result;
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
					return find_chain(prog, walk_target{ false, in.src[0].tmp_src }, last, depth + 1);
				}
			}

			return chain_result{};
		}

		// Bounded backward slice from HPOS, only to count constants and spot indexed addressing.
		void slice_position(const program_walker& prog, u32& out_distinct_consts, u32& out_instructions, bool& out_indexed, std::vector<u32>* out_indices = nullptr)
		{
			std::vector<walk_target> pending;
			std::vector<walk_target> seen_targets;
			std::vector<u8> seen_instr(prog.size(), 0);
			std::vector<u32> consts;

			pending.push_back(walk_target{ true, 0 });

			u32 instructions = 0;
			bool indexed = false;

			while (!pending.empty())
			{
				const walk_target target = pending.back();
				pending.pop_back();

				bool already = false;
				for (const auto& t : seen_targets)
				{
					if (t == target)
					{
						already = true;
						break;
					}
				}

				if (already)
				{
					continue;
				}

				seen_targets.push_back(target);

				for (u32 i = 0; i < static_cast<u32>(prog.size()); ++i)
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
							pending.push_back(walk_target{ false, in.src[s].tmp_src });
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

		slice_position(prog, result.distinct_consts, result.chain_instructions, result.indexed_const);

		if (result.chain_instructions == 0)
		{
			result.note = "no HPOS write";
			return result;
		}

		if (result.indexed_const)
		{
			result.note = "indexed constants";
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

		while (count < max_transform_groups)
		{
			const chain_result chain = find_chain(prog, target, before);

			if (!chain.found || (chain.base + 4) > s_legal_constant_slots)
			{
				break;
			}

			chains[count++] = chain;

			const chain_source source = resolve_source(prog, chain.source, chain.first_instruction);

			if (source.reg_type == RSX_VP_REGISTER_TYPE_INPUT)
			{
				reached_input = true;
				break;
			}

			if (source.reg_type != RSX_VP_REGISTER_TYPE_TEMP)
			{
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
				reached_input = true;
				break;
			}

			target = walk_target{ false, source.index };
			before = chain.first_instruction;
		}

		if (count == 0)
		{
			// A 4x4 transform needs four distinct constant slots by construction (one constant
			// per instruction, digest 1.3). Fewer than that and no chain means the program cannot
			// be transforming a position at all - it is a 2D / pre-projected program.
			if (result.distinct_consts < 4)
			{
				result.archetype = vp_archetype::screen_space;
				result.note = "no matrix chain, <4 constants";
			}
			else
			{
				result.note = "no matrix chain into HPOS";
			}

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
		result.archetype = (count == 1) ? vp_archetype::fused : vp_archetype::layered;
		result.note = reached_input ? "chain reaches the vertex attribute" : "innermost operand is not an attribute";
		return result;
	}

	const char* archetype_name(vp_archetype a)
	{
		switch (a)
		{
		case vp_archetype::screen_space: return "screen_space";
		case vp_archetype::fused: return "fused";
		case vp_archetype::layered: return "layered";
		default: return "unknown";
		}
	}

	std::string describe_position_slice(const RSXVertexProgram& vp, u32 max_instructions)
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
		slice_position(prog, consts, instructions, indexed, &indices);

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

			fmt::append(out, "(%s%u.%c%c%c%c,%s%u.%c%c%c%c,%s%u.%c%c%c%c)",
				source_kind(in.src[0]), u32{in.src[0].tmp_src},
				comps[in.src[0].swz_x], comps[in.src[0].swz_y], comps[in.src[0].swz_z], comps[in.src[0].swz_w],
				source_kind(in.src[1]), u32{in.src[1].tmp_src},
				comps[in.src[1].swz_x], comps[in.src[1].swz_y], comps[in.src[1].swz_z], comps[in.src[1].swz_w],
				source_kind(in.src[2]), u32{in.src[2].tmp_src},
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

	bool build_prescale(const vp_fingerprint& fp, mat4& out)
	{
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
		static const bool value = env_flag(L"RPCS3_REMIX_DUMP");
		return value;
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

	f32 debug_light_radius()
	{
		static const f32 value = env_float(L"RPCS3_REMIX_LIGHTRADIUS", 0.1f);
		return value;
	}

	f32 debug_light_radiance()
	{
		static const f32 value = env_float(L"RPCS3_REMIX_LIGHTRADIANCE", 100.f);
		return value;
	}
}

#endif
