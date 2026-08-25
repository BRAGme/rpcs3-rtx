#pragma once

#ifdef _WIN32

#include "util/types.hpp"

#include "Emu/RSX/Remix/remix_c.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rsx
{
	class fragment_texture;
}

namespace remix_rsx
{
	// Everything the RSX texture registers say about one bound unit. Two draws with an
	// identical descriptor read the same guest bytes with the same interpretation, so this is
	// the cache key; the content hash below is what a modder actually sees.
	struct texture_descriptor
	{
		u32 offset = 0;
		u32 location = 0;
		u32 format = 0;
		u32 pitch = 0;
		u16 width = 0;
		u16 height = 0;
		u16 depth = 0;
		u8 mipmaps = 0;
		u8 border = 0;
		u8 wrap_s = 0;
		u8 wrap_t = 0;
		u8 cubemap = 0;
		u8 dimension = 0;

		// The draw's alpha test, as a VkCompareOp (RSX comparison_function - 0x200) plus the
		// 0-255 reference. Part of the key because the material carries the alpha state and the
		// material is what this cache hands out: one texture used by an opaque wall draw and by
		// an alpha-cutout foliage card needs two materials, not one. 7 == ALWAYS == no test.
		u8 alpha_func = 7;
		u8 alpha_ref = 0;

		bool operator==(const texture_descriptor&) const = default;
		u64 key() const;
	};

	// One decoded + uploaded texture and the material that names it.
	struct texture_entry
	{
		remixapi_TextureHandle texture = nullptr;
		remixapi_MaterialHandle material = nullptr;

		// ROUND 39. The hash build_material() declared for `material`. Usually equal to
		// content_hash; it differs for a non-default wrap/alpha variant and, since round 39, for a
		// hash the sky classifier promoted - the promoted definition MUST get its own identity or
		// it aliases the non-emissive one it is replacing and the winner is draw-order dependent.
		// Recorded so a promotion can PROVE the two differ instead of assuming it, and so the
		// promoted dome's mat_<HASH> is discoverable for a modder.
		u64 material_hash = 0;

		// FNV-1a over the raw guest mip-0 bytes, mixed with format and dimensions. This is
		// the modder-facing identity: same content => same value across runs.
		u64 content_hash = 0;

		// Cheap strided sample of the same bytes, re-taken every draw so a texture updated
		// in place (animated water/fire) is noticed without rehashing megabytes.
		u64 fingerprint = 0;

		// Full hash of the guest range as of the last CPU-pixel refresh (bind's
		// 'refresh_pixels' path). Separate from 'fingerprint' so the two staleness policies
		// never overwrite each other, and separate from 'content_hash' because that one is
		// folded into the mesh key and must not move when only the CPU copy is rebuilt.
		u64 content_refresh = 0;

		// Round 8, measurement only. Full hash of the guest range as of the last cadence verify
		// (RPCS3_REMIX_TEXVERIFY), and the frame that verify ran. A THIRD hash rather than a reuse
		// of either of the two above, deliberately: 'fingerprint' is the rehash policy's (and is a
		// strided sample at the default mode), 'content_refresh' belongs to the CPU-pixel refresh
		// and only advances for callers that pass refresh_pixels - sharing either would make one
		// policy silently mask the other's staleness.
		u64 verify_hash = 0;
		u64 last_verified_frame = 0;

		u32 width = 0;
		u32 height = 0;

		// Remix wrap-mode enumerants derived from the RSX sampler state.
		u8 wrap_u = 1;
		u8 wrap_v = 1;

		// Alpha test carried over from the descriptor, applied to the material at upload time.
		u8 alpha_func = 7;
		u8 alpha_ref = 0;

		// Range of the decoded alpha channel, measured once at upload. The instance blend ext
		// tells Remix that surface alpha is the albedo texture's alpha channel
		// (textureAlphaOperation = SelectArg1, arg1 = Texture), which is a D3D9 fixed-function
		// assumption: an RSX title computes alpha in its fragment program and may not put it in
		// the texture at all. min == max says this texture cannot be what cuts a sprite's
		// backing out, and min == max == 255 in particular means an alpha-blended draw using it
		// is fully opaque however the blend factors are set. DXT1 can only ever produce 255 or
		// a hard 0 (bcdec.hpp:157-159 decodes it with onlyOpaqueMode false), never a gradient.
		u8 alpha_min = 255;
		u8 alpha_max = 0;

		// Mean of the decoded R/G/B channels in 0..1, measured in the same single pass as the alpha
		// range above. This is the *fixture's own colour*, which is what a light derived from that
		// fixture should be tinted by: a sodium lamp texture is orange and its light should be too.
		// Defaults to white so an entry that never reached upload() tints nothing.
		//
		// Deliberately a mean and not a max: a lamp texture whose glass is orange over a white
		// housing has a max of white, which would throw away the very hue this exists to carry.
		f32 mean_rgb[3] = { 1.f, 1.f, 1.f };

		// Round 23. Where the BRIGHTEST region of this texture sits, in normalised texel
		// coordinates (u = column / width, v = row / height, row 0 = the first decoded row).
		// peak_uv[0] < 0 means "never measured" - the walk below only runs for textures whose
		// content hash is on RPCS3_REMIX_SKYEMISSIVE, so every other texture pays nothing.
		//
		// It is the luminance-weighted CENTROID of the texels within 2 % of the peak, not the
		// single brightest texel: MEASURED offline on the dumped Selva dome
		// (unit0_D1A6D1B27ADE6232_2048x1024.bmp) the sun is a broad warm glow, not a hard disc -
		// zero texels reach luma 250, 112 reach 245, 3060 reach 220 - so the single brightest
		// texel (1340,513) is 46 texels off the centroid of the region it belongs to.
		f32 peak_uv[2] = { -1.f, -1.f };
		f32 peak_luma = 0.f;
		u32 peak_texels = 0;

		// Decoded BGRA8, kept so the UI compositor can sample it CPU-side.
		std::vector<u8> pixels;

		// CELL_GCM_TEXTURE_B8 carries one channel. The Remix material keeps the opaque grayscale
		// expansion above, while the UI compositor interprets the same byte as glyph coverage.
		bool b8_coverage = false;

		u64 last_used_frame = 0;
		bool unsupported = false;

		// Why the decode refused, when it did - the same static string note_refusal prints. Added
		// beside 'unsupported' rather than replacing it: every existing reader tests the bool and
		// the point of this round is to measure, not to move behaviour. Two of the five reasons
		// ("unreadable" and "no-mip0") are *transient* - the guest range was not mapped yet, or the
		// mip chain had not been written - and this cache records them as permanently unsupported
		// for the whole 21,600-frame residency. tex_tomb_transient counts how much of the white
		// population that accounts for, which is the entry ticket for a round-5 tombstone retry.
		const char* refusal_reason = "";
	};

	struct texture_stats
	{
		u64 created = 0;
		u64 destroyed = 0;
		u64 hits = 0;
		u64 deferred = 0;
		u64 unreadable = 0;
		u64 unsupported = 0;
		u64 rehashed = 0;
		u64 refreshed = 0;
		u64 materials = 0;
		// Of the 'unsupported' population, how many were a descriptor already tombstoned by an
		// earlier failure rather than a fresh refusal. 'unsupported' counts per draw, so a
		// handful of bad descriptors inflate it without bound and it cannot be read as "how
		// many textures does this title bind that we cannot decode". This split can.
		u64 tombstone_hits = 0;

		// Materials created while the RSX alpha test was disabled, i.e. whose alphaTestType is
		// ALWAYS and whose transparency - if it has any - can only come from the per-draw blend
		// state chained onto the instance, never from the material itself. Alpha state is part of
		// texture_descriptor::key(), so this is not a staleness measure: the same texture bound
		// under a real alpha test gets its own entry and is counted separately. Read it against
		// 'materials'. High here on a title whose foliage looks like solid cards says the cutout
		// is blend-driven and the material is not the place to look; near zero says the opposite.
		u64 materials_untested = 0;

		// Tombstone hits whose recorded refusal reason was a *transient* condition - "unreadable"
		// (the guest range was not mapped when the decode ran) or "no-mip0" (the mip chain had not
		// been written yet) - rather than a genuine format or size refusal. These are the entries
		// that should arguably be retried instead of remembered, and until this counter existed
		// there was no way to tell how much of the permanent-white population they are. Measurement
		// only this round; nothing retries.
		u64 tombstone_transient = 0;
		// CreateTexture calls whose content hash was already live under a *different* descriptor
		// key. The descriptor-only cache key means one image can hold several entries (different
		// wrap, different alpha state), which is intended - but it also means the content hash the
		// mesh key and every conf texture list are written against is not unique to an entry. This
		// is the direct measurement of the duplicate-hash-namespace suspicion behind the wrong-
		// texture sightings. Non-zero is the round-5 entry ticket; zero kills the theory outright.
		u64 key_duplicate_hash = 0;

		// Materials created with a non-zero emissiveIntensity because their content hash is on
		// RPCS3_REMIX_EMISSIVE. The partition is "listed and created", not "listed": a typo'd hash
		// and a hash whose texture never decodes are both 0 here, which is the difference between
		// "the list did nothing" and "the list is empty".
		u64 materials_emissive = 0;

		// Round 13. The RPCS3_REMIX_SKYEMISSIVE subset of the above - a strict subset, because the
		// sky branch runs inside the same 'if' and increments both. This is the counter the sky
		// census reads to answer "was the emissive material actually created", which is a different
		// question from "is the hash on the list": a hash whose texture never decodes never reaches
		// CreateMaterial at all.
		u64 materials_sky_emissive = 0;

		// Round 13. The subset of the above that also declared BlendType::kEmissive, i.e. the ones
		// that will reach the unordered TLAS and stop occluding the sun. Separate from
		// materials_sky_emissive so RPCS3_REMIX_SKYEMISSIVEBLEND=0 is visible as a number: with the
		// knob off these two counters disagree, and that difference IS the A/B.
		u64 materials_sky_unordered = 0;

		// Round 14 (ITEM 2). The RPCS3_REMIX_SUNCARDEMISSIVE twin of materials_sky_emissive: a
		// SUNCARDALBEDO hash that reached CreateMaterial and got the emissive + kEmissive treatment.
		// It is DISJOINT from materials_sky_emissive by construction (the sun-card test requires
		// !sky_emissive), so the two never double-count one texture. materials_sky_unordered counts
		// both, because both declare the same blend type and it is the one thing they share.
		// Reading it: 0 with a non-empty SUNCARDALBEDO means the hash never reached CreateMaterial -
		// wrong hash, or the texture never decoded. This is the numeric acceptance for ITEM 2.
		u64 materials_sun_card = 0;

		// Round 7. Entries whose wrap mode was overridden to CLAMP because their content hash is on
		// RPCS3_REMIX_CLAMPALBEDO - the UI seam lever for the GPU-sampled world-space-UI route.
		// Counted per created entry, like materials_emissive and for the same reason: "listed and
		// created" separates a typo'd hash from a list that is simply empty. Round 8: that is now
		// literally what it counts - a listed texture the guest had ALREADY bound clamp counts
		// too (the list entry is correct, it just had nothing to change), and a failed
		// CreateTexture counts for nothing.
		u64 wrap_forced = 0;

		// Round 8, measurement only. Cache HITS whose guest bytes had changed since the last
		// cadence verify while the descriptor key stayed put - i.e. the "stale pool slot" arm of
		// the wrong-texture bug ("a Mantel soldier rendered in tree bark"): a streaming pool
		// recycles an address, the descriptor-only key still hits, and the entry keeps serving the
		// previous image's pixels AND the previous image's Remix material. Nothing is rebuilt on
		// this count - see the RPCS3_REMIX_TEXVERIFY note for why the obvious in-place rebuild is
		// not safe in this tree - so a non-zero here is evidence, not a fix.
		u64 stale_detected = 0;

		// Round 17. The subset of stale_detected that RPCS3_REMIX_TEXSTALEEVICT acted on: entries
		// erased so the next bind decodes the guest bytes that are there NOW and derives the
		// content hash of the image actually resident. With the knob off (its default) this stays 0
		// while stale_detected climbs, and that difference IS the A/B for the bark-helmet family.
		// stale_orphaned counts the texture+material pairs deliberately NOT destroyed on the way
		// out - see the orphan note at the erase site. The two should track each other exactly.
		u64 stale_evicted = 0;
		u64 stale_orphaned = 0;

		// --- round 9: the reap split ------------------------------------------------------------
		// reap() used to have exactly one outcome and no counter: an entry idle past TEXIDLE lost
		// its material and its texture and was erased. It consulted nothing about whether a live
		// MESH still referenced that material - and the mesh cache bakes the material handle into
		// the mesh at CreateMesh while keying the mesh on its CONTENT, so a reaped-then-rebound
		// texture gets a NEW material handle while the reused mesh keeps the DESTROYED one
		// forever. That is a permanently white surface, and if the runtime ever recycles the freed
		// handle value it is a wrongly-textured one.
		//
		// reap_kept: idle entries spared because a live mesh still references their material
		// (their last_used_frame is bumped so they re-age normally). reap_freed: idle entries
		// reaped as before. Read them across the 30-second idle repro: reap_kept climbing while
		// the screen stays textured IS the fix firing. RPCS3_REMIX_TEXREAPSAFE=0 drives reap_kept
		// to 0 and restores today's reap bit-exactly.
		u64 reap_kept = 0;
		u64 reap_freed = 0;

		// ROUND 39. promote_sky_emissive()'s two numbers, and they answer different questions.
		// sky_promote_entries: texture-cache entries whose material was rebuilt because their
		// content hash was classified as a sky dome. This is per ENTRY, and this title aliases
		// ~45 descriptor entries onto one content hash, so it is expected to be much larger than
		// the number of domes - read it against skyclassify_armed on the live line, not instead
		// of it. sky_promote_orphans: how many old material handles were left alive rather than
		// destroyed (the mesh-lifetime rule; see promote_sky_emissive's comment). The two are
		// equal unless a rebuild returned the same handle. Both stay 0 at SKYCLASSIFY<2.
		u64 sky_promote_entries = 0;
		u64 sky_promote_orphans = 0;

		// ROUND 39. Entries whose material rebuild FAILED during a promotion. Non-zero here with
		// sky_promote_entries at 0 for the same hash is the case the caller undoes: the hash is
		// taken back out of the promoted set rather than left to re-key every later mesh under an
		// identity whose material never changed.
		u64 sky_promote_failed = 0;
	};

	// Per-draw albedo texture cache. Owns every remixapi texture and material it creates.
	class texture_cache
	{
	public:
		texture_cache() = default;
		texture_cache(const texture_cache&) = delete;
		texture_cache& operator=(const texture_cache&) = delete;

		// Called once per flip: resets the per-frame CreateTexture budget.
		void begin_frame();

		// Resolves the bound unit to a material. Returns nullptr when the draw should be
		// submitted without one (unreadable, unsupported, over budget or disabled).
		// 'out_entry' is set on success and stays valid until the next reap.
		//
		// 'refresh_pixels' re-checks the guest bytes on a cache hit and re-decodes the CPU
		// copy in place when they changed, leaving the Remix texture/material handles alone.
		// The UI compositor needs this: titles rewrite a font atlas under a stable descriptor,
		// and the global RPCS3_REMIX_TEXREHASH policy cannot be turned on to catch it because
		// it rebuilds the handles, which changes the albedo hash folded into the mesh key and
		// explodes 3D mesh churn (see texture_rehash_mode's note). Refreshing only the CPU
		// pixels has no effect on any mesh key.
		remixapi_MaterialHandle bind(const remixapi_Interface& api,
			const rsx::fragment_texture& tex,
			u64 frame,
			const texture_entry** out_entry,
			bool refresh_pixels = false);

		// 'live_materials' is the set of material handles that live meshes have baked into their
		// Remix mesh objects. An idle entry whose material is in that set is KEPT rather than
		// destroyed, because destroying it leaves a dangling handle inside a mesh whose key -
		// being content-derived - will never change, so no later bind can heal it. Pass nullptr
		// (or run with RPCS3_REMIX_TEXREAPSAFE=0) for the pre-round-9 behaviour.
		void reap(const remixapi_Interface& api, u64 frame,
			const std::unordered_set<const void*>* live_materials = nullptr);

		// True when at least one entry is past TEXIDLE, i.e. when the next reap() would actually
		// destroy or keep something. The caller uses this to decide whether it is worth walking
		// ~65k mesh entries to build the live-material set: reap runs every flip, but in steady
		// state it has work only once per TEXIDLE window per entry, because a kept entry is
		// re-aged and a freed one is erased. Scans the texture entries only - the cheap side.
		// ROUND 39. Rebuild the MATERIAL of every live entry carrying this content hash, so a
		// texture that was uploaded before the sky classifier identified it still gets the dome's
		// emissive material, its emissive blend type and its peak_uv measurement. The texture
		// handle, the CPU pixels and content_hash are untouched, so no ALBEDO hash moves and no
		// other content hash is affected.
		//
		// CORRECTED: an earlier version of this comment said "no mesh key moves because of this
		// call". That is wrong, and it matters. The material HANDLE is folded into
		// RemixGSRender's static_key and union_hash, so replacing it does move those keys - and
		// the ordinary mesh key is moved DELIBERATELY as well, by folding
		// sky_emissive_promoted() in, because a Remix mesh bakes its material at CreateMesh and
		// nothing else could make the rebuilt material reach the screen. The accurate statement
		// is that this call moves keys only for the promoted content hash.
		//
		// The old material handle is orphaned
		// rather than destroyed; the full lifetime argument is on the definition. Returns the
		// number of entries rebuilt. Idempotent in effect but not free, so the caller promotes a
		// hash once.
		u32 promote_sky_emissive(const remixapi_Interface& api, u64 content_hash);

		bool has_idle(u64 frame) const;
		void destroy_all(const remixapi_Interface& api);

		usz live() const { return m_entries.size(); }
		const texture_stats& stats() const { return m_stats; }

		// True once this frame's CreateTexture budget is spent. A caller walking several texture
		// units for an albedo must stop here: with budget left, a bind() that returns null was
		// refused for a permanent reason (format, dimensions, unreadable) and the next unit is
		// worth trying, but out of budget *every* unit returns null, so continuing would bind
		// whatever unit happens to be cached already - a normal map instead of the diffuse map,
		// this frame only. That is a wrong texture that flickers, which is worse than the white
		// the retry exists to remove.
		bool over_budget() const { return m_budget_left == 0; }

	private:
		bool decode(const rsx::fragment_texture& tex, texture_entry& out);
		bool upload(const remixapi_Interface& api, texture_entry& entry);

		// ROUND 39. Both moved out of upload() verbatim so promote_sky_emissive() can re-run them
		// on an entry that is already uploaded. measure_peak_uv walks the decoded pixels for the
		// sun's position inside a dome texture and does nothing at all unless the content hash is
		// on (or promoted into) RPCS3_REMIX_SKYEMISSIVE. build_material creates the Remix material
		// and, unlike the code it was lifted from, does NOT destroy the texture when CreateMaterial
		// fails - that belongs to upload(), which owns the texture it just created.
		void measure_peak_uv(texture_entry& entry);
		bool build_material(const remixapi_Interface& api, texture_entry& entry);

		// One notice per distinct (format, dimensions, reason) a decode refuses. Without it
		// 'tex_unsupported' is a single number that names neither the format nor the count of
		// distinct offenders, and "many surfaces stay white" cannot be attributed.
		void note_refusal(const char* reason, u32 gcm_format, u32 width, u32 height);

		// Round 8. One 'Remix texstale:' line per descriptor key whose guest bytes moved under it,
		// and one 'Remix texdup:' line per content hash living under two descriptor keys. Both are
		// bounded, both mirror to remix_dump.log, and neither changes a decision - they are the two
		// halves of the evidence that says which arm of the wrong-texture bug is real.
		void note_stale(u64 key, const texture_entry& entry, u32 format, u64 frame);
		void note_content_dup(u64 content_hash, u64 key_a, u64 key_b, u32 format, u32 width, u32 height);

		static constexpr u32 s_max_texstale_lines = 64;
		static constexpr u32 s_max_texdup_lines = 64;
		std::unordered_set<u64> m_texstale_seen;
		std::unordered_set<u64> m_texdup_seen;
		u32 m_texstale_lines = 0;
		u32 m_texdup_lines = 0;

		std::unordered_map<u64, texture_entry> m_entries;

		// content_hash -> the descriptor key that first published it. Read-only bookkeeping for
		// key_duplicate_hash; nothing consults it to make a decision, and it is deliberately not
		// pruned on reap - a hash that comes back under a second key after the first was reaped is
		// exactly the collision this is meant to catch, and pruning would hide it.
		std::unordered_map<u64, u64> m_content_keys;
		std::unordered_set<u64> m_refusals_seen;
		texture_stats m_stats{};
		u32 m_budget_left = 0;

		// Reused across draws to keep the hot path allocation free.
		std::vector<u8> m_scratch;
	};

	// Env knobs, read once. RPCS3_REMIX_NOTEX=1 disables the whole workstream,
	// RPCS3_REMIX_TEXBUDGET caps CreateTexture calls per frame, RPCS3_REMIX_TEXLINEAR=1
	// uploads UNORM instead of SRGB, RPCS3_REMIX_TEXREHASH picks the staleness policy
	// (0 = descriptor only, 1 = sampled fingerprint (default), 2 = full hash every draw).
	bool textures_disabled();
	u32 texture_budget();
	bool textures_linear();

	// RPCS3_REMIX_TEXVERIFY=<frames> (default 120; 0 disables): how often a live cache entry
	// re-hashes its guest bytes to find out whether the image under a stable descriptor key has
	// been replaced. MEASUREMENT ONLY - it counts (tex_stale_detected) and names
	// ('Remix texstale:'), and rebuilds nothing.
	//
	// Why nothing is rebuilt, stated so the next round does not have to rediscover it: the honest
	// fix is an in-place rebuild with content_hash PINNED (the existing refresh_pixels idiom), so
	// the mesh key never moves and the 32x mesh-churn trap that made RPCS3_REMIX_TEXREHASH
	// unusable is avoided. But pinning the hash is exactly what makes destroying and re-creating
	// the Remix texture + material unsafe HERE: the mesh cache bakes the material handle into the
	// mesh at CreateMesh time and a reused mesh keeps it (RemixGSRender submit path), so a mesh
	// key that does not move keeps submitting a destroyed material. Refreshing only the CPU pixels
	// is safe but does not reach the GPU-sampled route, which is the one the sighting is on.
	// A safe fix needs a way to re-point a live material, which is a round-9 question.
	//
	// Cost: one FNV over the guest range per entry per TEXVERIFY frames, amortised - not per bind.
	u32 texture_verify_frames();

	// RPCS3_REMIX_TEXSTALEEVICT=1 (default 0 = build 4b7bdb4, bit for bit). Acts on the detector
	// above: when the cadence verify finds the guest bytes under a stable descriptor key have
	// changed, ERASE the entry so the next bind decodes the image that is actually there.
	//
	// This is the "my helmet turned into tree bark" family, and the mechanism is not a hash
	// collision. texture_descriptor::key() identifies a texture by WHERE it is - offset, location,
	// format, pitch, dims, wrap, alpha state - while content_hash identifies it by WHAT it is. Haze
	// streams into a recycled address pool, so bark is decoded at address X, the helmet is later
	// written over X, and the helmet's bind produces the SAME descriptor key, hits the cache and is
	// handed back the material whose albedoTexture path is 0x<bark hash>. Nothing ages it out
	// either: tex_destroyed and reap_freed are both 0 over 18,269 flips on the round-16 run, so
	// every swap is permanent for the session. Measured there: tex_stale_detected=134 over 63+
	// distinct keys.
	//
	// The round-8 note above assumed the fix had to be an in-place rebuild with content_hash
	// PINNED, and that pin is what makes destroying the material unsafe. Erasing does not need the
	// pin - the next bind derives the correct hash and re-keys its own meshes - so the only thing
	// still required is that the OLD material outlive the meshes that baked it, which the orphan
	// branch guarantees.
	//
	// Not the RPCS3_REMIX_TEXREHASH trap: that hangs off a strided sample_fingerprint that
	// false-positives (604 rehashes of 682 textures, 32x mesh churn). This hangs off the full-hash
	// cadence verify. Expected blast radius on Haze: 134 entries rebuilt in 18,269 flips - 1.2% of
	// tex_created, 0.003% of tex_hits, and every one of them was serving the wrong image.
	// Counters: tex_stale_evicted, tex_stale_orphaned. Inert unless TEXVERIFY != 0.
	bool texture_stale_evict();

	// RPCS3_REMIX_BLENDSTATE=0 restores the behaviour up to and including ae94587: every material
	// declared its own alpha state (useDrawCallAlphaState = 0) and no remixapi_InstanceInfoBlendEXT
	// was ever chained onto an instance, so every draw reached the runtime fully opaque regardless
	// of NV4097_SET_BLEND_ENABLE. One Resistance 2 capture of 665 dumped draws had ~163 with
	// blend=1 (104 depth_test=1/depth_write=0, 32 depth_write=1, 27 depth_test=0/depth_write=0) and
	// all 163 shipped opaque - which is why its god rays were solid white walls, its sun card
	// occluded the level and its world-space distance markers were black boxes.
	//
	// With the knob on (default) the material sets useDrawCallAlphaState = 1 and the per-draw blend
	// *and* alpha-test state travels on the instance instead. It has to be one switch, not two:
	// rtx_instance_manager.cpp calculateAlphaState() reads useLegacyAlphaState once and it gates
	// both halves (alpha test at :687-693, alpha blend at :705-709), so a build that took blend
	// from the instance and alpha test from the material is not expressible.
	bool blend_state_enabled();

	// RPCS3_REMIX_TEXBMP=1: write each unique decoded texture out as a BMP next to the executable.
	bool dump_texture_images();
	u32 texture_rehash_mode();

	// RPCS3_REMIX_TEXIDLE=<frames>: how long an unreferenced texture keeps its decode and its Remix
	// handles before reap() releases them. Config "Texture Idle Frames", default 300 (~5 s at
	// 60 fps). The tell for it being too low is tex_created and tex_destroyed both climbing over a
	// window in which the camera stayed in one area; raising it costs VRAM, which tex_live reports.
	// 0 reaps a texture the frame it stops being bound, so umax is the unset sentinel.
	u32 texture_idle_frames();

	// RPCS3_REMIX_TEXREAPSAFE=1 (default): reap() may not destroy the material of a texture that a
	// live mesh still has baked in.
	//
	// The lifetime hole this closes is proven by code inspection, independently of whether it is
	// also the idle-degradation mechanism: reap() consults nothing about meshes, the mesh cache
	// bakes the material handle at CreateMesh, and a mesh key is content-derived and therefore
	// stable - so a reaped material leaves a dangling handle in a mesh that will be reused, not
	// rebuilt, for as long as its geometry is unchanged. No bind can heal that; there is no code
	// path that re-points a mesh's material.
	//
	// It is also the leading suspect for "stand still ~30 s and the room degrades to flat white":
	// TEXIDLE's config default is 300 frames, which at this title's 30-50 fps is 6-10 s, the right
	// order for the observed onset, while MESHIDLE at 3600 frames is 72-120 s - the wrong order.
	// 0 restores the old reap exactly and is the control for that experiment.
	bool texture_reap_safe();
}

#endif
