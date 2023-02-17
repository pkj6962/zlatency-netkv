
struct bitmap; 


/* Bitmap abstract data type. */

/* Creation and destruction. */
struct bitmap *bitmap_create (unsigned int bit_cnt);
struct bitmap *bitmap_create_in_buf (unsigned int bit_cnt, void *, unsigned int byte_cnt);
unsigned int bitmap_buf_size (unsigned int bit_cnt);
void bitmap_destroy (struct bitmap *);

/* Bitmap size. */
unsigned int bitmap_size (const struct bitmap *);

/* Setting and testing single bits. */
void bitmap_set (struct bitmap *, unsigned int idx, bool);
void bitmap_mark (struct bitmap *, unsigned int idx);
void bitmap_reset (struct bitmap *, unsigned int idx);
void bitmap_flip (struct bitmap *, unsigned int idx);
bool bitmap_test (const struct bitmap *, unsigned int idx);

/* Setting and testing multiple bits. */
void bitmap_set_all (struct bitmap *, bool);
void bitmap_set_multiple (struct bitmap *, unsigned int start, unsigned int cnt, bool);
unsigned int bitmap_count (const struct bitmap *, unsigned int start, unsigned int cnt, bool);
bool bitmap_contains (const struct bitmap *, unsigned int start, unsigned int cnt, bool);
bool bitmap_any (const struct bitmap *, unsigned int start, unsigned int cnt);
bool bitmap_none (const struct bitmap *, unsigned int start, unsigned int cnt);
bool bitmap_all (const struct bitmap *, unsigned int start, unsigned int cnt);

/* Finding set or unset bits. */
#define BITMAP_ERROR 987654321
unsigned int bitmap_scan (const struct bitmap *, unsigned int start, unsigned int cnt, bool);
unsigned int bitmap_scan_and_flip (struct bitmap *, unsigned int start, unsigned int cnt, bool);

/* File input and output. */
