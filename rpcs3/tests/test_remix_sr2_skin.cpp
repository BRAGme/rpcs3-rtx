#include <gtest/gtest.h>

#include "Emu/RSX/Program/RSXVertexProgram.h"
#include "Emu/RSX/Remix/RemixTransforms.h"
#include "Emu/RSX/rsx_methods.h"

#include <bit>
#include <cmath>
#include <limits>

#ifdef _WIN32

namespace remix_rsx
{
	namespace
	{
		// Exact little-endian words from bin/remix_ucode/<hash>.vp, not reconstructed
		// shaders. These SR2 programs combine four indexed DPH bones, weight-sum
		// normalization, reused temporaries and (except 527C) an attr12 morph delta.
		constexpr u32 s_F7FA79C9F2466421[] = {
			0x401f9c6c, 0x009d3808, 0x012a80c3, 0x60419f9c, 0x00009c6c, 0x005d2000, 0x0186c083, 0x60403ffc,
			0x00001c6c, 0x005d2000, 0x0186c083, 0x60403ffc, 0x00021c6c, 0x03c00770, 0x8106c083, 0x6041fffc,
			0x00019c6c, 0x00c00100, 0x0106c08a, 0xa0403ffc, 0x00001c6c, 0x009d3d0c, 0x013fc0c3, 0x6041dffc,
			0x00011c6c, 0x005d307f, 0x8186c083, 0x60411ffc, 0x00009c6c, 0x0040000c, 0x0106c083, 0x6041dffc,
			0x00019c6c, 0x011d3c0c, 0x010000c3, 0x00a1dffc, 0x00011c6c, 0x011d2e0d, 0x8100026a, 0xa061fffc,
			0x00009c6c, 0x00c0020c, 0x0086c083, 0x0041dffc, 0x00001c6c, 0x00c00155, 0x0106c09f, 0xe1a11ffc,
			0x00021c6c, 0x009d300d, 0x889540c3, 0x6041fffc, 0x00001c6c, 0x0340000d, 0x8886c083, 0x6041fffc,
			0x00019c6c, 0x00c0017f, 0x8106c080, 0x00203ffc, 0x00021c6c, 0x0140000c, 0x02860143, 0x60403ffc,
			0x00001c6c, 0x1040000c, 0x0486c09f, 0xe1a3c1fc, 0x00021c6c, 0x01c3600d, 0x8186c043, 0x60405ffe,
			0x00021c6c, 0x01c3500d, 0x8186c043, 0x60409ffe, 0x00021c6c, 0x01c3400d, 0x8186c043, 0x60411ffe,
			0x00039c6c, 0x0183600c, 0x0686c0c3, 0x60405ffe, 0x00039c6c, 0x0183500c, 0x0686c0c3, 0x60409ffe,
			0x00039c6c, 0x0183400c, 0x0686c0c3, 0x60411ffe, 0x00041c6d, 0x0183600c, 0x0686c0c3, 0x60405ffe,
			0x00041c6d, 0x0183500c, 0x0686c0c3, 0x60409ffe, 0x00041c6d, 0x0183400c, 0x0686c0c3, 0x60411ffe,
			0x00029c6d, 0x01c3600d, 0x8186c043, 0x60405ffe, 0x00029c6d, 0x01c3500d, 0x8186c043, 0x60409ffe,
			0x00029c6d, 0x01c3400d, 0x8186c043, 0x60411ffe, 0x00031c6e, 0x01c3600d, 0x8186c043, 0x60405ffe,
			0x00031c6e, 0x01c3500d, 0x8186c043, 0x60409ffe, 0x00031c6e, 0x01c3400d, 0x8186c043, 0x60411ffe,
			0x00049c6e, 0x0183600c, 0x0686c0c3, 0x60405ffe, 0x00049c6e, 0x0183500c, 0x0686c0c3, 0x60409ffe,
			0x00049c6e, 0x0183400c, 0x0686c0c3, 0x60411ffe, 0x00011c6f, 0x0183600c, 0x0686c0c3, 0x60405ffe,
			0x00011c6f, 0x0183500c, 0x0686c0c3, 0x60409ffe, 0x00051c6f, 0x01c3600d, 0x8186c043, 0x60405ffe,
			0x00051c6f, 0x01c3500d, 0x8186c043, 0x60409ffe, 0x00051c6f, 0x01c3400d, 0x8186c043, 0x60411ffe,
			0x001f9c6c, 0x2000000d, 0x8106c09f, 0xe230007c, 0x00011c6f, 0x0183400c, 0x0686c0c3, 0x60411ffe,
			0x00059c6c, 0x0080012a, 0x81060a43, 0x6041dffc, 0x00009c6c, 0x00800000, 0x00860143, 0x6041dffc,
			0x00051c6c, 0x0080012a, 0x81060243, 0x6041dffc, 0x00001c6c, 0x01c3600d, 0x8186c143, 0x60405ffe,
			0x00001c6c, 0x01c3500d, 0x8186c143, 0x60409ffe, 0x00001c6c, 0x01c3400d, 0x8186c143, 0x60411ffe,
			0x00011c6d, 0x01c3600d, 0x8186c143, 0x60405ffe, 0x00011c6d, 0x01c3500d, 0x8186c143, 0x60409ffe,
			0x00011c6d, 0x01c3400d, 0x8186c143, 0x60411ffe, 0x00019c6e, 0x01c3600d, 0x8186c143, 0x60405ffe,
			0x00019c6e, 0x01c3500d, 0x8186c143, 0x60409ffe, 0x00019c6e, 0x01c3400d, 0x8186c143, 0x60411ffe,
			0x00061c6f, 0x01c3600d, 0x8186c143, 0x60405ffe, 0x00061c6f, 0x01c3500d, 0x8186c143, 0x60409ffe,
			0x00061c6f, 0x01c3400d, 0x8186c143, 0x60411ffe, 0x00009c6c, 0x01000100, 0x01060943, 0x0521dffc,
			0x00031c6c, 0x01000100, 0x01060643, 0x05a1dffc, 0x00029c6c, 0x01000155, 0x01060543, 0x0321dffc,
			0x00009c6c, 0x01000155, 0x01060843, 0x00a1dffc, 0x00009c6c, 0x0100017f, 0x81060743, 0x00a1dffc,
			0x00021c6c, 0x0100017f, 0x81060443, 0x02a1dffc, 0x00029c6c, 0x0080012a, 0x81060c43, 0x6041dffc,
			0x00019c6c, 0x01000100, 0x01060343, 0x02a1dffc, 0x00001c6c, 0x0140000c, 0x08860443, 0x60403ffc,
			0x00011c6c, 0x01000155, 0x01060243, 0x01a1dffc, 0x00009c6c, 0x0080000c, 0x02bfc343, 0x6041dffc,
			0x401f9c6c, 0x0180700c, 0x0286c0c3, 0x60403f80, 0x401f9c6c, 0x0180600c, 0x0286c0c3, 0x60405f80,
			0x401f9c6c, 0x0180500c, 0x0286c0c3, 0x60409f80, 0x00011c6c, 0x0100017f, 0x81060043, 0x0121dffc,
			0x401f9c6c, 0x0180400c, 0x0286c0c3, 0x60411f80, 0x001f9c6c, 0x2000000d, 0x8106c09f, 0xe02200fc,
			0x00001c6c, 0x0140000c, 0x04860243, 0x60403ffc, 0x00001c6c, 0x00c0a08c, 0x0286c083, 0x0061dffc,
			0x00019c6c, 0x2080007f, 0x8286045f, 0xe023c07c, 0x00011c6c, 0x0080007f, 0x80860243, 0x6041dffc,
			0x00001c6c, 0x0140000c, 0x04860243, 0x60403ffc, 0x00021c6c, 0x00800043, 0x04984343, 0x6041dffc,
			0x00019c6c, 0x01000030, 0x84a18363, 0x0221dffc, 0x00009c6c, 0x00c0208c, 0x0286c083, 0x0061dffc,
			0x00019c6c, 0x2080000c, 0x06bfc25f, 0xe023c07c, 0x00009c6c, 0x0140000c, 0x06860343, 0x60403ffc,
			0x00011c6c, 0x0080007f, 0x80860243, 0x6041dffc, 0x401f9c6c, 0x0140000c, 0x02860243, 0x60405fa0,
			0x401f9c6c, 0x2141200c, 0x048600df, 0xe0a24024, 0x401f9c6c, 0x0140000c, 0x04860043, 0x60405fa8,
			0x00019c6c, 0x0080007f, 0x80860343, 0x6041dffc, 0x401f9c6c, 0x0140000c, 0x02860343, 0x60409fa0,
			0x401f9c6c, 0x0141200c, 0x068600c3, 0x60409fa4, 0x00021c6c, 0x0080000c, 0x06bfc243, 0x6041dffc,
			0x00029c6c, 0x00800030, 0x84a18443, 0x6041dffc, 0x00011c6c, 0x01000043, 0x04984463, 0x02a1dffc,
			0x401f9c6c, 0x0140000c, 0x06860043, 0x60409fa8, 0x401f9c6c, 0x0140000c, 0x02860243, 0x60411fa0,
			0x401f9c6c, 0x0141200c, 0x048600c3, 0x60411fa4, 0x401f9c6c, 0x0140000c, 0x04860043, 0x60411fa9,
		};

		constexpr u32 s_F1E164AF57003605[] = {
			0x00009c6c, 0x005d2000, 0x0186c083, 0x60403ffc, 0x00001c6c, 0x005d2000, 0x0186c083, 0x60403ffc,
			0x00021c6c, 0x009d3808, 0x012a80c3, 0x60419ffc, 0x00029c6c, 0x03c00770, 0x8106c083, 0x6041fffc,
			0x00019c6c, 0x00c00100, 0x0106c08a, 0xa0403ffc, 0x00001c6c, 0x009d3d0c, 0x013fc0c3, 0x6041dffc,
			0x00011c6c, 0x005d307f, 0x8186c083, 0x60411ffc, 0x00009c6c, 0x009d3c0c, 0x010000c3, 0x6041dffc,
			0x00019c6c, 0x0101300c, 0x010600c3, 0x00a1dffc, 0x00011c6c, 0x011d2e0d, 0x8100026a, 0xa061fffc,
			0x00009c6c, 0x00c0020c, 0x0086c083, 0x0041dffc, 0x00001c6c, 0x00c00155, 0x0106c09f, 0xe1a11ffc,
			0x00029c6c, 0x009d300d, 0x8a9540c3, 0x6041fffc, 0x00021c6c, 0x00818000, 0x888040c3, 0x60407ffc,
			0x401f9c6c, 0x0040000d, 0x8886c083, 0x6041ff9c, 0x00001c6c, 0x0340000d, 0x8a86c083, 0x6041fffc,
			0x00019c6c, 0x00c0017f, 0x8106c080, 0x00203ffc, 0x00021c6c, 0x0140000c, 0x02860143, 0x60403ffc,
			0x00001c6c, 0x1040000c, 0x0486c09f, 0xe1a3c1fc, 0x00021c6c, 0x01c3600d, 0x8186c043, 0x60405ffe,
			0x00021c6c, 0x01c3500d, 0x8186c043, 0x60409ffe, 0x00021c6c, 0x01c3400d, 0x8186c043, 0x60411ffe,
			0x00039c6c, 0x0183600c, 0x0686c0c3, 0x60405ffe, 0x00039c6c, 0x0183500c, 0x0686c0c3, 0x60409ffe,
			0x00039c6c, 0x0183400c, 0x0686c0c3, 0x60411ffe, 0x00041c6d, 0x0183600c, 0x0686c0c3, 0x60405ffe,
			0x00041c6d, 0x0183500c, 0x0686c0c3, 0x60409ffe, 0x00041c6d, 0x0183400c, 0x0686c0c3, 0x60411ffe,
			0x00029c6d, 0x01c3600d, 0x8186c043, 0x60405ffe, 0x00029c6d, 0x01c3500d, 0x8186c043, 0x60409ffe,
			0x00029c6d, 0x01c3400d, 0x8186c043, 0x60411ffe, 0x00031c6e, 0x01c3600d, 0x8186c043, 0x60405ffe,
			0x00031c6e, 0x01c3500d, 0x8186c043, 0x60409ffe, 0x00031c6e, 0x01c3400d, 0x8186c043, 0x60411ffe,
			0x00049c6e, 0x0183600c, 0x0686c0c3, 0x60405ffe, 0x00049c6e, 0x0183500c, 0x0686c0c3, 0x60409ffe,
			0x00049c6e, 0x0183400c, 0x0686c0c3, 0x60411ffe, 0x00011c6f, 0x0183600c, 0x0686c0c3, 0x60405ffe,
			0x00011c6f, 0x0183500c, 0x0686c0c3, 0x60409ffe, 0x00051c6f, 0x01c3600d, 0x8186c043, 0x60405ffe,
			0x00051c6f, 0x01c3500d, 0x8186c043, 0x60409ffe, 0x00051c6f, 0x01c3400d, 0x8186c043, 0x60411ffe,
			0x001f9c6c, 0x2000000d, 0x8106c09f, 0xe230007c, 0x00011c6f, 0x0183400c, 0x0686c0c3, 0x60411ffe,
			0x00059c6c, 0x0080012a, 0x81060a43, 0x6041dffc, 0x00009c6c, 0x00800000, 0x00860143, 0x6041dffc,
			0x00051c6c, 0x0080012a, 0x81060243, 0x6041dffc, 0x00001c6c, 0x01c3600d, 0x8186c143, 0x60405ffe,
			0x00001c6c, 0x01c3500d, 0x8186c143, 0x60409ffe, 0x00001c6c, 0x01c3400d, 0x8186c143, 0x60411ffe,
			0x00011c6d, 0x01c3600d, 0x8186c143, 0x60405ffe, 0x00011c6d, 0x01c3500d, 0x8186c143, 0x60409ffe,
			0x00011c6d, 0x01c3400d, 0x8186c143, 0x60411ffe, 0x00019c6e, 0x01c3600d, 0x8186c143, 0x60405ffe,
			0x00019c6e, 0x01c3500d, 0x8186c143, 0x60409ffe, 0x00019c6e, 0x01c3400d, 0x8186c143, 0x60411ffe,
			0x00061c6f, 0x01c3600d, 0x8186c143, 0x60405ffe, 0x00061c6f, 0x01c3500d, 0x8186c143, 0x60409ffe,
			0x00061c6f, 0x01c3400d, 0x8186c143, 0x60411ffe, 0x00009c6c, 0x01000100, 0x01060943, 0x0521dffc,
			0x00031c6c, 0x01000100, 0x01060643, 0x05a1dffc, 0x00029c6c, 0x01000155, 0x01060543, 0x0321dffc,
			0x00009c6c, 0x01000155, 0x01060843, 0x00a1dffc, 0x00009c6c, 0x0100017f, 0x81060743, 0x00a1dffc,
			0x00021c6c, 0x0100017f, 0x81060443, 0x02a1dffc, 0x00029c6c, 0x0080012a, 0x81060c43, 0x6041dffc,
			0x00019c6c, 0x01000100, 0x01060343, 0x02a1dffc, 0x00001c6c, 0x0140000c, 0x08860443, 0x60403ffc,
			0x00011c6c, 0x01000155, 0x01060243, 0x01a1dffc, 0x00009c6c, 0x0080000c, 0x02bfc343, 0x6041dffc,
			0x401f9c6c, 0x0180700c, 0x0286c0c3, 0x60403f80, 0x401f9c6c, 0x0180600c, 0x0286c0c3, 0x60405f80,
			0x401f9c6c, 0x0180500c, 0x0286c0c3, 0x60409f80, 0x401f9c6c, 0x0180400c, 0x0286c0c3, 0x60411f80,
			0x00019c6c, 0x00c0a08c, 0x0286c083, 0x0061dffc, 0x00001c6c, 0x0100017f, 0x81060043, 0x0121dffc,
			0x00009c6c, 0x00c0208c, 0x0286c083, 0x0061dffc, 0x001f9c6c, 0x2000000d, 0x8106c09f, 0xe02200fc,
			0x00001c6c, 0x0140000c, 0x00860043, 0x60403ffc, 0x00011c6c, 0x0080007f, 0x82860443, 0x6041dffc,
			0x401f9c6c, 0x0140000c, 0x02860243, 0x60411fa0, 0x401f9c6c, 0x2141200c, 0x048600df, 0xe0230024,
			0x401f9c6c, 0x0140000c, 0x04860343, 0x60411fa8, 0x00001c6c, 0x0080007f, 0x80860043, 0x6041dffc,
			0x401f9c6c, 0x0140000c, 0x02860043, 0x60405fa0, 0x401f9c6c, 0x0141200c, 0x008600c3, 0x60405fa4,
			0x00021c6c, 0x00800043, 0x00984243, 0x6041dffc, 0x00011c6c, 0x01000030, 0x80a18263, 0x0221dffc,
			0x401f9c6c, 0x0140000c, 0x00860343, 0x60405fa8, 0x00001c6c, 0x0080000c, 0x04bfc243, 0x6041dffc,
			0x401f9c6c, 0x0140000c, 0x02860043, 0x60409fa0, 0x401f9c6c, 0x0141200c, 0x008600c3, 0x60409fa4,
			0x401f9c6c, 0x0140000c, 0x00860343, 0x60409fa9,
		};

		constexpr u32 s_527C1166C9EF61E5[] = {
			0x00001c6c, 0x03c00770, 0x8106c083, 0x6041fffc, 0x00009c6c, 0x00c00100, 0x0106c08a, 0xa0411ffc,
			0x00009c6c, 0x00c00155, 0x0106c080, 0x00a11ffc, 0x00001c6c, 0x009d300d, 0x808000c3, 0x6041fffc,
			0x00001c6c, 0x0340000d, 0x8086c083, 0x6041fffc, 0x00001c6c, 0x00c0017f, 0x8106c080, 0x00a11ffc,
			0x001f9c6c, 0x1000000d, 0x8106c080, 0x0022007c, 0x00001c6c, 0x0183600c, 0x0106c0c3, 0x60405ffe,
			0x00001c6c, 0x0183500c, 0x0106c0c3, 0x60409ffe, 0x00001c6c, 0x0183400c, 0x0106c0c3, 0x60411ffe,
			0x00009c6d, 0x0183600c, 0x0106c0c3, 0x60405ffe, 0x00009c6d, 0x0183500c, 0x0106c0c3, 0x60409ffe,
			0x00009c6d, 0x0183400c, 0x0106c0c3, 0x60411ffe, 0x00011c6e, 0x0183600c, 0x0106c0c3, 0x60405ffe,
			0x00019c6f, 0x0183600c, 0x0106c0c3, 0x60405ffe, 0x00019c6f, 0x0183500c, 0x0106c0c3, 0x60409ffe,
			0x00019c6f, 0x0183400c, 0x0106c0c3, 0x60411ffe, 0x00011c6e, 0x0183500c, 0x0106c0c3, 0x60409ffe,
			0x00011c6e, 0x0183400c, 0x0106c0c3, 0x60411ffe, 0x00019c6c, 0x0080012a, 0x81060343, 0x6041dffc,
			0x00011c6c, 0x01000100, 0x01060243, 0x01a1dffc, 0x00009c6c, 0x01000155, 0x01060143, 0x0121dffc,
			0x00001c6c, 0x0100017f, 0x81060043, 0x00a1dffc, 0x00001c6c, 0x0080000c, 0x00bfc043, 0x6041dffc,
			0x00009c6c, 0x00c0a00c, 0x0086c0a3, 0x0061dffc, 0x00001c6c, 0x0140000c, 0x02860143, 0x60403ffc,
			0x401f9c6c, 0x009d3808, 0x011540c3, 0x60419f9c, 0x001f9c6c, 0x2000000d, 0x8106c09f, 0xe022007c,
			0x401f9c6c, 0x0180700c, 0x0086c0c3, 0x60403f80, 0x401f9c6c, 0x0180600c, 0x0086c0c3, 0x60405f80,
			0x001f9c6c, 0x1000000d, 0x8106c09f, 0xe022007c, 0x401f9c6c, 0x0180500c, 0x0086c0c3, 0x60409f80,
			0x00009c6c, 0x00c0e07f, 0x8086c0a8, 0xa0619ffc, 0x401f9c6c, 0x0180400c, 0x0086c0c3, 0x60411f80,
			0x441f9c6c, 0x0080e02a, 0x02aac0c3, 0x60407f9d,
		};

		constexpr u32 s_855E0E3EE97C8DD9[] = {
			0x00001c6c, 0x03c00770, 0x8106c083, 0x6041fffc, 0x00009c6c, 0x00c00100, 0x0106c08a, 0xa0403ffc,
			0x00009c6c, 0x0040000c, 0x0106c083, 0x6041dffc, 0x00021c6c, 0x011d3c0c, 0x010000c3, 0x00a1dffc,
			0x00009c6c, 0x00c00155, 0x0106c09f, 0xe0a11ffc, 0x00001c6c, 0x009d300d, 0x809540c3, 0x6041fffc,
			0x00001c6c, 0x0340000d, 0x8086c083, 0x6041fffc, 0x00001c6c, 0x00c0017f, 0x8106c080, 0x00a11ffc,
			0x001f9c6c, 0x1000000d, 0x8106c080, 0x0022007c, 0x00001c6c, 0x0183600c, 0x0886c0c3, 0x60405ffe,
			0x00001c6c, 0x0183500c, 0x0886c0c3, 0x60409ffe, 0x00001c6c, 0x0183400c, 0x0886c0c3, 0x60411ffe,
			0x00009c6d, 0x0183600c, 0x0886c0c3, 0x60405ffe, 0x00009c6d, 0x0183500c, 0x0886c0c3, 0x60409ffe,
			0x00009c6d, 0x0183400c, 0x0886c0c3, 0x60411ffe, 0x00011c6e, 0x0183600c, 0x0886c0c3, 0x60405ffe,
			0x00019c6f, 0x0183600c, 0x0886c0c3, 0x60405ffe, 0x00019c6f, 0x0183500c, 0x0886c0c3, 0x60409ffe,
			0x00019c6f, 0x0183400c, 0x0886c0c3, 0x60411ffe, 0x00011c6e, 0x0183500c, 0x0886c0c3, 0x60409ffe,
			0x00011c6e, 0x0183400c, 0x0886c0c3, 0x60411ffe, 0x00019c6c, 0x0080012a, 0x81060343, 0x6041dffc,
			0x00011c6c, 0x01000100, 0x01060243, 0x01a1dffc, 0x00009c6c, 0x01000155, 0x01060143, 0x0121dffc,
			0x00001c6c, 0x0100017f, 0x81060043, 0x00a1dffc, 0x401f9c6c, 0x009d3808, 0x012a80c3, 0x60419f9c,
			0x00001c6c, 0x0080000c, 0x00bfc043, 0x6041dffc, 0x401f9c6c, 0x0180700c, 0x0086c0c3, 0x60403f80,
			0x401f9c6c, 0x0180600c, 0x0086c0c3, 0x60405f80, 0x401f9c6c, 0x0180500c, 0x0086c0c3, 0x60409f80,
			0x401f9c6c, 0x0180400c, 0x0086c0c3, 0x60411f81,
		};

		constexpr u32 s_DDE10DE6375786DF[] = {
			0x00009c6c, 0x005d2000, 0x0186c083, 0x60403ffc, 0x00001c6c, 0x005d2000, 0x0186c083, 0x60403ffc,
			0x00021c6c, 0x03c00770, 0x8106c083, 0x6041fffc, 0x00029c6c, 0x00c00100, 0x0106c08a, 0xa0411ffc,
			0x00001c6c, 0x009d3d0c, 0x013fc0c3, 0x6041dffc, 0x00029c6c, 0x005d307f, 0x8186c083, 0x60409ffc,
			0x00009c6c, 0x009d3c0c, 0x010000c3, 0x6041dffc, 0x00011c6c, 0x00819b00, 0x812ac0c3, 0x60407ffc,
			0x00011c6c, 0x00819a08, 0x010400c3, 0x60419ffc, 0x00019c6c, 0x009d3800, 0x812a80c3, 0x60407ffc,
			0x00019c6c, 0x00818908, 0x012e80c3, 0x60419ffc, 0x401f9c6c, 0x009d3000, 0x86aa80c3, 0x60407f9c,
			0x401f9c6c, 0x0081805d, 0x068400c3, 0x60419f9c, 0x401f9c6c, 0x009d300d, 0x84aa80c3, 0x6041ffa0,
			0x401f9c6c, 0x0081a05d, 0x068400c3, 0x60419fa4, 0x00019c6c, 0x0101300c, 0x010600c3, 0x00a1dffc,
			0x00011c6c, 0x011d2e0d, 0x8115456a, 0xa061fffc, 0x00009c6c, 0x00c0020c, 0x0086c083, 0x0041dffc,
			0x00001c6c, 0x00c00155, 0x0106c080, 0x02a11ffc, 0x00021c6c, 0x009d300d, 0x889540c3, 0x6041fffc,
			0x00001c6c, 0x0340000d, 0x8886c083, 0x6041fffc, 0x00019c6c, 0x00c0017f, 0x8106c080, 0x00203ffc,
			0x00021c6c, 0x0140000c, 0x02860143, 0x60403ffc, 0x00001c6c, 0x1040000c, 0x0486c09f, 0xe1a3c1fc,
			0x00021c6c, 0x01c3600d, 0x8186c043, 0x60405ffe, 0x00021c6c, 0x01c3500d, 0x8186c043, 0x60409ffe,
			0x00021c6c, 0x01c3400d, 0x8186c043, 0x60411ffe, 0x00039c6c, 0x0183600c, 0x0686c0c3, 0x60405ffe,
			0x00039c6c, 0x0183500c, 0x0686c0c3, 0x60409ffe, 0x00039c6c, 0x0183400c, 0x0686c0c3, 0x60411ffe,
			0x00041c6d, 0x0183600c, 0x0686c0c3, 0x60405ffe, 0x00041c6d, 0x0183500c, 0x0686c0c3, 0x60409ffe,
			0x00041c6d, 0x0183400c, 0x0686c0c3, 0x60411ffe, 0x00029c6d, 0x01c3600d, 0x8186c043, 0x60405ffe,
			0x00029c6d, 0x01c3500d, 0x8186c043, 0x60409ffe, 0x00029c6d, 0x01c3400d, 0x8186c043, 0x60411ffe,
			0x00031c6e, 0x01c3600d, 0x8186c043, 0x60405ffe, 0x00031c6e, 0x01c3500d, 0x8186c043, 0x60409ffe,
			0x00031c6e, 0x01c3400d, 0x8186c043, 0x60411ffe, 0x00049c6e, 0x0183600c, 0x0686c0c3, 0x60405ffe,
			0x00049c6e, 0x0183500c, 0x0686c0c3, 0x60409ffe, 0x00049c6e, 0x0183400c, 0x0686c0c3, 0x60411ffe,
			0x00011c6f, 0x0183600c, 0x0686c0c3, 0x60405ffe, 0x00011c6f, 0x0183500c, 0x0686c0c3, 0x60409ffe,
			0x00051c6f, 0x01c3600d, 0x8186c043, 0x60405ffe, 0x00051c6f, 0x01c3500d, 0x8186c043, 0x60409ffe,
			0x00051c6f, 0x01c3400d, 0x8186c043, 0x60411ffe, 0x001f9c6c, 0x2000000d, 0x8106c09f, 0xe230007c,
			0x00011c6f, 0x0183400c, 0x0686c0c3, 0x60411ffe, 0x00059c6c, 0x0080012a, 0x81060a43, 0x6041dffc,
			0x00009c6c, 0x00800000, 0x00860143, 0x6041dffc, 0x00051c6c, 0x0080012a, 0x81060243, 0x6041dffc,
			0x00001c6c, 0x01c3600d, 0x8186c143, 0x60405ffe, 0x00001c6c, 0x01c3500d, 0x8186c143, 0x60409ffe,
			0x00001c6c, 0x01c3400d, 0x8186c143, 0x60411ffe, 0x00011c6d, 0x01c3600d, 0x8186c143, 0x60405ffe,
			0x00011c6d, 0x01c3500d, 0x8186c143, 0x60409ffe, 0x00011c6d, 0x01c3400d, 0x8186c143, 0x60411ffe,
			0x00019c6e, 0x01c3600d, 0x8186c143, 0x60405ffe, 0x00019c6e, 0x01c3500d, 0x8186c143, 0x60409ffe,
			0x00019c6e, 0x01c3400d, 0x8186c143, 0x60411ffe, 0x00061c6f, 0x01c3600d, 0x8186c143, 0x60405ffe,
			0x00061c6f, 0x01c3500d, 0x8186c143, 0x60409ffe, 0x00061c6f, 0x01c3400d, 0x8186c143, 0x60411ffe,
			0x00009c6c, 0x01000100, 0x01060943, 0x0521dffc, 0x00031c6c, 0x01000100, 0x01060643, 0x05a1dffc,
			0x00029c6c, 0x01000155, 0x01060543, 0x0321dffc, 0x00009c6c, 0x01000155, 0x01060843, 0x00a1dffc,
			0x00009c6c, 0x0100017f, 0x81060743, 0x00a1dffc, 0x00021c6c, 0x0100017f, 0x81060443, 0x02a1dffc,
			0x00029c6c, 0x0080012a, 0x81060c43, 0x6041dffc, 0x00019c6c, 0x01000100, 0x01060343, 0x02a1dffc,
			0x00001c6c, 0x0140000c, 0x08860443, 0x60403ffc, 0x00011c6c, 0x01000155, 0x01060243, 0x01a1dffc,
			0x00009c6c, 0x0080000c, 0x02bfc343, 0x6041dffc, 0x401f9c6c, 0x0180700c, 0x0286c0c3, 0x60403f80,
			0x401f9c6c, 0x0180600c, 0x0286c0c3, 0x60405f80, 0x401f9c6c, 0x0180500c, 0x0286c0c3, 0x60409f80,
			0x00011c6c, 0x0100017f, 0x81060043, 0x0121dffc, 0x401f9c6c, 0x0180400c, 0x0286c0c3, 0x60411f80,
			0x001f9c6c, 0x2000000d, 0x8106c09f, 0xe02200fc, 0x00001c6c, 0x0140000c, 0x04860243, 0x60403ffc,
			0x00001c6c, 0x00c0a08c, 0x0286c083, 0x0061dffc, 0x00019c6c, 0x2080007f, 0x8286045f, 0xe023c07c,
			0x00011c6c, 0x0080007f, 0x80860243, 0x6041dffc, 0x00001c6c, 0x0140000c, 0x04860243, 0x60403ffc,
			0x00021c6c, 0x00800043, 0x04984343, 0x6041dffc, 0x00019c6c, 0x01000030, 0x84a18363, 0x0221dffc,
			0x00009c6c, 0x00c0208c, 0x0286c083, 0x0061dffc, 0x00019c6c, 0x2080000c, 0x06bfc25f, 0xe023c07c,
			0x00009c6c, 0x0140000c, 0x06860343, 0x60403ffc, 0x00011c6c, 0x0080007f, 0x80860243, 0x6041dffc,
			0x401f9c6c, 0x0140000c, 0x02860243, 0x60405fa8, 0x401f9c6c, 0x2141200c, 0x048600df, 0xe0a2402c,
			0x401f9c6c, 0x0140000c, 0x04860043, 0x60405fb0, 0x00019c6c, 0x0080007f, 0x80860343, 0x6041dffc,
			0x401f9c6c, 0x0140000c, 0x02860343, 0x60409fa8, 0x401f9c6c, 0x0141200c, 0x068600c3, 0x60409fac,
			0x00021c6c, 0x0080000c, 0x06bfc243, 0x6041dffc, 0x00029c6c, 0x00800030, 0x84a18443, 0x6041dffc,
			0x00011c6c, 0x01000043, 0x04984463, 0x02a1dffc, 0x401f9c6c, 0x0140000c, 0x06860043, 0x60409fb0,
			0x401f9c6c, 0x0140000c, 0x02860243, 0x60411fa8, 0x401f9c6c, 0x0141200c, 0x048600c3, 0x60411fac,
			0x401f9c6c, 0x0140000c, 0x04860043, 0x60411fb1,
		};

		constexpr u32 s_FEA9FD1B5EB207B4[] = {
			0x00001c6c, 0x03c00770, 0x8106c083, 0x6041fffc, 0x00009c6c, 0x00c00100, 0x0106c08a, 0xa0403ffc,
			0x00009c6c, 0x009d3c0c, 0x010000c3, 0x6041dffc, 0x00021c6c, 0x00c0000c, 0x0286c083, 0x0041dffc,
			0x00009c6c, 0x00c00155, 0x0106c09f, 0xe0a11ffc, 0x00001c6c, 0x009d300d, 0x809540c3, 0x6041fffc,
			0x00001c6c, 0x0340000d, 0x8086c083, 0x6041fffc, 0x00001c6c, 0x00c0017f, 0x8106c080, 0x00a11ffc,
			0x001f9c6c, 0x1000000d, 0x8106c080, 0x0022007c, 0x00001c6c, 0x0183600c, 0x0886c0c3, 0x60405ffe,
			0x00001c6c, 0x0183500c, 0x0886c0c3, 0x60409ffe, 0x00001c6c, 0x0183400c, 0x0886c0c3, 0x60411ffe,
			0x00009c6d, 0x0183600c, 0x0886c0c3, 0x60405ffe, 0x00009c6d, 0x0183500c, 0x0886c0c3, 0x60409ffe,
			0x00009c6d, 0x0183400c, 0x0886c0c3, 0x60411ffe, 0x00011c6e, 0x0183600c, 0x0886c0c3, 0x60405ffe,
			0x00019c6f, 0x0183600c, 0x0886c0c3, 0x60405ffe, 0x00019c6f, 0x0183500c, 0x0886c0c3, 0x60409ffe,
			0x00019c6f, 0x0183400c, 0x0886c0c3, 0x60411ffe, 0x00011c6e, 0x0183500c, 0x0886c0c3, 0x60409ffe,
			0x00011c6e, 0x0183400c, 0x0886c0c3, 0x60411ffe, 0x00019c6c, 0x0080012a, 0x81060343, 0x6041dffc,
			0x00011c6c, 0x01000100, 0x01060243, 0x01a1dffc, 0x00009c6c, 0x01000155, 0x01060143, 0x0121dffc,
			0x00001c6c, 0x0100017f, 0x81060043, 0x00a1dffc, 0x00009c6c, 0x0080000c, 0x00bfc043, 0x6041dffc,
			0x00011c6c, 0x00c0a00c, 0x0286c0a3, 0x0061dffc, 0x00001c6c, 0x00400900, 0x8106c083, 0x60407ffc,
			0x00009c6c, 0x0140000c, 0x04860243, 0x60403ffc, 0x00001c6c, 0x00400808, 0x0106c083, 0x60419ffc,
			0x401f9c6c, 0x009d300d, 0x80aa80c3, 0x6041ff9c, 0x001f9c6c, 0x2000000d, 0x8106c09f, 0xe0b0007c,
			0x401f9c6c, 0x0180700c, 0x0286c0c3, 0x60403f80, 0x401f9c6c, 0x1180600c, 0x0286c0c0, 0x00304000,
			0x401f9c6c, 0x0180500c, 0x0286c0c3, 0x60409f80, 0x00001c6c, 0x00c0e000, 0x0086c0a8, 0xa0619ffc,
			0x401f9c6c, 0x0180400c, 0x0286c0c3, 0x60411f80, 0x441f9c6c, 0x0080e022, 0x80ae80c3, 0x60419fa1,
		};

		template <usz N>
		RSXVertexProgram program(const u32 (&words)[N])
		{
			RSXVertexProgram vp{};
			vp.data.assign(words, words + N);
			return vp;
		}


		struct saved_slots
		{
			u32 slots[15] = { 19, 466, 467, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63 };
			u32 values[15][4]{};
			saved_slots()
			{
				for (u32 i = 0; i < 15; ++i)
					for (u32 c = 0; c < 4; ++c) values[i][c] = rsx::method_registers.transform_constants[slots[i]][c];
			}
			~saved_slots()
			{
				for (u32 i = 0; i < 15; ++i)
					for (u32 c = 0; c < 4; ++c) rsx::method_registers.transform_constants[slots[i]][c] = values[i][c];
			}
		};

		void write_slot(u32 slot, f32 x, f32 y, f32 z, f32 w)
		{
			const f32 values[4] = { x, y, z, w };
			for (u32 c = 0; c < 4; ++c)
				rsx::method_registers.transform_constants[slot][c] = std::bit_cast<u32>(values[c]);
		}

		void expect_reference(const RSXVertexProgram& vp, bool morph, bool base_scaled)
		{
			saved_slots restore;
			const auto fp = scan_vertex_program(vp);
			ASSERT_TRUE(fp.skinned) << fp.note;
			ASSERT_FALSE(fp.skin_unrecognised) << fp.skin_note;
			ASSERT_EQ(morph, fp.has_skin_morph);
			ASSERT_EQ(base_scaled, fp.skin_morph_base_scaled);
			EXPECT_FALSE(fp.has_prescale);
			EXPECT_FALSE(fp.has_const_affine);

			write_slot(19, 2.f, 0.5f, 1.5f, 17.f);
			write_slot(467, morph ? 0.375f : 3.f, 3.f, 1.f, 1.f);
			f32 rows[4][3][4]{};
			for (u32 bone = 0; bone < 4; ++bone)
			{
				for (u32 row = 0; row < 3; ++row)
				{
					for (u32 axis = 0; axis < 3; ++axis)
						rows[bone][row][axis] = axis == row ? 1.f + 0.2f * bone : 0.05f * (axis + row + 1);
					rows[bone][row][3] = static_cast<f32>(bone * 2 + row) - 2.f;
					write_slot(52 + bone * 3 + row, rows[bone][row][0], rows[bone][row][1], rows[bone][row][2], rows[bone][row][3]);
				}
			}

			const f32 base[4] = { 1.25f, -2.f, 3.f, 123.f };
			const f32 delta[4] = { 0.75f, 1.5f, -0.5f, -456.f };
			const f32 indices[4] = { 0.25f, 1.25f, 2.25f, 3.25f };
			const f32 weights[4] = { 0.2f, 0.7f, 0.3f, 0.9f }; // sum 2.1, not one
			const f32 sum = weights[0] + weights[1] + weights[2] + weights[3];
			f32 point[4] = { base[0], base[1], base[2], 1.f };
			if (morph)
			{
				skin_morph_decode decode{};
				ASSERT_TRUE(read_skin_morph_decode(fp, decode));
				ASSERT_TRUE(evaluate_skin_morph_position(decode, base, delta, point));
			}

			f32 actual[3]{}, expected[3]{};
			const f32 scale[3] = { 2.f, 0.5f, 1.5f };
			for (u32 k = 0; k < 4; ++k)
			{
				u32 slot = 0;
				ASSERT_TRUE(evaluate_palette_slot(fp, fp.blend_bone[k], indices[fp.blend_bone[k].component], slot));
				EXPECT_EQ(52u + k * 3, slot);
				mat4 matrix{};
				ASSERT_TRUE(build_palette_matrix(fp, slot, matrix));
				for (u32 row = 0; row < 3; ++row)
				{
					f32 hardware = rows[k][row][3];
					f32 replay = matrix.m[3][row];
					for (u32 axis = 0; axis < 3; ++axis)
					{
						// Independent DPH reference: form the guest point, then dot the raw
						// constant row. No replay helper or transposed matrix is used here.
						const f32 source = base[axis] * (base_scaled ? scale[axis] : 1.f)
							+ (morph ? delta[axis] * 0.375f : 0.f);
						hardware += source * rows[k][row][axis];
						replay += point[axis] * matrix.m[axis][row];
					}
					actual[row] += replay * weights[fp.blend_weight_component[k]] / sum;
					expected[row] += hardware * weights[k] / sum;
				}
			}
			for (u32 axis = 0; axis < 3; ++axis) EXPECT_NEAR(expected[axis], actual[axis], 1e-5f);
		}

		void replace_bits(RSXVertexProgram& vp, u32 instruction, u32 word, u32 mask, u32 bits)
		{
			u32& raw = vp.data[instruction * 4 + word];
			raw = (raw & ~mask) | bits;
		}

		void expect_refused(const RSXVertexProgram& vp)
		{
			const auto fp = scan_vertex_program(vp);
			EXPECT_TRUE(!fp.skinned || fp.skin_unrecognised || !fp.bone_resolved);
			EXPECT_FALSE(fp.has_skin_morph);
		}

		void expect_skin(const RSXVertexProgram& vp)
		{
			const auto fp = scan_vertex_program(vp);
			EXPECT_TRUE(fp.skinned) << fp.note;
			EXPECT_TRUE(fp.skin_vertex_blend) << fp.note;
			EXPECT_FALSE(fp.skin_unrecognised) << fp.skin_note;
			EXPECT_TRUE(fp.inner_is_input) << fp.note;
			EXPECT_TRUE(fp.bone_resolved);
			EXPECT_EQ(4u, fp.blend_bones);
			EXPECT_EQ(52u, fp.palette_base);
			EXPECT_EQ(3u, fp.palette_rows);
			EXPECT_TRUE(fp.palette_implicit_w);
			EXPECT_EQ(1u, fp.blend_weight_attribute);
		}
	}

	TEST(RemixSR2Skin, RawF7FA)
	{
		expect_skin(program(s_F7FA79C9F2466421));
	}

	TEST(RemixSR2Skin, RawF1E1)
	{
		expect_skin(program(s_F1E164AF57003605));
	}

	TEST(RemixSR2Skin, Raw527C)
	{
		expect_skin(program(s_527C1166C9EF61E5));
	}

	TEST(RemixSR2Skin, Raw855E)
	{
		expect_skin(program(s_855E0E3EE97C8DD9));
	}

	TEST(RemixSR2Skin, NormalMorphIsProvenSeparatelyFromPositionMorph)
	{
		for (const auto& vp : { program(s_F7FA79C9F2466421), program(s_F1E164AF57003605), program(s_DDE10DE6375786DF) })
		{
			expect_skin(vp);
			const auto fp = scan_vertex_program(vp);
			EXPECT_TRUE(fp.has_skin_morph);
			EXPECT_EQ(skin_normal_form::morph, fp.skin_normal);
		}
		for (const auto& vp : { program(s_FEA9FD1B5EB207B4), program(s_855E0E3EE97C8DD9) })
		{
			expect_skin(vp);
			const auto fp = scan_vertex_program(vp);
			EXPECT_TRUE(fp.has_skin_morph);
			EXPECT_EQ(skin_normal_form::base_only, fp.skin_normal);
			skin_normal_morph_decode decode{};
			EXPECT_FALSE(read_skin_normal_morph_decode(fp, decode));
		}
	}

	TEST(RemixSR2Skin, LiveNormalMorphUsesWNotPositionXOrC19)
	{
		saved_slots restore;
		const auto fp = scan_vertex_program(program(s_F1E164AF57003605));
		ASSERT_EQ(skin_normal_form::morph, fp.skin_normal);
		write_slot(466, 0.f, 1.f, 0.f, 0.f);
		write_slot(19, 2.f, 0.5f, 1.5f, 0.f);
		write_slot(467, 0.25f, 3.f, 1.f, 0.5f);
		const f32 base[4] = { 0.f, 0.f, 1.f, 99.f };
		const f32 delta[4] = { 1.f, -2.f, 0.5f, -99.f };
		skin_normal_morph_decode decode{};
		f32 first[4]{}, second[4]{}, third[4]{};
		ASSERT_TRUE(read_skin_normal_morph_decode(fp, decode));
		ASSERT_TRUE(evaluate_skin_normal_morph(decode, base, delta, first));
		const f32 reference[3] = { 0.5f, -1.f, 1.25f };
		const f32 length = std::sqrt(0.5f * 0.5f + 1.f + 1.25f * 1.25f);
		for (u32 axis = 0; axis < 3; ++axis) EXPECT_NEAR(reference[axis] / length, first[axis], 1e-6f);
		EXPECT_FLOAT_EQ(0.f, first[3]);

		// Position-only values must not leak into the normal decode, even if unreadable.
		write_slot(19, std::numeric_limits<f32>::quiet_NaN(), 7.f, -3.f, 0.f);
		write_slot(467, std::numeric_limits<f32>::quiet_NaN(), 3.f, 1.f, 0.5f);
		ASSERT_TRUE(read_skin_normal_morph_decode(fp, decode));
		ASSERT_TRUE(evaluate_skin_normal_morph(decode, base, delta, second));
		for (u32 axis = 0; axis < 4; ++axis) EXPECT_FLOAT_EQ(first[axis], second[axis]);
		write_slot(467, 0.25f, 3.f, 1.f, -0.25f);
		ASSERT_TRUE(read_skin_normal_morph_decode(fp, decode));
		ASSERT_TRUE(evaluate_skin_normal_morph(decode, base, delta, third));
		EXPECT_NE(first[0], third[0]);
		EXPECT_FLOAT_EQ(0.f, third[3]);
	}

	TEST(RemixSR2Skin, ZeroNormalMorphAndMissingOrInvalidDelta)
	{
		saved_slots restore;
		const auto fp = scan_vertex_program(program(s_F7FA79C9F2466421));
		ASSERT_EQ(skin_normal_form::morph, fp.skin_normal);
		write_slot(466, 0.f, 1.f, 0.f, 0.f);
		write_slot(467, 0.75f, 3.f, 1.f, 0.f);
		skin_normal_morph_decode decode{};
		ASSERT_TRUE(read_skin_normal_morph_decode(fp, decode));
		const f32 base[4] = { 0.6f, 0.8f, 0.f, 17.f };
		const f32 delta[4] = { 3.f, -2.f, 5.f, 29.f };
		f32 actual[4] = { 11.f, 12.f, 13.f, 14.f };
		ASSERT_TRUE(evaluate_skin_normal_morph(decode, base, delta, actual));
		for (u32 axis = 0; axis < 3; ++axis) EXPECT_NEAR(base[axis], actual[axis], 1e-6f);
		EXPECT_FLOAT_EQ(0.f, actual[3]);
		EXPECT_FALSE(evaluate_skin_normal_morph(decode, base, nullptr, actual));
		const f32 bad_delta[4] = { std::numeric_limits<f32>::infinity(), 0.f, 0.f, 0.f };
		EXPECT_FALSE(evaluate_skin_normal_morph(decode, base, bad_delta, actual));
		EXPECT_NEAR(0.6f, actual[0], 1e-6f); // no partial output on refusal
		const f32 zero[4]{};
		EXPECT_FALSE(evaluate_skin_normal_morph(decode, zero, delta, actual));
		write_slot(466, 1.f, 1.f, 0.f, 0.f); // guest DP4 would translate this, not rotate a direction
		EXPECT_FALSE(read_skin_normal_morph_decode(fp, decode));
		write_slot(466, 0.f, 1.f, 0.f, 0.f);
		write_slot(467, 0.75f, 3.f, 1.f, std::numeric_limits<f32>::infinity());
		EXPECT_FALSE(read_skin_normal_morph_decode(fp, decode));
	}

	TEST(RemixSR2Skin, UnsupportedNormalMorphCannotGuessBaseNormal)
	{
		auto check = [](const RSXVertexProgram& vp)
		{
			expect_skin(vp); // normal refusal must not disturb the proven position path
			const auto fp = scan_vertex_program(vp);
			EXPECT_TRUE(fp.has_skin_morph);
			EXPECT_EQ(skin_normal_form::refused, fp.skin_normal);
			skin_normal_morph_decode decode{};
			EXPECT_FALSE(read_skin_normal_morph_decode(fp, decode));
		};
		auto vp = program(s_F7FA79C9F2466421);
		replace_bits(vp, 5, 1, 0xf00u, 14u << 8); // not normal delta attr13
		check(vp);
		vp = program(s_F7FA79C9F2466421);
		replace_bits(vp, 5, 2, 0xffu << 14, 0u); // c467.x, not its independent w coefficient
		check(vp);
		vp = program(s_F7FA79C9F2466421);
		replace_bits(vp, 5, 0, 0u, 1u << 21); // abs(delta)
		check(vp);
		vp = program(s_F7FA79C9F2466421);
		replace_bits(vp, 5, 0, 0u, 1u << 26); // saturated morph
		check(vp);
		vp = program(s_F7FA79C9F2466421);
		replace_bits(vp, 5, 0, 0u, 1u << 27); // indexed delta input
		check(vp);
		vp = program(s_F7FA79C9F2466421);
		replace_bits(vp, 10, 1, 0xf00u, 3u << 8); // not base normal attr2
		check(vp);
		vp = program(s_F7FA79C9F2466421);
		replace_bits(vp, 15, 2, 0x3fu << 8, 0u); // DP3 length reads a different temp
		check(vp);
		vp = program(s_F7FA79C9F2466421);
		replace_bits(vp, 40, 1, 0x1fu << 27, RSX_SCA_OPCODE_RCP << 27); // not reciprocal sqrt
		check(vp);
		vp = program(s_F7FA79C9F2466421);
		replace_bits(vp, 42, 0, 0x3fu << 15, 1u << 15); // normal input changed after its DP3
		check(vp);
		vp = program(s_F7FA79C9F2466421);
		replace_bits(vp, 45, 2, 0x3fu << 8, 0u); // one normal row reads the tangent temp instead
		check(vp);
		vp = program(s_F7FA79C9F2466421);
		replace_bits(vp, 48, 0, 3u, 0u); // duplicate a0.x row instead of a0.y
		check(vp);
		vp = program(s_F7FA79C9F2466421);
		const u32 duplicate[4] = { vp.data[45 * 4], vp.data[45 * 4 + 1], vp.data[45 * 4 + 2], vp.data[45 * 4 + 3] };
		vp.data.insert(vp.data.begin() + 57 * 4, std::begin(duplicate), std::end(duplicate));
		check(vp); // an extra duplicate must be refused even when all twelve row bits remain present
		vp = program(s_F7FA79C9F2466421);
		replace_bits(vp, 1, 1, 0xffu, 0x2au);
		replace_bits(vp, 1, 2, 0u, 1u << 31); // normal homogeneous W is c466.y, not c466.x
		check(vp);
	}

	TEST(RemixSR2Skin, NonzeroMorphAndNonunitWeightsMatchIndependentDPH)
	{
		expect_reference(program(s_F7FA79C9F2466421), true, false);
		expect_reference(program(s_F1E164AF57003605), true, true);
		expect_reference(program(s_855E0E3EE97C8DD9), true, false);
		expect_reference(program(s_527C1166C9EF61E5), false, false);
	}

	TEST(RemixSR2Skin, LiveMorphCoefficientsChangeSubmittedPosition)
	{
		saved_slots restore;
		const auto fp = scan_vertex_program(program(s_F1E164AF57003605));
		ASSERT_TRUE(fp.has_skin_morph);
		const f32 base[4] = { 2.f, 3.f, 4.f, 100.f };
		const f32 delta[4] = { 1.f, -2.f, 3.f, 200.f };
		f32 first[4]{}, second[4]{}, third[4]{};
		skin_morph_decode decode{};
		write_slot(19, 2.f, 0.5f, 1.5f, 0.f);
		write_slot(467, 0.25f, 3.f, 0.f, 0.f);
		ASSERT_TRUE(read_skin_morph_decode(fp, decode));
		ASSERT_TRUE(evaluate_skin_morph_position(decode, base, delta, first));
		write_slot(467, 0.75f, 3.f, 0.f, 0.f);
		ASSERT_TRUE(read_skin_morph_decode(fp, decode));
		ASSERT_TRUE(evaluate_skin_morph_position(decode, base, delta, second));
		write_slot(19, 3.f, 0.5f, 1.5f, 0.f);
		ASSERT_TRUE(read_skin_morph_decode(fp, decode));
		ASSERT_TRUE(evaluate_skin_morph_position(decode, base, delta, third));
		EXPECT_NE(std::bit_cast<u32>(first[0]), std::bit_cast<u32>(second[0]));
		EXPECT_NE(std::bit_cast<u32>(second[0]), std::bit_cast<u32>(third[0]));
		EXPECT_FLOAT_EQ(4.25f, first[0]);
		EXPECT_FLOAT_EQ(4.75f, second[0]);
		EXPECT_FLOAT_EQ(6.75f, third[0]);
		EXPECT_FLOAT_EQ(1.f, third[3]);
		write_slot(467, std::numeric_limits<f32>::infinity(), 3.f, 0.f, 0.f);
		EXPECT_FALSE(read_skin_morph_decode(fp, decode));
		write_slot(467, 0.75f, 3.f, 0.f, 0.f);
		write_slot(19, std::numeric_limits<f32>::quiet_NaN(), 0.5f, 1.5f, 0.f);
		EXPECT_FALSE(read_skin_morph_decode(fp, decode));
		f32 bad_delta[4] = { std::numeric_limits<f32>::infinity(), 0.f, 0.f, 0.f };
		EXPECT_FALSE(evaluate_skin_morph_position(decode, base, bad_delta, third));
		EXPECT_FLOAT_EQ(6.75f, third[0]); // refusal does not partially modify the submitted position
	}

	TEST(RemixSR2Skin, UnmodeledMorphInputsAndOperationsStayRefused)
	{
		auto vp = program(s_855E0E3EE97C8DD9);
		replace_bits(vp, 3, 1, 0xf00u, 11u << 8); // attr11 instead of attr12
		expect_refused(vp);
		vp = program(s_855E0E3EE97C8DD9);
		replace_bits(vp, 3, 1, 0x3ff000u, 466u << 12); // unmodeled coefficient slot
		expect_refused(vp);
		vp = program(s_855E0E3EE97C8DD9);
		replace_bits(vp, 3, 0, 0u, 1u << 21); // abs(attr12)
		expect_refused(vp);
		vp = program(s_855E0E3EE97C8DD9);
		replace_bits(vp, 3, 0, 0u, 1u << 26); // saturation
		expect_refused(vp);
		vp = program(s_F1E164AF57003605);
		replace_bits(vp, 7, 1, 0xf00u, 11u << 8); // foreign morph input cannot be dropped by affine fallback
		expect_refused(vp);
		vp = program(s_F1E164AF57003605);
		replace_bits(vp, 8, 1, 0x3ff000u, 20u << 12); // not optional c19 base scale
		expect_refused(vp);
		vp = program(s_855E0E3EE97C8DD9);
		replace_bits(vp, 3, 1, 3u << 3, 0u); // src0.y becomes x: nonidentity xyz
		expect_refused(vp);
		vp = program(s_855E0E3EE97C8DD9);
		replace_bits(vp, 3, 2, 3u << 18, 1u << 18); // c467.y in the morph's y lane
		expect_refused(vp);
	}

	TEST(RemixSR2Skin, ConsumedScalarLaneAndBoneSourceChangesStayRefused)
	{
		auto vp = program(s_527C1166C9EF61E5);
		replace_bits(vp, 27, 3, 1u << 17, 1u << 18); // live RSQ writes z, not unused w
		expect_refused(vp);
		vp = program(s_855E0E3EE97C8DD9);
		replace_bits(vp, 9, 1, 3u << 3, 0u); // a bone DPH source permutes y to x
		expect_refused(vp);
		vp = program(s_F7FA79C9F2466421);
		replace_bits(vp, 21, 0, 0x3fu << 15, 3u << 15); // overwrite morph temp between bone rows
		expect_refused(vp);
	}


	TEST(RemixSR2Skin, ArbitraryPostBlendScaleIsNotWeightNormalization)
	{
		auto vp = program(s_F7FA79C9F2466421);
		replace_bits(vp, 16, 1, 0x1fu << 27, RSX_SCA_OPCODE_RSQ << 27);
		expect_refused(vp);
		vp = program(s_F1E164AF57003605);
		replace_bits(vp, 18, 1, 0x1fu << 27, RSX_SCA_OPCODE_RSQ << 27);
		expect_refused(vp);
		vp = program(s_527C1166C9EF61E5);
		replace_bits(vp, 6, 1, 0x1fu << 27, RSX_SCA_OPCODE_RSQ << 27);
		expect_refused(vp);
		vp = program(s_855E0E3EE97C8DD9);
		replace_bits(vp, 8, 1, 0x1fu << 27, RSX_SCA_OPCODE_RSQ << 27);
		expect_refused(vp);
		vp = program(s_855E0E3EE97C8DD9);
		replace_bits(vp, 8, 1, 0x1fu << 27, RSX_SCA_OPCODE_MOV << 27);
		expect_refused(vp);
		vp = program(s_527C1166C9EF61E5);
		replace_bits(vp, 6, 1, 0x1fu << 27, RSX_SCA_OPCODE_MOV << 27);
		expect_refused(vp);
		vp = program(s_855E0E3EE97C8DD9);
		replace_bits(vp, 4, 1, 0xf00u, 2u << 8); // sum reads a different attribute
		expect_refused(vp);
		vp = program(s_855E0E3EE97C8DD9);
		replace_bits(vp, 4, 1, 0xffu, 0u); // repeated x instead of z in sum
		replace_bits(vp, 4, 2, 1u << 31, 0u); // complete the xxxx source swizzle
		expect_refused(vp);
	}

	TEST(RemixSR2Skin, DirectBoneInputMustBeSubmittedPositionAttribute)
	{
		auto vp = program(s_855E0E3EE97C8DD9);
		for (u32 instruction = 9; instruction <= 20; ++instruction)
		{
			replace_bits(vp, instruction, 2, 3u << 23, 2u << 23); // source0 becomes INPUT
			replace_bits(vp, instruction, 1, 0xf00u, 11u << 8);
		}
		expect_refused(vp);
	}


	TEST(RemixSR2Skin, ForwardedPositionMustBePlainAndUnconditional)
	{
		auto plain = program(s_855E0E3EE97C8DD9);
		for (u32 word = 0; word < 4; ++word) plain.data[3 * 4 + word] = plain.data[2 * 4 + word];
		replace_bits(plain, 3, 0, 0x3fu << 15, 4u << 15); // MOV r4 <- attr0
		replace_bits(plain, 3, 3, 0u, 1u << 13); // full xyzw MOV, the unsafe legacy resolver's case
		expect_skin(plain);
		auto vp = plain;
		replace_bits(vp, 3, 0, 0u, 1u << 21);
		expect_refused(vp);
		vp = plain;
		replace_bits(vp, 3, 1, 0u, 1u << 7); // -attr0
		expect_refused(vp);
		vp = plain;
		replace_bits(vp, 3, 1, 3u << 3, 0u); // nonidentity source xyz
		expect_refused(vp);
		vp = plain;
		replace_bits(vp, 3, 0, 0u, 1u << 26);
		expect_refused(vp);
		vp = plain;
		replace_bits(vp, 3, 0, 7u << 10, 1u << 13); // predicated MOV
		expect_refused(vp);
	}

	TEST(RemixSR2Skin, IndexedWeightInputStaysRefused)
	{
		auto vp = program(s_855E0E3EE97C8DD9);
		replace_bits(vp, 21, 0, 0u, 1u << 27);
		expect_refused(vp);
	}

}

#endif
