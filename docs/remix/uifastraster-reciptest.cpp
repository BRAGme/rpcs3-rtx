// Exhaustive proof for the compositor's reciprocal division: for every divisor d in 1..255
// (out_alpha's full range) and every numerator n in [0, 2^18), (n * recip[d]) >> 32 must equal
// n / d. The reachable numerator bound is src*alpha + dst_term + out_alpha/2 <= 255*254 + 64770
// + 127 = 129667 < 2^18, so the tested range strictly contains everything blend() can form.
#include <cstdint>
#include <cstdio>

typedef uint32_t u32;
typedef uint64_t u64;

int main()
{
	// Same construction the compositor will use: ceil(2^32 / d) as floor((2^32 - 1) / d) + 1.
	u64 recip[256]{};
	for (u32 d = 1; d < 256; ++d)
		recip[d] = (0xFFFFFFFFull / d) + 1;

	u64 checked = 0, bad = 0;
	const u32 n_end = 1u << 18;

	for (u32 d = 1; d < 256; ++d)
	{
		for (u32 n = 0; n < n_end; ++n)
		{
			const u32 ref = n / d;
			const u32 got = static_cast<u32>((u64{n} * recip[d]) >> 32);
			++checked;
			if (ref != got)
			{
				if (bad < 10) std::printf("MISMATCH d=%u n=%u ref=%u got=%u\n", d, n, ref, got);
				++bad;
			}
		}
	}

	std::printf("divisors=1..255 numerators=0..%u checked=%llu mismatches=%llu\n",
		n_end - 1, static_cast<unsigned long long>(checked), static_cast<unsigned long long>(bad));
	std::printf("max reachable numerator=%u (255*254 + 64770 + 127); recip[1]=%llu recip[255]=%llu\n",
		255u * 254u + 64770u + 127u, static_cast<unsigned long long>(recip[1]), static_cast<unsigned long long>(recip[255]));
	return bad == 0 ? 0 : 1;
}
