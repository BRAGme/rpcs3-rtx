#include "stdafx.h"
#include "RemixTextures.h"

#ifdef _WIN32

#include "Emu/Memory/vm.h"
#include "Emu/RSX/Common/TextureUtils.h"
#include "Emu/RSX/Common/io_buffer.h"
#include "Emu/RSX/RSXTexture.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/gcm_enums.h"
#include "Emu/RSX/Remix/RemixRuntime.h"
#include "util/fnv_hash.hpp"

#include "3rdparty/bcdec/bcdec.hpp"

#include <algorithm>
#include <cstdio>

namespace remix_rsx
{
	namespace
	{
		// Frames a texture may go unreferenced before its handles are released. Deliberately
		// the same shape as the mesh LRU in RemixGSRender so both age out together.
		constexpr u64 s_texture_idle_frames = 300;

		// Upper bound on a single decoded mip 0. 4096x4096 BGRA8 = 64 MiB.
		constexpr usz s_max_decoded_bytes = 64ull * 1024 * 1024;

		// Bytes read for the per-draw staleness fingerprint.
		constexpr u32 s_fingerprint_budget = 4096;

		u32 read_env_u32(const wchar_t* name, u32 fallback)
		{
			wchar_t buffer[64]{};
			const DWORD written = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));

			if (written == 0 || written >= std::size(buffer))
			{
				return fallback;
			}

			const long parsed = ::wcstol(buffer, nullptr, 10);
			return (parsed >= 0) ? static_cast<u32>(parsed) : fallback;
		}

		// RSX wrap modes that tile map onto Remix's Repeat (1); everything else clamps (0).
		u8 to_remix_wrap(rsx::texture_wrap_mode mode)
		{
			switch (mode)
			{
			case rsx::texture_wrap_mode::wrap:
			case rsx::texture_wrap_mode::mirror:
			case rsx::texture_wrap_mode::mirror_once_clamp_to_edge:
			case rsx::texture_wrap_mode::mirror_once_border:
			case rsx::texture_wrap_mode::mirror_once_clamp:
				return 1;
			default:
				return 0;
			}
		}

		// FNV-1a over a byte range, the same shape as buffered_section::fast_hash_internal
		// (Emu\RSX\Common\texture_cache.cpp:157-197). Copied rather than widening its access.
		u64 fnv_bytes(const u8* data, usz length, u64 seed)
		{
			u64 hash = seed;
			usz i = 0;

			for (; (i + sizeof(u64)) <= length; i += sizeof(u64))
			{
				u64 word;
				std::memcpy(&word, data + i, sizeof(u64));
				hash = rpcs3::hash64(hash, word);
			}

			for (; i < length; ++i)
			{
				hash = rpcs3::hash64(hash, u64{data[i]});
			}

			return hash;
		}

		// Cheap strided sample used to notice in-place updates without rehashing everything.
		u64 sample_fingerprint(const u8* data, u32 length)
		{
			if (!data || length == 0)
			{
				return 0;
			}

			const u32 step = std::max<u32>(8, (length / s_fingerprint_budget) & ~7u);

			u64 hash = rpcs3::fnv_seed;
			hash = rpcs3::hash64(hash, u64{length});

			for (u32 i = 0; (i + sizeof(u64)) <= length; i += step)
			{
				u64 word;
				std::memcpy(&word, data + i, sizeof(u64));
				hash = rpcs3::hash64(hash, word);
			}

			return hash;
		}

		// 16-bit -> BGRA8 expanders. The source word is already native-endian by the time
		// upload_texture_subresource is done with it.
		using expand_fn = u32 (*)(u16);

		u32 expand_r5g6b5(u16 v)
		{
			const u32 r = ((v >> 11) & 0x1F) * 255 / 31;
			const u32 g = ((v >> 5) & 0x3F) * 255 / 63;
			const u32 b = (v & 0x1F) * 255 / 31;
			return b | (g << 8) | (r << 16) | 0xFF000000u;
		}

		u32 expand_a1r5g5b5(u16 v)
		{
			const u32 a = (v & 0x8000) ? 0xFFu : 0u;
			const u32 r = ((v >> 10) & 0x1F) * 255 / 31;
			const u32 g = ((v >> 5) & 0x1F) * 255 / 31;
			const u32 b = (v & 0x1F) * 255 / 31;
			return b | (g << 8) | (r << 16) | (a << 24);
		}

		u32 expand_d1r5g5b5(u16 v)
		{
			return expand_a1r5g5b5(v) | 0xFF000000u;
		}

		u32 expand_a4r4g4b4(u16 v)
		{
			const u32 a = ((v >> 12) & 0xF) * 17;
			const u32 r = ((v >> 8) & 0xF) * 17;
			const u32 g = ((v >> 4) & 0xF) * 17;
			const u32 b = (v & 0xF) * 17;
			return b | (g << 8) | (r << 16) | (a << 24);
		}

		u32 expand_r5g5b5a1(u16 v)
		{
			const u32 r = ((v >> 11) & 0x1F) * 255 / 31;
			const u32 g = ((v >> 6) & 0x1F) * 255 / 31;
			const u32 b = ((v >> 1) & 0x1F) * 255 / 31;
			const u32 a = (v & 1) ? 0xFFu : 0u;
			return b | (g << 8) | (r << 16) | (a << 24);
		}

		expand_fn expander_for(u32 gcm_format)
		{
			switch (gcm_format)
			{
			case CELL_GCM_TEXTURE_R5G6B5: return &expand_r5g6b5;
			case CELL_GCM_TEXTURE_A1R5G5B5: return &expand_a1r5g5b5;
			case CELL_GCM_TEXTURE_D1R5G5B5: return &expand_d1r5g5b5;
			case CELL_GCM_TEXTURE_A4R4G4B4: return &expand_a4r4g4b4;
			case CELL_GCM_TEXTURE_R5G5B5A1: return &expand_r5g5b5a1;
			default: return nullptr;
			}
		}

		// upload_texture_subresource writes a BGRA8 image directly for these.
		bool is_direct_bgra8(u32 gcm_format)
		{
			switch (gcm_format)
			{
			case CELL_GCM_TEXTURE_A8R8G8B8:
			case CELL_GCM_TEXTURE_D8R8G8B8:
			case CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
			case CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8:
				return true;
			default:
				return false;
			}
		}

		bool is_bc_format(u32 gcm_format)
		{
			return gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT1 ||
				gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT23 ||
				gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT45;
		}
	}

	bool textures_disabled()
	{
		static const bool value = read_env_u32(L"RPCS3_REMIX_NOTEX", 0) != 0;
		return value;
	}

	u32 texture_budget()
	{
		static const u32 value = std::max<u32>(1, read_env_u32(L"RPCS3_REMIX_TEXBUDGET", 8));
		return value;
	}

	bool textures_linear()
	{
		static const bool value = read_env_u32(L"RPCS3_REMIX_TEXLINEAR", 0) != 0;
		return value;
	}

	u32 texture_rehash_mode()
	{
		// Default 0 (descriptor only), measured rather than assumed: on Minecraft NPUB31419
		// mode 1 turned 682 CreateTexture calls into 604 rehashes over 9,120 frames, and
		// because the albedo hash is folded into the mesh key that dragged mesh churn from
		// 5,881 creates / 887 live (mode 0) to 188,427 creates / 31,793 live (mode 1).
		// Animated textures go stale instead; RPCS3_REMIX_TEXREHASH=1 or 2 buys them back.
		static const u32 value = std::min<u32>(2, read_env_u32(L"RPCS3_REMIX_TEXREHASH", 0));
		return value;
	}

	u64 texture_descriptor::key() const
	{
		u64 hash = rpcs3::fnv_seed;
		hash = rpcs3::hash64(hash, u64{offset});
		hash = rpcs3::hash64(hash, u64{location} | (u64{format} << 8) | (u64{pitch} << 16));
		hash = rpcs3::hash64(hash, u64{width} | (u64{height} << 16) | (u64{depth} << 32) | (u64{mipmaps} << 48));
		hash = rpcs3::hash64(hash, u64{border} | (u64{wrap_s} << 8) | (u64{wrap_t} << 16) | (u64{cubemap} << 24) | (u64{dimension} << 32));
		return hash ? hash : 1;
	}

	void texture_cache::begin_frame()
	{
		m_budget_left = texture_budget();
	}

	remixapi_MaterialHandle texture_cache::bind(const remixapi_Interface& api,
		const rsx::fragment_texture& tex,
		u64 frame,
		const texture_entry** out_entry)
	{
		if (out_entry)
		{
			*out_entry = nullptr;
		}

		if (textures_disabled())
		{
			return nullptr;
		}

		texture_descriptor desc{};
		desc.offset = tex.offset();
		desc.location = tex.location();
		desc.format = tex.format();
		desc.pitch = tex.pitch();
		desc.width = tex.width();
		desc.height = tex.height();
		desc.depth = tex.depth();
		desc.mipmaps = static_cast<u8>(std::min<u32>(0xFF, tex.get_exact_mipmap_count()));
		desc.border = tex.border_type();
		desc.wrap_s = static_cast<u8>(tex.wrap_s());
		desc.wrap_t = static_cast<u8>(tex.wrap_t());
		desc.cubemap = tex.cubemap() ? 1 : 0;
		desc.dimension = static_cast<u8>(tex.dimension());

		if (desc.width == 0 || desc.height == 0)
		{
			++m_stats.unsupported;
			return nullptr;
		}

		const u64 key = desc.key();
		auto it = m_entries.find(key);

		if (it != m_entries.end())
		{
			texture_entry& entry = it->second;
			entry.last_used_frame = frame;

			if (entry.unsupported)
			{
				++m_stats.unsupported;
				return nullptr;
			}

			bool stale = false;

			if (texture_rehash_mode() > 0)
			{
				// The descriptor is unchanged, so the guest range is too: sample it directly
				// instead of walking the subresource layout again.
				const u32 address = rsx::get_address(desc.offset, desc.location);
				const u32 length = static_cast<u32>(std::min<usz>(rsx::get_texture_size(tex), 0x4000000));

				if (length && vm::check_addr(address, vm::page_readable, length))
				{
					const u64 fingerprint = (texture_rehash_mode() >= 2)
						? fnv_bytes(vm::_ptr<const u8>(address), length, rpcs3::fnv_seed)
						: sample_fingerprint(vm::_ptr<const u8>(address), length);

					stale = (fingerprint != entry.fingerprint);
				}
			}

			if (!stale)
			{
				++m_stats.hits;

				if (out_entry)
				{
					*out_entry = &entry;
				}

				return entry.material;
			}

			// Content changed under a stable descriptor. Drop the old pair and fall through
			// to a rebuild; the budget applies to that rebuild like any other miss.
			if (entry.material)
			{
				guarded_destroy_material(api.DestroyMaterial, entry.material);
			}

			if (entry.texture)
			{
				guarded_destroy_texture(api.DestroyTexture, entry.texture);
			}

			++m_stats.destroyed;
			++m_stats.rehashed;
			m_entries.erase(it);
		}

		if (m_budget_left == 0)
		{
			++m_stats.deferred;
			return nullptr;
		}

		texture_entry entry{};
		entry.last_used_frame = frame;
		entry.wrap_u = to_remix_wrap(tex.wrap_s());
		entry.wrap_v = to_remix_wrap(tex.wrap_t());

		if (!decode(tex, entry))
		{
			// Remember the failure so the same descriptor is not retried every draw.
			entry.unsupported = true;
			m_entries.emplace(key, std::move(entry));
			return nullptr;
		}

		--m_budget_left;

		if (!upload(api, entry))
		{
			entry.unsupported = true;
			entry.pixels.clear();
			entry.pixels.shrink_to_fit();
			m_entries.emplace(key, std::move(entry));
			return nullptr;
		}

		auto inserted = m_entries.emplace(key, std::move(entry));

		if (out_entry)
		{
			*out_entry = &inserted.first->second;
		}

		return inserted.first->second.material;
	}

	bool texture_cache::decode(const rsx::fragment_texture& tex, texture_entry& out)
	{
		const u32 raw_format = tex.format();
		const u32 gcm_format = raw_format & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
		const bool is_swizzled = !(raw_format & CELL_GCM_TEXTURE_LN);

		const u32 address = rsx::get_address(tex.offset(), tex.location());
		const usz total_size = rsx::get_texture_size(tex);

		if (total_size == 0 || total_size > 0x4000000)
		{
			++m_stats.unsupported;
			return false;
		}

		if (!vm::check_addr(address, vm::page_readable, static_cast<u32>(total_size)))
		{
			// Render-target-sourced or unmapped: guest RAM is not authoritative here.
			++m_stats.unreadable;
			return false;
		}

		const bool direct = is_direct_bgra8(gcm_format);
		const expand_fn expand = expander_for(gcm_format);
		const bool is_bc = is_bc_format(gcm_format);
		const bool is_b8 = (gcm_format == CELL_GCM_TEXTURE_B8);

		if (!direct && !expand && !is_bc && !is_b8)
		{
			++m_stats.unsupported;
			return false;
		}

		const std::vector<rsx::subresource_layout> layouts = rsx::get_subresources_layout(tex);

		const rsx::subresource_layout* mip0 = nullptr;

		for (const auto& layout : layouts)
		{
			if (layout.level == 0 && layout.layer == 0)
			{
				mip0 = &layout;
				break;
			}
		}

		if (!mip0 || mip0->data.empty())
		{
			++m_stats.unreadable;
			return false;
		}

		const u32 width = mip0->width_in_texel;
		const u32 height = mip0->height_in_texel;

		if (width == 0 || height == 0 || (usz{width} * height * 4) > s_max_decoded_bytes)
		{
			++m_stats.unsupported;
			return false;
		}

		// The modder-facing identity: the raw guest bytes of mip 0, exactly as the title
		// wrote them, mixed with the interpretation that makes them an image.
		u64 hash = fnv_bytes(mip0->data.data<u8>(), mip0->data.size(), rpcs3::fnv_seed);
		hash = rpcs3::hash64(hash, u64{gcm_format});
		hash = rpcs3::hash64(hash, u64{width} | (u64{height} << 32));

		out.content_hash = hash ? hash : 1;
		out.fingerprint = (texture_rehash_mode() >= 2)
			? fnv_bytes(vm::_ptr<const u8>(address), static_cast<u32>(total_size), rpcs3::fnv_seed)
			: sample_fingerprint(vm::_ptr<const u8>(address), static_cast<u32>(total_size));
		out.width = width;
		out.height = height;
		out.pixels.assign(usz{width} * height * 4, 0);

		if (is_bc)
		{
			const u32 block_bytes = (gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT1) ? 8u : 16u;
			const u32 blocks_x = mip0->width_in_block;
			const u32 blocks_y = mip0->height_in_block;
			const u32 src_pitch = mip0->pitch_in_block;

			const u8* src = mip0->data.data<u8>();
			const usz src_size = mip0->data.size();

			for (u32 by = 0; by < blocks_y; ++by)
			{
				for (u32 bx = 0; bx < blocks_x; ++bx)
				{
					const usz src_offset = (usz{by} * src_pitch + bx) * block_bytes;

					if ((src_offset + block_bytes) > src_size)
					{
						continue;
					}

					// bcdec writes 0xAABBGGRR words, i.e. RGBA byte order.
					u8 block[4 * 4 * 4];

					switch (gcm_format)
					{
					case CELL_GCM_TEXTURE_COMPRESSED_DXT1: bcdec_bc1(src + src_offset, block, 16); break;
					case CELL_GCM_TEXTURE_COMPRESSED_DXT23: bcdec_bc2(src + src_offset, block, 16); break;
					default: bcdec_bc3(src + src_offset, block, 16); break;
					}

					for (u32 y = 0; y < 4; ++y)
					{
						const u32 dst_y = (by * 4) + y;

						if (dst_y >= height)
						{
							break;
						}

						for (u32 x = 0; x < 4; ++x)
						{
							const u32 dst_x = (bx * 4) + x;

							if (dst_x >= width)
							{
								break;
							}

							const u8* texel = block + (y * 16) + (x * 4);
							u8* dst = out.pixels.data() + ((usz{dst_y} * width + dst_x) * 4);

							dst[0] = texel[2]; // B
							dst[1] = texel[1]; // G
							dst[2] = texel[0]; // R
							dst[3] = texel[3]; // A
						}
					}
				}
			}

			return true;
		}

		// Non-BC: let the shared decoder do the swizzle/byteswap work. alignment = 1 makes
		// every destination row tightly packed regardless of word size, which is what the
		// BGRA8 expansion below and the compositor both assume.
		rsx::texture_uploader_capabilities caps
		{
			.supports_byteswap = false,
			.supports_vtc_decoding = false,
			.supports_hw_deswizzle = false,
			.supports_zero_copy = false,
			.supports_dxt = false,
			.alignment = 1
		};

		if (direct)
		{
			rsx::io_buffer dst{ out.pixels.data(), out.pixels.size() };
			rsx::upload_texture_subresource(dst, *mip0, static_cast<int>(gcm_format), is_swizzled, caps);
			return true;
		}

		if (is_b8)
		{
			m_scratch.assign(usz{width} * height, 0);
			rsx::io_buffer dst{ m_scratch.data(), m_scratch.size() };
			rsx::upload_texture_subresource(dst, *mip0, static_cast<int>(gcm_format), is_swizzled, caps);

			for (usz i = 0, count = usz{width} * height; i < count; ++i)
			{
				const u8 v = m_scratch[i];
				u8* pixel = out.pixels.data() + (i * 4);
				pixel[0] = v;
				pixel[1] = v;
				pixel[2] = v;
				pixel[3] = 0xFF;
			}

			return true;
		}

		// 16-bit packed colour.
		m_scratch.assign(usz{width} * height * 2, 0);
		rsx::io_buffer dst{ m_scratch.data(), m_scratch.size() };
		rsx::upload_texture_subresource(dst, *mip0, static_cast<int>(gcm_format), is_swizzled, caps);

		for (usz i = 0, count = usz{width} * height; i < count; ++i)
		{
			u16 word;
			std::memcpy(&word, m_scratch.data() + (i * 2), sizeof(u16));

			const u32 bgra = expand(word);
			std::memcpy(out.pixels.data() + (i * 4), &bgra, sizeof(u32));
		}

		return true;
	}

	bool texture_cache::upload(const remixapi_Interface& api, texture_entry& entry)
	{
		const remixapi_Format format = textures_linear()
			? REMIXAPI_FORMAT_B8G8R8A8_UNORM
			: REMIXAPI_FORMAT_B8G8R8A8_SRGB;

		remixapi_TextureInfo info{};
		info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
		info.pNext = nullptr;
		info.hash = entry.content_hash;
		info.width = entry.width;
		info.height = entry.height;
		info.depth = 1;
		info.mipLevels = 1;
		info.format = format;
		info.data = entry.pixels.data();
		info.dataSize = entry.pixels.size();

		const u32 tex_status = guarded_create_texture(api.CreateTexture, &info, &entry.texture);

		if (tex_status != REMIXAPI_ERROR_CODE_SUCCESS || !entry.texture)
		{
			rsx_log.error("Remix: CreateTexture failed for %ux%u hash %016llx (%s)",
				entry.width, entry.height, entry.content_hash, error_name(tex_status));
			entry.texture = nullptr;
			return false;
		}

		++m_stats.created;

		// The fork resolves this pseudo-path against the texture manager's hash table, which
		// remixapi_CreateTexture just populated with entry.content_hash
		// (rtx_fork_api_entry.cpp textureHashPathLookup). That is what puts an API texture in
		// the same hash namespace as a native D3D9 one.
		wchar_t albedo_path[32]{};
		::swprintf_s(albedo_path, L"0x%016llX", entry.content_hash);

		remixapi_MaterialInfoOpaqueEXT opaque{};
		opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
		opaque.pNext = nullptr;
		opaque.roughnessTexture = nullptr;
		opaque.metallicTexture = nullptr;
		opaque.anisotropy = 0.f;
		opaque.albedoConstant = { 1.f, 1.f, 1.f };
		opaque.opacityConstant = 1.f;
		opaque.roughnessConstant = 0.7f;
		opaque.metallicConstant = 0.f;
		opaque.thinFilmThickness_hasvalue = 0;
		opaque.thinFilmThickness_value = 0.f;
		opaque.alphaIsThinFilmThickness = 0;
		opaque.heightTexture = nullptr;
		opaque.displaceIn = 0.f;
		opaque.useDrawCallAlphaState = 1;
		opaque.blendType_hasvalue = 0;
		opaque.blendType_value = 0;
		opaque.invertedBlend = 0;
		opaque.alphaTestType = 7;
		opaque.alphaReferenceValue = 0;
		opaque.displaceOut = 0.f;

		remixapi_MaterialInfo material{};
		material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
		material.pNext = &opaque;
		material.hash = entry.content_hash;
		material.albedoTexture = albedo_path;
		material.normalTexture = nullptr;
		material.tangentTexture = nullptr;
		material.emissiveTexture = nullptr;
		material.emissiveIntensity = 0.f;
		material.emissiveColorConstant = { 0.f, 0.f, 0.f };
		material.spriteSheetRow = 1;
		material.spriteSheetCol = 1;
		material.spriteSheetFps = 0;
		material.filterMode = 1; // Linear
		material.wrapModeU = entry.wrap_u;
		material.wrapModeV = entry.wrap_v;

		const u32 mat_status = guarded_create_material(api.CreateMaterial, &material, &entry.material);

		if (mat_status != REMIXAPI_ERROR_CODE_SUCCESS || !entry.material)
		{
			rsx_log.error("Remix: CreateMaterial failed for hash %016llx (%s)",
				entry.content_hash, error_name(mat_status));
			guarded_destroy_texture(api.DestroyTexture, entry.texture);
			entry.texture = nullptr;
			entry.material = nullptr;
			++m_stats.destroyed;
			return false;
		}

		++m_stats.materials;
		return true;
	}

	void texture_cache::reap(const remixapi_Interface& api, u64 frame)
	{
		if (m_entries.empty() || frame < s_texture_idle_frames)
		{
			return;
		}

		const u64 cutoff = frame - s_texture_idle_frames;

		for (auto it = m_entries.begin(); it != m_entries.end();)
		{
			if (it->second.last_used_frame > cutoff)
			{
				++it;
				continue;
			}

			if (it->second.material)
			{
				guarded_destroy_material(api.DestroyMaterial, it->second.material);
			}

			if (it->second.texture)
			{
				guarded_destroy_texture(api.DestroyTexture, it->second.texture);
				++m_stats.destroyed;
			}

			it = m_entries.erase(it);
		}
	}

	void texture_cache::destroy_all(const remixapi_Interface& api)
	{
		for (auto& [key, entry] : m_entries)
		{
			if (entry.material)
			{
				guarded_destroy_material(api.DestroyMaterial, entry.material);
			}

			if (entry.texture)
			{
				guarded_destroy_texture(api.DestroyTexture, entry.texture);
			}
		}

		m_entries.clear();
	}
}

#endif
