#pragma once

#include "RemixSR2CharacterPlans.h"
#include "util/types.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace remix_rsx
{
	using sr2_colour = std::array<f32, 4>;
	struct sr2_character_instruction
	{
		std::array<u32, 4> words{};
		sr2_colour literal{};
		u8 mask = 0;
	};
	struct sr2_character_program
	{
		const sr2_character_generated::plan* plan = nullptr;
		std::vector<sr2_character_instruction> instructions;
		u64 key = 0;
		f32 alpha_tint = 1.f;
	};
	using sr2_uv = std::array<f32, 2>;
	using sr2_uv_map = std::array<f32, 6>;
	inline bool fit_sr2_uv_map(std::span<const sr2_uv> base, std::span<const sr2_uv> other, sr2_uv_map& map)
	{
		if (base.size() != other.size() || base.size() < 3) return false;
		if (std::equal(base.begin(), base.end(), other.begin()) &&
			std::all_of(base.begin(), base.end(), [](const sr2_uv& uv) { return std::isfinite(uv[0]) && std::isfinite(uv[1]); }))
		{
			map = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
			return true;
		}
		usz pivot = 0;
		f64 radius = 0;
		for (usz i = 1; i < base.size(); ++i)
		{
			const f64 x = f64{base[i][0]} - base[0][0], y = f64{base[i][1]} - base[0][1];
			const f64 distance = x * x + y * y;
			if (!std::isfinite(distance)) return false;
			if (distance > radius) { radius = distance; pivot = i; }
		}
		bool found = false;
		for (usz i = 1; i < base.size() && !found; ++i)
		{
			const f64 a = f64{base[pivot][0]} - base[0][0], b = f64{base[pivot][1]} - base[0][1];
			const f64 c = f64{base[i][0]} - base[0][0], d = f64{base[i][1]} - base[0][1];
			const f64 det = a * d - b * c;
			if (!std::isfinite(det) || std::abs(det) < 1e-10) continue;
			for (u32 lane = 0; lane < 2; ++lane)
			{
				const f64 e = f64{other[pivot][lane]} - other[0][lane], f = f64{other[i][lane]} - other[0][lane];
				map[lane * 3] = static_cast<f32>((e * d - b * f) / det);
				map[lane * 3 + 1] = static_cast<f32>((a * f - e * c) / det);
				map[lane * 3 + 2] = other[0][lane] - map[lane * 3] * base[0][0] - map[lane * 3 + 1] * base[0][1];
			}
			found = true;
		}
		if (!found) return false;
		// Canonicalize fit noise; the bake is mip-0 material reconstruction, not pixel parity.
		for (f32& value : map)
		{
			if (!std::isfinite(value)) return false;
			value = static_cast<f32>(std::round(f64{value} * 1e7) / 1e7);
		}
		for (usz i = 0; i < base.size(); ++i)
		{
			for (u32 lane = 0; lane < 2; ++lane)
			{
				const f32 value = map[lane * 3] * base[i][0] + map[lane * 3 + 1] * base[i][1] + map[lane * 3 + 2];
				if (!std::isfinite(value) || !std::isfinite(other[i][lane]) || std::abs(value - other[i][lane]) > 2e-5f) return false;
			}
		}
		return true;
	}
	inline bool sr2_uv_bake_addressing(std::span<const sr2_uv> base, const sr2_uv_map& map,
		u8 base_u, u8 base_v, u8 other_u, u8 other_v)
	{
		bool outside[2]{};
		for (const auto& uv : base)
		{
			for (u32 lane = 0; lane < 2; ++lane)
			{
				if (!std::isfinite(uv[lane])) return false;
				outside[lane] |= uv[lane] < 0.f || uv[lane] > 1.f;
			}
		}
		if (!outside[0] && !outside[1]) return true;
		if (map == sr2_uv_map{1.f, 0.f, 0.f, 0.f, 1.f, 0.f} && base_u == other_u && base_v == other_v) return true;
		const u8 base_wrap[] = {base_u, base_v}, other_wrap[] = {other_u, other_v};
		for (u32 axis = 0; axis < 2; ++axis)
		{
			if (!outside[axis]) continue;
			if (base_wrap[axis] != 1) return false;
			for (u32 lane = 0; lane < 2; ++lane)
			{
				const f32 coefficient = map[lane * 3 + axis];
				if (!std::isfinite(coefficient)) return false;
				if (coefficient != 0.f && (other_wrap[lane] != 1 || coefficient != std::round(coefficient))) return false;
			}
		}
		return true;
	}

	inline u32 sr2_fp_word(const u8* data)
	{
		u32 word;
		std::memcpy(&word, data, sizeof(word));
		return ((word & 0x00FF00FFu) << 8) | ((word & 0xFF00FF00u) >> 8);
	}
	inline u64 sr2_character_mix(u64 key, const void* data, usz size)
	{
		const auto* bytes = static_cast<const u8*>(data);
		for (usz i = 0; i < size; ++i)
		{
			key = (key ^ bytes[i]) * 0x100000001b3ull;
		}
		return key;
	}

	inline bool build_sr2_character_program(u64 hash, const void* raw, usz size,
		sr2_character_program& result)
	{
		result = {};
		const auto* data = static_cast<const u8*>(raw);
		if (!data) return false;
		for (const auto& plan : sr2_character_generated::plans)
		{
			if (plan.hash != hash || plan.rgb_premultiplied) continue;
			result.plan = &plan;
			result.key = sr2_character_mix(0xcbf29ce484222325ull, &hash, sizeof(hash));
			if (plan.alpha_tint_offset != 0xffff)
			{
				if (usz{plan.alpha_tint_offset} + 4 > size) return false;
				result.alpha_tint = std::bit_cast<f32>(sr2_fp_word(data + plan.alpha_tint_offset));
				if (!std::isfinite(result.alpha_tint)) return false;
			}
			if (plan.tint_max_guard_offset != 0xffff)
			{
				if (usz{plan.tint_max_guard_offset} + 16 > size) return false;
				const f32 x = std::bit_cast<f32>(sr2_fp_word(data + plan.tint_max_guard_offset));
				const f32 w = std::bit_cast<f32>(sr2_fp_word(data + plan.tint_max_guard_offset + 12));
				if (!std::isfinite(x) || !std::isfinite(w) || w != result.alpha_tint || x > w || w <= 0.f) return false;
			}
			for (const auto& step : std::span(plan.steps, plan.count))
			{
				const usz offset = usz{step.slot} * 16;
				if (offset + 16 > size) return false;
				sr2_character_instruction instruction{};
				instruction.mask = step.mask;
				for (u32 i = 0; i < 4; ++i)
				{
					instruction.words[i] = sr2_fp_word(data + offset + i * 4);
					if (instruction.words[i] != step.words[i]) return false;
				}
				result.key = sr2_character_mix(result.key, &step.slot, sizeof(step.slot));
				result.key = sr2_character_mix(result.key, &step.mask, sizeof(step.mask));
				const u32 opcode = (step.words[0] >> 24) & 0x3f;
				const u32 sources = opcode == 0x17 ? 0 : (opcode == 1 || opcode == 0x1a ? 1 : (opcode == 4 || opcode == 0x1f ? 7 : 3));
				u8 literal_mask = 0;
				for (u32 s = 0; s < 3; ++s)
				{
					if (!(sources & (1u << s)) || (step.words[s + 1] & 3) != 2) continue;
					for (u32 lane = 0; lane < 4; ++lane)
					{
						if (!(step.mask & (1u << lane))) continue;
						const u32 source_lane = (opcode == 0x3a && s == 1) || opcode == 0x1a ? 0 : lane;
						literal_mask |= 1u << ((step.words[s + 1] >> (9 + source_lane * 2)) & 3);
					}
				}
				if (literal_mask)
				{
					if (offset + 32 > size) return false;
					for (u32 i = 0; i < 4; ++i)
					{
						if (!(literal_mask & (1u << i))) continue;
						instruction.literal[i] = std::bit_cast<f32>(sr2_fp_word(data + offset + 16 + i * 4));
						if (!std::isfinite(instruction.literal[i])) return false;
						result.key = sr2_character_mix(result.key, &instruction.literal[i], sizeof(f32));
					}
				}
				result.instructions.push_back(instruction);
			}
			return true;
		}
		return false;
	}

	// A pre-lighting material bake, not a replacement for the guest's fragment lighting.
	// H temporaries are range-clamped; native-half rounding and post-bake filtering differ.
	inline bool evaluate_sr2_character(const sr2_character_program& program,
		const std::array<sr2_colour, 16>& texels, sr2_colour& output)
	{
		if (!program.plan) return false;
		std::array<sr2_colour, 128> registers{};
		for (auto& reg : registers) reg.fill(std::numeric_limits<f32>::quiet_NaN());
		for (const auto& instruction : program.instructions)
		{
			const auto& words = instruction.words;
			const u32 opcode = (words[0] >> 24) & 0x3f;
			const u32 target = ((words[0] >> 1) & 63) + ((words[0] & 0x80) ? 64 : 0);
			sr2_colour value{};
			if (opcode == 0x17)
			{
				value = texels[(words[0] >> 17) & 15];
			}
			else
			{
				sr2_colour source[3]{};
				for (u32 s = 0; s < 3; ++s)
				{
					const u32 word = words[s + 1];
					const u32 type = word & 3;
					const auto& base = type == 2 ? instruction.literal
						: registers[((word >> 2) & 63) + ((word & 0x100) ? 64 : 0)];
					for (u32 lane = 0; lane < 4; ++lane)
					{
						f32 v = base[(word >> (9 + lane * 2)) & 3];
						if (word & (s == 0 ? (1u << 29) : (1u << 18))) v = std::abs(v);
						if (word & (1u << 17)) v = -v;
						source[s][lane] = v;
					}
				}
				for (u32 lane = 0; lane < 4; ++lane)
				{
					if (!(instruction.mask & (1u << lane))) continue;
					const f32 a = source[0][lane], b = source[1][lane], c = source[2][lane];
					switch (opcode)
					{
					case 1: value[lane] = a; break;
					case 2: value[lane] = a * b; break;
					case 3: value[lane] = a + b; break;
					case 4: value[lane] = a * b + c; break;
					case 8: value[lane] = std::min(a, b); break;
					case 9: value[lane] = std::max(a, b); break;
					case 0xa: value[lane] = a < b ? 1.f : 0.f; break;
					case 0xb: value[lane] = a >= b ? 1.f : 0.f; break;
					case 0x1a: value[lane] = 1.f / source[0][0]; break;
					case 0x1f: value[lane] = a * b + (1.f - a) * c; break;
					case 0x3a: value[lane] = a / source[1][0]; break;
					default: return false;
					}
				}
			}
			const u32 scale = (words[2] >> 28) & 7;
			for (u32 lane = 0; lane < 4; ++lane)
			{
				if (!(instruction.mask & (1u << lane))) continue;
				f32 v = value[lane];
				if (scale == 1) v *= 2.f;
				else if (scale != 0) return false;
				if (words[0] & 0x80000000u) v = std::isnan(v) ? 0.f : std::clamp(v, 0.f, 1.f);
				else if ((words[0] & 0x80) || ((words[0] >> 22) & 3) == 1) v = std::clamp(v, -65504.f, 65504.f);
				if (!std::isfinite(v)) return false;
				registers[target][lane] = v;
			}
		}
		const auto& plan = *program.plan;
		for (u32 lane = 0; lane < 3; ++lane)
		{
			output[lane] = registers[plan.rgb_register + (plan.rgb_half ? 64 : 0)][lane] + (plan.rgb_bias_one ? 1.f : 0.f);
		}
		output[3] = plan.alpha_valid ? registers[plan.alpha_register + (plan.alpha_half ? 64 : 0)][plan.alpha_lane] : 1.f;
		return std::all_of(output.begin(), output.end(), [](f32 v) { return std::isfinite(v); });
	}
}
