/* xcprint.h - stand-in for xclip's xcprint.h: xclib.c only calls errmalloc() (out of memory),
 * which the program linking xclib.c defines (clip/rdx_clip.c). See README.rdma-display. */
extern void errmalloc(void);
