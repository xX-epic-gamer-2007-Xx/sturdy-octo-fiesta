#include "bcimgview-core1"

/* Compressed data from a BCFLAT file is buffered up to FLATBUF (100)
   bytes at once. The compressed data is variable-length, so we can't
   tell how much to read for each row in advance. 100 bytes is a
   compromise between not having to do too many short reads, and not
   wasting too much time moving data down in the buffer. (We could
   save data movement in the buffer by using circular indexing, but
   that would make the code more complex.) To decide how much space to
   keep for uncompressed data, we need to keep track of the maximum
   factor by which the size of data can increase as it is
   decompressed. */
#define FLATBUF 100
#define EXPANSION 8

/* In the BCFLAT image format, each sample after the first in a row is
   represented as a difference relative to the pixel before it. The
   differences are encoded with codewords that range from 3 to 13 bits
   long, and represent 1-8 bytes each. Because most other operations
   on data use 8-bit words, we need a buffer to hold bits that were
   left over from a previous codeword. It's easiest if this buffer can
   hold at least (13 - 1) + 8 = 24 bits, so 32 bits is enough; we
   assume an int holds at least 32 bits. This buffer is managed
   somewhat like a shift register in that the next bits to parse are
   at the most significant position; bits move left through the
   register as they are processed. "reg" represents the register
   contents, while "reg_size" keeps track of how many positions are in
   use. */
struct flat_decode_state {
    unsigned int reg;   /* shift register of codeword bits */
    int reg_size;       /* number of bits in the register */
    unsigned char last; /* previous pixel value */
};

/* Summary of BCFLAT codewords:

   000            2 samples: 0, 0        expansion 5.33
   0010           3 samples: 0, 0, 0     expansion 6
   00110          4 samples: 0, 0, 0, 0  expansion 6.4
   0011100        5 samples, all 0       expansion 5.71
   0011101        6 samples, all 0       expansion 6.86
   0011110        7 samples, all 0       expansion 8

   0100xx         2 samples, +1..+4 x2   expansion 2.66
   0101xx         2 samples, -1..-4 x2   expansion 2.66
   0110xx         3 samples, +1..+4 x3   expansion 4
   0111xx         3 samples, -1..-4 x3   expansion 4

   100            one sample, 0 difference   expansion 2.66
   1010           one sample, difference +1  expansion 2
   1011           one sample, difference -1  expansion 2
   11000x         one sample, difference +2, +3
   11001x         one sample, difference -2, -3
   11010xx        one sample, difference +4...+7
   11011xx        one sample, difference -4...-7
   1110000xxx     one sample, difference +8...+15
   1110001xxx     one sample, difference -8...-15
   1110010xxxx    one sample, difference +16...+31
   1110011xxxx    one sample, difference -16...-31
   1110100xxxxx   one sample, difference +32...+63
   1110101xxxxx   one sample, difference -32...-63
   1110110xxxxxx  one sample, difference +64...+127
   1110111xxxxxx  one sample, difference -64...-127
   1111000        one sample, difference (+/-)128

   Notice that no codeword is a prefix of any other codeword. This is
   what allows a sequence of variable-length codes to be uniquely
   decoded without delimiters.
*/

/* This function represents the inner loop of decoding flat-compressed
   samples. It reads compressed data from "comp_p" and writes
   corresponding uncompressed data to "uncomp_p", updating the
   decompression state "state" accordingly. On entrance
   "comp_size_inout" should point to the number of bytes of compressed
   data that comp_p points to, and on exit is updated to point to the
   number of compressed bytes consumed. On entrance pixels_inout
   should point to the maximum number of pixels that should be
   decompressed (i.e., before the end of a row), and on exit it will
   hold the number that were actually decompressed. */
void decode_flat(unsigned char *comp_p, int *comp_size_inout,
                 unsigned char *uncomp_p, int *pixels_inout,
                 struct flat_decode_state *state) {
    /* compressed data buffer, dynamically allocated */
    unsigned char *comp_buf;
    /* decompressed data buffer */
    unsigned char uncomp_buf[EXPANSION * (FLATBUF + 1)];
    unsigned char last = state->last;
    unsigned int reg = state->reg;
    unsigned int reg_size = state->reg_size;
    unsigned char *p, *q;
    int max_pixels = *pixels_inout;
    int num_pixels = 0;
    int comp_size = *comp_size_inout;
    /* If non-zero, some of the bits in reg_size were from padding
       bytes, and so should not be used for decoding. */
    int padding_bits = 0;

    /* Pad the compressed input with two extra bytes, so that we don't
       read ahead into undefined data */
    assert(comp_size <= FLATBUF);
    comp_buf = xmalloc(FLATBUF + 2);
    memcpy(comp_buf, comp_p, comp_size);
    comp_buf[comp_size] = 0x00;
    comp_buf[comp_size + 1] = 0x00;

    p = comp_buf;
    q = uncomp_buf;

    while (num_pixels < max_pixels && p < comp_buf + comp_size + 2) {
        int codelen;
        assert(num_pixels >= 0);
        /* Read compressed data into the register until we have
           enough to cover any codeword */
        while (reg_size < 13) {
            /* We might read up to two bytes beyond the supplied
               compressed data. The bits from these bytes go into the
               shift register, but if it looks like we're about to
               match a codeword including them, we stop. */
            if (p >= comp_buf + comp_size)
                padding_bits += 8;
            reg |= (*p++) << ((32 - 8) - reg_size);
            reg_size += 8;
        }
        /* The next long tree of branches covers all the different
           possible code words. It may look intimidating, but a lot of
           the cases are similar to one another, and you can match
           them back up with the table of codewords in the comment
           above. Another possibility would be to implement this with
           a lookup table, but with 2**13 = 8182 entries it would have
           to be pretty big itself. */
        if ((reg & 0xc0000000) == 0) {
            /* 00... repeated 0 differences, i.e. repeated pixel */
            int num_zeros, i;
            if ((reg & 0xe0000000) == 0) {
                /* 000 */
                num_zeros = 2;
                codelen = 3;
            } else if ((reg & 0xf0000000) == 0x20000000) {
                /* 0010 */
                num_zeros = 3;
                codelen = 4;
            } else if ((reg & 0xf8000000) == 0x30000000) {
                /* 00110 */
                num_zeros = 4;
                codelen = 5;
            } else if ((reg & 0xf8000000) == 0x38000000) {
                /* 00111... */
                num_zeros = 5 + ((reg >> 25) & 3);
                codelen = 7;
            } else {
                assert(0);
            }
            if (codelen > (reg_size - padding_bits)) {
                /* Don't match using padding bits */
                break;
            }
            assert(num_zeros >= 2 && num_zeros <= 8);
            for (i = 0; i < num_zeros; i++) {
                *q++ = last;
            }
            num_pixels += num_zeros;
        } else if ((reg & 0xc0000000) == 0x40000000) {
            /* 01... repeated small differences */
            int num, diff, amt, i;
            codelen = 6;
            if (codelen > (reg_size - padding_bits)) {
                break;
            }
            if (reg & 0x20000000) {
                num = 3;
            } else {
                num = 2;
            }
            amt = (reg & 0x0c000000) >> 26;
            if (reg & 0x10000000) {
                diff = -4 + amt;
            } else {
                diff = 1 + amt;
            }
            for (i = 0; i < num; i++) {
                unsigned char next = last + diff;
                *q++ = next;
                last = next;
            }
            num_pixels += num;
        } else {
            int diff;
            assert((reg & 0x80000000) == 0x80000000);
            /* 1... one sample difference */
            if ((reg & 0xe0000000) == 0x80000000) {
                /* 100 */
                diff = 0;
                codelen = 3;
            } else if ((reg & 0xe0000000) == 0xa0000000) {
                /* 1010 or 1011 */
                codelen = 4;
                if ((reg & 0xf0000000) == 0xa0000000) {
                    diff = 1;
                } else {
                    diff = -1;
                }
            } else if ((reg & 0xf0000000) == 0xc0000000) {
                /* 1100xy */
                int diffs[4] = {2, 3, -3, -2};
                codelen = 6;
                diff = diffs[(reg & 0x0c000000) >> 26];
            } else if ((reg & 0xf0000000) == 0xd0000000) {
                /* 1101xyy */
                int diffs[8] = {4, 5, 6, 7, -7, -6, -5, -4};
                codelen = 7;
                diff = diffs[(reg & 0x0e000000) >> 25];
            } else if ((reg & 0xf0000000) == 0xe0000000) {
                /* 1110... larger signed differences */
                int subtype = (reg & 0x0e000000) >> 25;
                int mask = (1 << (3 + (subtype >> 1))) - 1;
                int pos = 22 - (subtype >> 1);
                int amt = (reg >> pos) & mask;
                int bases[8] = {8, -15, 16, -31, 32, -63, 64, -127};
                codelen = 10 + (subtype >> 1);
                assert(subtype >= 0 && subtype <= 7);
                diff = bases[subtype] + amt;
            } else if ((reg & 0xf0000000) == 0xf0000000) {
                /* 1111000: +/- 128 */
                /* other 1111xxx values reserved, decoded the same for now */
                codelen = 7;
                diff = -128;
            } else {
                assert(0);
            }
            if (codelen > (reg_size - padding_bits)) {
                break;
            }
            *q++ = last + diff;
            last = last + diff;
            num_pixels++;
        }
        /* Remove the matched codeword from the shift register by
           shifting it off the top and decreasing the size
           accordingly. */
        reg <<= codelen;
        reg_size -= codelen;
    }
    /* If we've reached the end of our decoding but still have more
       than one byte's worth of bits in the shift register, "put" the
       extra bytes back so it's as if we hadn't read them in the first
       place. This is important when we get to the end of a row,
       because the next byte is the start of a new row that needs to
       be read directly, not via the shift register. */
    while (reg_size >= 8) {
        reg &= ~(0xff << (32 - reg_size));
        reg_size -= 8;
        p--;
    }
    assert(q - uncomp_buf == num_pixels);
    /* If the input is incorrectly encoded, it may produce too many
       pixels, going beyond the end of the row. The loop above always
       stops after the first codeword that that gets to or beyond the
       end of the row. But for instance if there is supposed to be one
       pixel left, and the last codeword (incorrectly) encodes two
       pixels, there will be an extra pixel. Up to EXPANSION number of
       extra pixels can occur temporarily, though it will be detected
       as a format error by the caller. */
    assert(num_pixels <= max_pixels + EXPANSION);
    memcpy(uncomp_p, uncomp_buf, num_pixels);

    *comp_size_inout = MIN(comp_size, p - comp_buf);
    free(comp_buf);
    *pixels_inout = num_pixels;
    state->reg = reg;
    state->reg_size = reg_size;
    state->last = last;
}

/* Read and decompress all the compressed samples in a BCFLAT file
   into the internal uncompressed format. A color image is stored as
   if it were a series of three grayscale images, one each for the
   red, green, and blue channels, whereas the channel information is
   interleaved in the internal format. Each row of samples is
   compressed separately, with the first sample of the row stored
   directly, and all subsequent samples compressed in terms of
   differences from the previous pixel. Because the compressed rows
   have unpredictable length, all the input bytes need to pass through
   a buffer. Since the buffer might not be long enough to hold a full
   row of samples, the decompression state is maintained across calls
   to decode_flat in one row. Returns 1 on success, 0 on an error. */
int read_flat_data(FILE *fh, struct image_info *info) {
    int channel, y;
    struct flat_decode_state state;
    unsigned char buf[FLATBUF];
    unsigned char pixel_buf[3 * EXPANSION * FLATBUF];
    int buf_read_pos = 0;
    size_t num_read;
    for (channel = 0; channel <= 2; channel++) {
        for (y = 0; y < info->height; y++) {
            unsigned char *row = info->pixels + 3 * y * info->width;
            unsigned char first;
            int x = 0;
            if (buf_read_pos > 0) {
                /* Get the first byte already buffered */
                first = buf[0];
                memmove(buf, buf + 1, buf_read_pos - 1);
                buf_read_pos--;
            } else {
                /* If the buffer is empty, read the first byte directly */
                num_read = fread(&first, 1, 1, fh);
                if (num_read != 1) {
                    format_problem = "failed to read first byte";
                    return 0;
                }
            }
            row[0 + channel] = first;
            x++;
            /* Initialize the decompression state based on the first
               sample and with an empty shift register. */
            state.last = first;
            state.reg = 0;
            state.reg_size = 0;
            while (x < info->width) {
                /* This limit ensures we stop decoding when we get to
                   the end of the row. */
                int max_pixels = info->width - x;
                int comp_size, num_pixels, i;
                if (buf_read_pos < FLATBUF) {
                    /* If there's space in the buffer, read more
                       data. Enough to fill the buffer, if
                       possible. */
                    int num_to_read = FLATBUF - buf_read_pos;
                    num_read = fread(buf + buf_read_pos, 1, num_to_read, fh);
                    if (num_read < num_to_read && !feof(fh)) {
                        format_problem = "short read";
                        return 0;
                    }
                    buf_read_pos += num_read;
                }
                comp_size = buf_read_pos;
                if (!comp_size) {
                    format_problem = "too little data";
                    return 0;
                }
                num_pixels = max_pixels;
                decode_flat(buf, &comp_size, pixel_buf, &num_pixels, &state);
                assert(num_pixels > 0);
                assert(num_pixels <= max_pixels + EXPANSION);
                if (num_pixels > max_pixels) {
                    format_problem = "excess pixels at end of row";
                    return 0;
                }
                for (i = 0; i < num_pixels; i++) {
                    /* Write samples into the uncompressed row. These
                       samples of the same color are spaced every 3rd
                       byte. */
                    assert(x < info->width);
                    row[3*x + channel] = pixel_buf[i];
                    x++;
                }
                /* memmove is similar to memcpy, but it is
                   particularly guaranteed to work correctly when the
                   source and destination regions might overlap, as
                   they can do when moving data down a buffer. */
                memmove(buf, buf + comp_size, buf_read_pos - comp_size);
                buf_read_pos -= comp_size;
            }
        }
    }
    return 1;
}

long size_limit = 26754; /* floor(sqrt(2**31/3)) */

/* Read a BCFLAT image from a file into our internal format. Only the
   magic number should have been read before calling this
   routine. Returns a pointer to an image_info structure representing
   the image, or a null pointer on failure such as invalid or
   unsupported image contents. */
struct image_info *parse_bcflat(FILE *fh) {
    struct image_info *info, *info_footer;
    size_t num_read;
    int num_bytes, is_ok;
    long width, height;
    unsigned char flags[8], *pixels;

    num_read = fread(flags, 8, 1, fh);
    if (num_read != 1) return 0;

    if (flags[0] != 0 || flags[1] != 0 || flags[2] != 0 || flags[3] != 0 ||
        flags[4] != 0 || flags[5] != 0) {
        format_problem = "reserved flags should be 0";
        return 0;
    }

    if (flags[6] != 0x0d) {
        format_problem = "unsupported dictionary size";
        return 0; /* 0x0d = 13-bit dictionary */
    }

    if (flags[7] != 0x03) {
        format_problem = "unsupported number of channels";
        return 0; /* 0x03 = 3 channels */
    }

    width = read_u64_bigendian(fh);
    if (width == -1) return 0;

    height = read_u64_bigendian(fh);
    if (height == -1) return 0;

    /* Size must be positive */
    if (height < 1 || width < 1) {
        format_problem = "size must be positive";
        return 0;
    }

    /* Sanity-check size */
    if (height > size_limit || width > size_limit) {
        format_problem = "size too large compared to stack";
        return 0;
    }

    num_bytes = 3 * width * height;
    pixels = xmalloc(num_bytes +
                     TRAILER_ALIGNMENT + sizeof(struct image_info));
    info_footer = trailer_location(pixels, num_bytes);
    info_footer->width = width;
    info_footer->height = height;
    info_footer->pixels = pixels;
    info_footer->create_time = -1;
    info_footer->cleanup = 0;

    is_ok = process_tagged_data(fh, info_footer);
    if (!is_ok) {
        free(pixels);
        return 0;
    }

    if (!read_flat_data(fh, info_footer)) {
        free(pixels);
        return 0;
    }

    /* Copy metadata from the footer into a new separate object */
    info = xmalloc(sizeof(struct image_info));
    info->width = info_footer->width;
    info->height = info_footer->height;
    info->create_time = info_footer->create_time;
    info->pixels = info_footer->pixels;
    info->cleanup = info_footer->cleanup;
    return info;
}

/* Top-level routine for reading a Badly Coded image file into an
   internal format. All this function knows how to do is to match the
   magic number and dispatch to an appropriate format-specific parse
   function. Returns an image_info pointer on success, or a null
   pointer on failure. */
struct image_info *parse_image(const char *fname) {
    FILE *fh = fopen(fname, "rb");
    size_t num_read;
    unsigned char magic[8];
    struct image_info *info;

    if (!fh) {
        fprintf(stderr, "Failed to open %s: %s\n", fname, strerror(errno));
        return 0;
    }
    
    num_read = fread(magic, 8, 1, fh);
    if (num_read != 1) {
        fprintf(stderr, "Failed to read magic number from %s\n", fname);
        fclose(fh);
        return 0;
    }

    format_problem = 0;

    if (memcmp(magic, bcraw_magic, 8) == 0) {
        info = parse_bcraw(fh);
    } else if (memcmp(magic, bcprog_magic, 8) == 0) {
        info = parse_bcprog(fh);
    } else if (memcmp(magic, bcflat_magic, 8) == 0) {
        info = parse_bcflat(fh);
    } else {
        fprintf(stderr, "%s: unrecognized format\n", fname);
        fclose(fh);
        return 0;
    }

    fclose(fh);

    if (!info) {
        /* All different sorts of file errors lead to this message. */
        if (format_problem)
            fprintf(stderr, "%s: invalid format, %s\n", fname, format_problem);
        else
            fprintf(stderr, "%s: invalid format\n", fname);
    }

    return info;
}

/* Do all the cleanup associated with an image_info that is no longer
   needed: call the destructor if present, and free both the pixel
   data and the structure. */
void free_image_info(struct image_info *info) {
    if (info->cleanup)
        (*info->cleanup)();
    free(info->pixels);
    free(info);
}

void benign_target(void) {
    /* Currently doesn't do anything useful */
    static int benign_counter;
    benign_counter++;
    return;
}

/* Normally this function is never called. The only way it could get
   executed is via some sort of control-flow hijacking attack. */
void shellcode_target(void) {
    printf("\nIf this code is executed, it means that some sort of "
           "attack has happened!\n");
    exit(1);
}

/* Callback executed for each image viewed. */
void (*per_image_callback)(void);

/* Print a log message about the image being displayed, including
   the creation time stamp. */
void print_log_msg(struct image_info *info) {
    struct tm tm_parts;
    char time_str[80];
    if (info->create_time != -1) {
        localtime_r(&info->create_time, &tm_parts);
        strftime(time_str, 80, "%a, %d %b %Y %T %z", &tm_parts);
    } else {
        strcpy(time_str, "recently");
    }
    printf(logging_fmt, info->width, info->height, time_str,
           info->create_time);
    printf("\n");

}
