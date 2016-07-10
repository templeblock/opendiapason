#ifndef BUFCVT_H
#define BUFCVT_H

#define BUFCVT_FMT_FLOAT (0)
#define BUFCVT_FMT_SLE16 (1)
#define BUFCVT_FMT_SLE24 (2)


void bufcvt_deinterleave(void *dest, size_t dest_stride, const void *src, unsigned length, unsigned nbuf, int infmt, int outfmt)
{
	if (infmt == BUFCVT_FMT_SLE16 && outfmt == BUFCVT_FMT_FLOAT)
	{
		if (nbuf == 2) {
			unsigned j;
			float *out1 = dest;
			float *out2 = out1 + dest_stride;
			for (j = 0; j < length; j++) {
				int_fast32_t s1;
				int_fast32_t s2;
				s1 =             ((const unsigned char *)src)[4*j+1];
				s1 = (s1 << 8) | ((const unsigned char *)src)[4*j+0];
				s2 =             ((const unsigned char *)src)[4*j+3];
				s2 = (s2 << 8) | ((const unsigned char *)src)[4*j+2];
				if (s1 >= 32768)
					s1 -= 65536;
				if (s2 >= 32768)
					s2 -= 65536;
				out1[j] = s1 * (256.0f / (float)0x800000);
				out2[j] = s2 * (256.0f / (float)0x800000);
			}
		} else {
			unsigned i;
			float *out = dest;
			for (i = 0; i < nbuf; i++, out += dest_stride) {
				unsigned j;
				for (j = 0; j < length; j++) {
					uint_fast32_t s;
					int_fast32_t t;
					s =            ((const unsigned char *)src)[2*(i+nbuf*j)+1];
					s = (s << 8) | ((const unsigned char *)src)[2*(i+nbuf*j)+0];
					t = (s & 0x8000u)   ? -(int_fast32_t)(((~s) & 0x7FFFu) + 1) : (int_fast32_t)s;
					out[j] = t * (256.0f / (float)0x800000);
				}
			}
		}
	}
	else if (infmt == BUFCVT_FMT_SLE24 && outfmt == BUFCVT_FMT_FLOAT)
	{
		unsigned i;
		if (nbuf == 2) {
			float *out1 = dest;
			float *out2 = out1 + dest_stride;
			for (i = 0; i < length; i++, src += 6) {
				int_fast32_t t1 = cop_ld_sle24(((const unsigned char *)src) + 0);
				int_fast32_t t2 = cop_ld_sle24(((const unsigned char *)src) + 3);
				out1[i]         = t1 * (1.0f / (float)0x800000);
				out2[i]         = t2 * (1.0f / (float)0x800000);
			}
		} else {
			float *out = dest;
			for (i = 0; i < nbuf; i++, out += dest_stride) {
				unsigned j;
				for (j = 0; j < length; j++) {
					out[j] = cop_ld_sle24(((const unsigned char *)src) + 3 * (i + nbuf * j)) * (1.0f / (float)0x800000);
				}
			}
		}
	}
	else
	{
		abort();
	}
}

#endif /* BUFCVT_H */
