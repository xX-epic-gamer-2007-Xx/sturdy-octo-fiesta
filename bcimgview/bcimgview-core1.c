/* Struct containing information about a parsed image. The pixel data
   is kept in an uncompressed format with red, green, and blue bytes
   per pixel. Rows are each stored left to right and the rows are top
   to bottom, with no padding. Sometimes a copy of this information is
   also kept after the end of the pixel data. */
struct image_info {
    long width;             /* width in pixels */
    long height;            /* height in pixels */
    void (*cleanup)(void);  /* destructor callback */
    unsigned char magic[8]; /* reserved for magic number */
    time_t create_time;     /* creation time, in Unix format */
    unsigned char *pixels;  /* pointer to pixel data */
};

/* Each Badly Coded image format is identified by a unique 8 bytes at
   the beginning of the file. */
unsigned char bcraw_magic[8] =
    {0x00, 0x42, 0x43, 0x52, 0xc3, 0x84, 0x57, 0x0a};

unsigned char bcprog_magic[8] =
    {0x42, 0x43, 0x50, 0x52, 0xc3, 0x96, 0x47, 0x0a};

unsigned char bcflat_magic[8] =
    {0x42, 0x43, 0x46, 0x4c, 0xc3, 0x84, 0x54, 0x0a};

/* To reduce the need for error checking code elsewhere in the
   program, this wrapper around malloc() will print an error message
   and then exit the program if an allocation fails. */
void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p) {
        fprintf(stderr, "Out of memory in allocation of %zd bytes\n", size);
        exit(1);
    }
    return p;
}

const char *format_problem = 0;

/* Most numeric metadata in Badly Coded image files is stored as
   big-endian 64-bit integers, commonly interpreted as unsigned. This
   routine will read one such number from a stdio stream. Note that it
   would not work correctly to just use fread() on its own if the
   system is little-endian like x86-64. */
uint64_t read_u64_bigendian(FILE *fh) {
    unsigned char bytes[8];
    uint64_t x = 0;
    int i, pos = 0;
    size_t num_read;

    num_read = fread(bytes, 8, 1, fh);
    if (num_read != 1) {
        format_problem = "short read of u64";
        return -1;
    }
    
    for (i = 7; i >= 0; i--) {
        x |= (uint64_t)bytes[i] << pos;
        pos += 8;
    }

    return x;
}

/* Printf format to log information about each displayed image */
const char *logging_fmt = "Displaying image of width %ld and height %ld"
    " from %s";

/* The tagged-data section of a Badly Coded image file contains
   optional information of various kinds: each kind of data is
   preceeded by a 4-byte type identifier and an 8-byte size (which
   counts bytes after the type identifier and size). A type identifier
   "DATA" represents the end of the tagged-data section, and is
   followed directly by the image data without another size. The
   function reads this region of a file, returning 1 if the format was
   correct or 0 if there was an error. */
int process_tagged_data(FILE *fh, struct image_info *info) {
    unsigned char ident[4];
    unsigned long size;
    size_t num_read;

    for (;;) {
        num_read = fread(ident, 4, 1, fh);
        if (num_read != 1) {
            format_problem = "short read of tag";
            return 0;
        }
        if (!memcmp(ident, "DATA", 4)) {
            /* We've reached the end of the tagged data */
            return 1;
        }
        size = read_u64_bigendian(fh);
        if (size == -1) {
            return 0;
        } else if (size > (1LL << 40)) {
            /* Reject any ridiculously large tag sizes. Code for
               specific tags may enforce more restrictive
               limitations. */
            format_problem = "tag too large";
            return 0;
        }
        if (!memcmp(ident, "TIME", 4)) {
            /* Creation time information */
            if (size != 8) {
                format_problem = "wrong size for TIME";
                return 0;
            }
            info->create_time = read_u64_bigendian(fh);
        } else if (!memcmp(ident, "FRMT", 4)) {
            /* Format for file information printing */
            char *fmt_buf = xmalloc(size + 1);
            num_read = fread(fmt_buf, 1, size, fh);
            if (num_read != size) {
                format_problem = "short read of format";
                return 0;
            }
            /* Add null terminator */
            fmt_buf[size] = 0;
            logging_fmt = fmt_buf;
        } else {
            /* An unrecognized tag is an error. */
            format_problem = "unrecognized tag";
            return 0;
        }
    }
    
}

/* Read the pixel data from a BCRAW image into the internal
   format. For the sake of 8-byte alignment, 8 contiguous pixels (24
   bytes) are read as a single unit. Returns 1 on success, or 0 for an
   error such as a short read.  */
int read_raw_data(FILE *fh, struct image_info *info) {
    int row, col;
    size_t num_read;
    unsigned char *p = info->pixels;

    for (row = 0; row < info->height; row++) {
        for (col = 0; col < info->width - 8; col += 8) {
            num_read = fread(p, 3, 8, fh);
            if (num_read != 8) {
                format_problem = "short read of raw data";
                return 0;
            }
            p += 3*8;
        }

        /* This loop covers any pixels in a row left over after the
           24-byte groups */
        for (; col < info->width; col++) {
            num_read = fread(p, 3, 1, fh);
            if (num_read != 1) {
                format_problem = "short read of raw data";
                return 0;
            }
            p += 3;
        }
    }
    return 1;
}

/* Desired alignment for the image_info data structure, in bytes. */
#ifdef __GNUC__
#define TRAILER_ALIGNMENT __alignof__(struct image_info)
#else
#define TRAILER_ALIGNMENT 8
#endif

/* Choose a location for the trailing image_info structure at the end
   of the memory allocation for an image (after the pixels) with
   proper alignment. This will add 0-7 bytes of padding in between the
   pixels and the image_info trailer. */
struct image_info *trailer_location(unsigned char *base, size_t pixel_bytes) {
    unsigned char *pixels_end = base + pixel_bytes;
    unsigned long trailer_loc = (unsigned long)pixels_end;
    unsigned long extra = trailer_loc % TRAILER_ALIGNMENT;
    unsigned long pad = (TRAILER_ALIGNMENT - extra) % TRAILER_ALIGNMENT;
    return (struct image_info *)(trailer_loc + pad);
}

/* Read a BCRAW image from a file into our internal format. Only the
   magic number should have been read before calling this
   routine. Returns a pointer to an image_info structure representing
   the image, or a null pointer on failure such as invalid or
   unsupported image contents. */
struct image_info *parse_bcraw(FILE *fh) {
    struct image_info *info, *info_footer;
    size_t num_read;
    int num_bytes, is_ok;
    long width, height;
    unsigned char flags[8], *pixels;

    num_read = fread(flags, 8, 1, fh);
    if (num_read != 1) {
        format_problem = "short read of flags";
        return 0;
    }

    if (flags[0] != 0 || flags[1] != 0 || flags[2] != 0 || flags[3] != 0 ||
        flags[4] != 0 || flags[5] != 0 || flags[6] != 0) {
        format_problem = "reserved flags should be 0";
        return 0;
    }

    if (flags[7] != 8) {
        format_problem = "unsupported depth";
        return 0; /* format should be 8, for 8 bit-deep RGB */
    }

    width = read_u64_bigendian(fh);
    if (width == -1) return 0;

    height = read_u64_bigendian(fh);
    if (height == -1) return 0;

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

    num_read = read_raw_data(fh, info_footer);
    if (!num_read) {
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

/* Read and transform the image data from a BCPROG file into our
   internal format. This happens in two steps. First, as the rows are
   being read, they are re-ordered from the progressive on-disk order
   into a normal sequential order. Then, the pixels in each row are
   expanded from the 8-bit format to 24-bit format. */
int read_prog_data(FILE *fh, struct image_info *info) {
    int row, col;
    size_t num_read;
    unsigned char *p = info->pixels;

    /* Step 1: decode progressive row ordering to sequential */
    /* Pass 1: multiples of 4 */
    row = 0;
    do {
        unsigned char *row_start = p + row * 3 * info->width;
        num_read = fread(row_start, info->width, 1, fh);
        if (num_read != 1) {
            format_problem = "short read of row";
            return 0;
        }
        row += 4;
    } while (row < info->height);
    /* Pass 2: odd multiples of 2 */
    row = 2;
    do {
        unsigned char *row_start = p + row * 3 * info->width;
        num_read = fread(row_start, info->width, 1, fh);
        if (num_read != 1) {
            format_problem = "short read of row";
            return 0;
        }
        row += 4;
    } while (row < info->height);
    /* Pass 3: rows */
    row = 1;
    do {
        unsigned char *row_start = p + row * 3 * info->width;
        num_read = fread(row_start, info->width, 1, fh);
        if (num_read != 1) {
            format_problem = "short read of row";
            return 0;
        }
        row += 2;
    } while (row < info->height);

    /* Step 2: decode 8-bit palette to 24-bit color */
    for (row = 0; row < info->height; row++) {
        /* This inner loop needs to run backwards because the decoding
           expands the pixel data. */
        unsigned char *row_p = p + row * 3 * info->width;
        for (col = info->width - 1; col >= 0; col--) {
            unsigned char packed = row_p[col];
            int r, g, b;
            if (packed >= 216) {
                format_problem = "invalid packed byte";
                return 0;
            }
            /* A number between 0 and 6**3-1 is interpreted like a
               number in base 6, where the three digits represent the
               red, blue, and green components. Digits between 0 and 5
               are scaled by 51 to 8-bit samples between 0 and 255. */
            b = packed % 6;
            packed /= 6;
            g = packed % 6;
            packed /= 6;
            r = packed;
            row_p[3 * col] = 51 * r;
            row_p[3 * col + 1] = 51 * g;
            row_p[3 * col + 2] = 51 * b;
        }
    }
    return 1;
}

/* Read a BCPROG image from a file into our internal format. Only the
   magic number should have been read before calling this
   routine. Returns a pointer to an image_info structure representing
   the image, or a null pointer on failure such as invalid or
   unsupported image contents. */
struct image_info *parse_bcprog(FILE *fh) {
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

    if (flags[6] != 0x01) {
        format_problem = "unsupported passes number";
        return 0; /* 1 = 3 pass row progressive */
    }

    if (flags[7] != 0xd8) {
        format_problem = "unsupported color depth";
        return 0; /* 0xd8 = 216 color "web safe" 6x6x6 cube palette */
    }

    width = read_u64_bigendian(fh);
    if (width == -1) return 0;

    height = read_u64_bigendian(fh);
    if (height == -1) return 0;

    /* Size must be positive, and tall enough for the progressive
       algorithm */
    if (height < 2 || width < 1) {
        format_problem = "size too small";
        return 0;
    }

    /* Size limited to SVGA-era 800x600 */
    if (height > 600 || width > 800) {
        format_problem = "size too large";
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

    num_read = read_prog_data(fh, info_footer);
    if (!num_read) {
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