/*
 * line renderer
 * (c) Copyright Dave, 2004
 * (C) notaz, 2006-2010
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */
/*
 * The renderer has 4 modes now:
 * - normal
 * - shadow/hilight (s/h)
 * - "sonic mode" for midline palette changes (8bit mode only)
 * - accurate sprites (AS) [+ s/h]
 *
 * AS and s/h both use upper bits for both priority and shadow/hilight flags.
 * "sonic mode" is autodetected, shadow/hilight is enabled by emulated game.
 * AS is enabled by user and takes priority over "sonic mode".
 *
 * since renderer always draws line in 8bit mode, there are 2 spare bits:
 * b \ mode: s/h             as        sonic
 * 00        normal          -         pal index
 * 01        shadow          -         pal index
 * 10        hilight+op spr  spr       pal index
 * 11        shadow +op spr  -         pal index
 *
 * not handled properly:
 * - hilight op on shadow tile
 * - AS + s/h (s/h sprite flag interferes with and cleared by AS code)
 */

#include "pico_int.h"

int (*PicoScanBegin)(unsigned int num) = NULL;
int (*PicoScanEnd)  (unsigned int num) = NULL;

static unsigned char DefHighCol[8+320+8];
unsigned char *HighCol = DefHighCol;
static unsigned char *HighColBase = DefHighCol;
static int HighColIncrement;

static unsigned int DefOutBuff[320*2/2];
void *DrawLineDestBase = DefOutBuff;
int DrawLineDestIncrement;

static int  HighCacheA[41+1];   // caches for high layers
static int  HighCacheB[41+1];
static int  HighPreSpr[80*2+1]; // slightly preprocessed sprites

#define SPRL_HAVE_HI     0x80 // have hi priority sprites
#define SPRL_HAVE_LO     0x40 // *lo*
#define SPRL_MAY_HAVE_OP 0x20 // may have operator sprites on the line
#define SPRL_LO_ABOVE_HI 0x10 // low priority sprites may be on top of hi
unsigned char HighLnSpr[240][3 + MAX_LINE_SPRITES]; // sprite_count, ^flags, tile_count, [spritep]...

int rendstatus_old;
int rendlines;
int PicoDrawMask = -1;

static int skip_next_line=0;

//unsigned short ppt[] = { 0x0f11, 0x0ff1, 0x01f1, 0x011f, 0x01ff, 0x0f1f, 0x0f0e, 0x0e7c };

struct TileStrip
{
  int nametab; // Position in VRAM of name table (for this tile line)
  int line;    // Line number in pixels 0x000-0x3ff within the virtual tilemap
  int hscroll; // Horizontal scroll value in pixels for the line
  int xmask;   // X-Mask (0x1f - 0x7f) for horizontal wraparound in the tilemap
  int *hc;     // cache for high tile codes and their positions
  int cells;   // cells (tiles) to draw (32 col mode doesn't need to update whole 320)
};

// stuff available in asm:
#ifdef _ASM_DRAW_C
void DrawWindow(int tstart, int tend, int prio, int sh,
                struct PicoEState *est);
void DrawAllSprites(unsigned char *sprited, int prio, int sh,
                    struct PicoEState *est);
void DrawTilesFromCache(int *hc, int sh, int rlim,
                    struct PicoEState *est);
void DrawSpritesSHi(unsigned char *sprited, struct PicoEState *est);
void DrawLayer(int plane_sh, int *hcache, int cellskip, int maxcells,
               struct PicoEState *est);
void *blockcpy(void *dst, const void *src, size_t n);
void blockcpy_or(void *dst, void *src, size_t n, int pat);
#else
// utility
void blockcpy_or(void *dst, void *src, size_t n, int pat)
{
  unsigned char *pd = dst, *ps = src;
  for (; n; n--)
    *pd++ = (unsigned char) (*ps++ | pat);
}
#define blockcpy memcpy
#endif


#define TileNormMaker(funcname,pix_func)                     \
static int funcname(int sx,int addr,int pal)                 \
{                                                            \
  unsigned char *pd = Pico.est.HighCol+sx;                   \
  unsigned int pack=0; unsigned int t=0;                     \
                                                             \
  pack=*(unsigned int *)(Pico.vram+addr); /* Get 8 pixels */ \
  if (pack)                                                  \
  {                                                          \
    t=(pack&0x0000f000)>>12; pix_func(0);                    \
    t=(pack&0x00000f00)>> 8; pix_func(1);                    \
    t=(pack&0x000000f0)>> 4; pix_func(2);                    \
    t=(pack&0x0000000f)    ; pix_func(3);                    \
    t=(pack&0xf0000000)>>28; pix_func(4);                    \
    t=(pack&0x0f000000)>>24; pix_func(5);                    \
    t=(pack&0x00f00000)>>20; pix_func(6);                    \
    t=(pack&0x000f0000)>>16; pix_func(7);                    \
    return 0;                                                \
  }                                                          \
                                                             \
  return 1; /* Tile blank */                                 \
}


#define TileFlipMaker(funcname,pix_func)                     \
static int funcname(int sx,int addr,int pal)                 \
{                                                            \
  unsigned char *pd = Pico.est.HighCol+sx;                   \
  unsigned int pack=0; unsigned int t=0;                     \
                                                             \
  pack=*(unsigned int *)(Pico.vram+addr); /* Get 8 pixels */ \
  if (pack)                                                  \
  {                                                          \
    t=(pack&0x000f0000)>>16; pix_func(0);                    \
    t=(pack&0x00f00000)>>20; pix_func(1);                    \
    t=(pack&0x0f000000)>>24; pix_func(2);                    \
    t=(pack&0xf0000000)>>28; pix_func(3);                    \
    t=(pack&0x0000000f)    ; pix_func(4);                    \
    t=(pack&0x000000f0)>> 4; pix_func(5);                    \
    t=(pack&0x00000f00)>> 8; pix_func(6);                    \
    t=(pack&0x0000f000)>>12; pix_func(7);                    \
    return 0;                                                \
  }                                                          \
                                                             \
  return 1; /* Tile blank */                                 \
}


#ifdef _ASM_DRAW_C_AMIPS
int TileNorm(int sx,int addr,int pal);
int TileFlip(int sx,int addr,int pal);
#else

#define pix_just_write(x) \
  if (t) pd[x]=pal|t

TileNormMaker(TileNorm,pix_just_write)
TileFlipMaker(TileFlip,pix_just_write)

#endif

#ifndef _ASM_DRAW_C

// draw a sprite pixel, process operator colors
#define pix_sh(x) \
  if (!t); \
  else if (t>=0xe) pd[x]=(pd[x]&0x3f)|(t<<6); /* c0 shadow, 80 hilight */ \
  else pd[x]=pal|t

TileNormMaker(TileNormSH, pix_sh)
TileFlipMaker(TileFlipSH, pix_sh)

// draw a sprite pixel, mark operator colors
#define pix_sh_markop(x) \
  if (!t); \
  else if (t>=0xe) pd[x]|=0x80; \
  else pd[x]=pal|t

TileNormMaker(TileNormSH_markop, pix_sh_markop)
TileFlipMaker(TileFlipSH_markop, pix_sh_markop)

// process operator pixels only, apply only on low pri tiles and other op pixels
#define pix_sh_onlyop(x) \
  if (t>=0xe && (pd[x]&0xc0)) \
    pd[x]=(pd[x]&0x3f)|(t<<6); /* c0 shadow, 80 hilight */ \

TileNormMaker(TileNormSH_onlyop_lp, pix_sh_onlyop)
TileFlipMaker(TileFlipSH_onlyop_lp, pix_sh_onlyop)

#endif

// draw a sprite pixel (AS)
#define pix_as(x) \
  if (t && !(pd[x]&0x80)) pd[x]=pal|t

TileNormMaker(TileNormAS, pix_as)
TileFlipMaker(TileFlipAS, pix_as)

// draw a sprite pixel, skip operator colors (AS)
#define pix_sh_as_noop(x) \
  if (t && t < 0xe && !(pd[x]&0x80)) pd[x]=pal|t

TileNormMaker(TileNormAS_noop, pix_sh_as_noop)
TileFlipMaker(TileFlipAS_noop, pix_sh_as_noop)

// mark pixel as sprite pixel (AS)
#define pix_sh_as_onlymark(x) \
  if (t) pd[x]|=0x80

TileNormMaker(TileNormAS_onlymark, pix_sh_as_onlymark)
TileFlipMaker(TileFlipAS_onlymark, pix_sh_as_onlymark)


// --------------------------------------------

#ifndef _ASM_DRAW_C
static void DrawStrip(struct TileStrip *ts, int plane_sh, int cellskip)
{
  int tilex,dx,ty,code=0,addr=0,cells;
  int oldcode=-1,blank=-1; // The tile we know is blank
  int pal=0,sh;

  // Draw tiles across screen:
  sh=(plane_sh<<5)&0x40;
  tilex=((-ts->hscroll)>>3)+cellskip;
  ty=(ts->line&7)<<1; // Y-Offset into tile
  dx=((ts->hscroll-1)&7)+1;
  cells = ts->cells - cellskip;
  if(dx != 8) cells++; // have hscroll, need to draw 1 cell more
  dx+=cellskip<<3;

  for (; cells > 0; dx+=8,tilex++,cells--)
  {
    int zero=0;

    code=Pico.vram[ts->nametab+(tilex&ts->xmask)];
    if (code==blank) continue;
    if (code>>15) { // high priority tile
      int cval = code | (dx<<16) | (ty<<25);
      if(code&0x1000) cval^=7<<26;
      *ts->hc++ = cval; // cache it
      continue;
    }

    if (code!=oldcode) {
      oldcode = code;
      // Get tile address/2:
      addr=(code&0x7ff)<<4;
      addr+=ty;
      if (code&0x1000) addr^=0xe; // Y-flip

      pal=((code>>9)&0x30)|sh;
    }

    if (code&0x0800) zero=TileFlip(dx,addr,pal);
    else             zero=TileNorm(dx,addr,pal);

    if (zero) blank=code; // We know this tile is blank now
  }

  // terminate the cache list
  *ts->hc = 0;
  // if oldcode wasn't changed, it means all layer is hi priority
  if (oldcode == -1) Pico.est.rendstatus |= PDRAW_PLANE_HI_PRIO;
}

// this is messy
void DrawStripVSRam(struct TileStrip *ts, int plane_sh, int cellskip)
{
  int tilex,dx,code=0,addr=0,cell=0;
  int oldcode=-1,blank=-1; // The tile we know is blank
  int pal=0,scan=Pico.est.DrawScanline;

  // Draw tiles across screen:
  tilex=(-ts->hscroll)>>3;
  dx=((ts->hscroll-1)&7)+1;
  if(dx != 8) cell--; // have hscroll, start with negative cell
  cell+=cellskip;
  tilex+=cellskip;
  dx+=cellskip<<3;

  for (; cell < ts->cells; dx+=8,tilex++,cell++)
  {
    int zero=0,nametabadd,ty;

    //if((cell&1)==0)
    {
      int line,vscroll;
      vscroll=Pico.vsram[(plane_sh&1)+(cell&~1)];

      // Find the line in the name table
      line=(vscroll+scan)&ts->line&0xffff; // ts->line is really ymask ..
      nametabadd=(line>>3)<<(ts->line>>24);    // .. and shift[width]
      ty=(line&7)<<1; // Y-Offset into tile
    }

    code=Pico.vram[ts->nametab+nametabadd+(tilex&ts->xmask)];
    if (code==blank) continue;
    if (code>>15) { // high priority tile
      int cval = code | (dx<<16) | (ty<<25);
      if(code&0x1000) cval^=7<<26;
      *ts->hc++ = cval; // cache it
      continue;
    }

    if (code!=oldcode) {
      oldcode = code;
      // Get tile address/2:
      addr=(code&0x7ff)<<4;
      if (code&0x1000) addr+=14-ty; else addr+=ty; // Y-flip

      pal=((code>>9)&0x30)|((plane_sh<<5)&0x40);
    }

    if (code&0x0800) zero=TileFlip(dx,addr,pal);
    else             zero=TileNorm(dx,addr,pal);

    if (zero) blank=code; // We know this tile is blank now
  }

  // terminate the cache list
  *ts->hc = 0;
  if (oldcode == -1) Pico.est.rendstatus |= PDRAW_PLANE_HI_PRIO;
}
#endif

#ifndef _ASM_DRAW_C
static
#endif
void DrawStripInterlace(struct TileStrip *ts)
{
  int tilex=0,dx=0,ty=0,code=0,addr=0,cells;
  int oldcode=-1,blank=-1; // The tile we know is blank
  int pal=0;

  // Draw tiles across screen:
  tilex=(-ts->hscroll)>>3;
  ty=(ts->line&15)<<1; // Y-Offset into tile
  dx=((ts->hscroll-1)&7)+1;
  cells = ts->cells;
  if(dx != 8) cells++; // have hscroll, need to draw 1 cell more

  for (; cells; dx+=8,tilex++,cells--)
  {
    int zero=0;

    code=Pico.vram[ts->nametab+(tilex&ts->xmask)];
    if (code==blank) continue;
    if (code>>15) { // high priority tile
      int cval = (code&0xfc00) | (dx<<16) | (ty<<25);
      cval|=(code&0x3ff)<<1;
      if(code&0x1000) cval^=0xf<<26;
      *ts->hc++ = cval; // cache it
      continue;
    }

    if (code!=oldcode) {
      oldcode = code;
      // Get tile address/2:
      addr=(code&0x7ff)<<5;
      if (code&0x1000) addr+=30-ty; else addr+=ty; // Y-flip

//      pal=Pico.cram+((code>>9)&0x30);
      pal=((code>>9)&0x30);
    }

    if (code&0x0800) zero=TileFlip(dx,addr,pal);
    else             zero=TileNorm(dx,addr,pal);

    if (zero) blank=code; // We know this tile is blank now
  }

  // terminate the cache list
  *ts->hc = 0;
}

// --------------------------------------------

#ifndef _ASM_DRAW_C
static void DrawLayer(int plane_sh, int *hcache, int cellskip, int maxcells,
  struct PicoEState *est)
{
  struct PicoVideo *pvid=&Pico.video;
  const char shift[4]={5,6,5,7}; // 32,64 or 128 sized tilemaps (2 is invalid)
  struct TileStrip ts;
  int width, height, ymask;
  int vscroll, htab;

  ts.hc=hcache;
  ts.cells=maxcells;

  // Work out the TileStrip to draw

  // Work out the name table size: 32 64 or 128 tiles (0-3)
  width=pvid->reg[16];
  height=(width>>4)&3; width&=3;

  ts.xmask=(1<<shift[width])-1; // X Mask in tiles (0x1f-0x7f)
  ymask=(height<<8)|0xff;       // Y Mask in pixels
  if(width == 1)   ymask&=0x1ff;
  else if(width>1) ymask =0x0ff;

  // Find name table:
  if (plane_sh&1) ts.nametab=(pvid->reg[4]&0x07)<<12; // B
  else            ts.nametab=(pvid->reg[2]&0x38)<< 9; // A

  htab=pvid->reg[13]<<9; // Horizontal scroll table address
  if ( pvid->reg[11]&2)     htab+=est->DrawScanline<<1; // Offset by line
  if ((pvid->reg[11]&1)==0) htab&=~0xf; // Offset by tile
  htab+=plane_sh&1; // A or B

  // Get horizontal scroll value, will be masked later
  ts.hscroll=Pico.vram[htab&0x7fff];

  if((pvid->reg[12]&6) == 6) {
    // interlace mode 2
    vscroll=Pico.vsram[plane_sh&1]; // Get vertical scroll value

    // Find the line in the name table
    ts.line=(vscroll+(est->DrawScanline<<1))&((ymask<<1)|1);
    ts.nametab+=(ts.line>>4)<<shift[width];

    DrawStripInterlace(&ts);
  } else if( pvid->reg[11]&4) {
    // shit, we have 2-cell column based vscroll
    // luckily this doesn't happen too often
    ts.line=ymask|(shift[width]<<24); // save some stuff instead of line
    DrawStripVSRam(&ts, plane_sh, cellskip);
  } else {
    vscroll=Pico.vsram[plane_sh&1]; // Get vertical scroll value

    // Find the line in the name table
    ts.line=(vscroll+est->DrawScanline)&ymask;
    ts.nametab+=(ts.line>>3)<<shift[width];

    DrawStrip(&ts, plane_sh, cellskip);
  }
}


// --------------------------------------------

// tstart & tend are tile pair numbers
static void DrawWindow(int tstart, int tend, int prio, int sh,
                       struct PicoEState *est)
{
  struct PicoVideo *pvid=&Pico.video;
  int tilex,ty,nametab,code=0;
  int blank=-1; // The tile we know is blank

  // Find name table line:
  if (pvid->reg[12]&1)
  {
    nametab=(pvid->reg[3]&0x3c)<<9; // 40-cell mode
    nametab+=(est->DrawScanline>>3)<<6;
  }
  else
  {
    nametab=(pvid->reg[3]&0x3e)<<9; // 32-cell mode
    nametab+=(est->DrawScanline>>3)<<5;
  }

  tilex=tstart<<1;

  if (!(est->rendstatus & PDRAW_WND_DIFF_PRIO)) {
    // check the first tile code
    code=Pico.vram[nametab+tilex];
    // if the whole window uses same priority (what is often the case), we may be able to skip this field
    if ((code>>15) != prio) return;
  }

  tend<<=1;
  ty=(est->DrawScanline&7)<<1; // Y-Offset into tile

  // Draw tiles across screen:
  if (!sh)
  {
    for (; tilex < tend; tilex++)
    {
      int addr=0,zero=0;
      int pal;

      code=Pico.vram[nametab+tilex];
      if (code==blank) continue;
      if ((code>>15) != prio) {
        est->rendstatus |= PDRAW_WND_DIFF_PRIO;
        continue;
      }

      pal=((code>>9)&0x30);

      // Get tile address/2:
      addr=(code&0x7ff)<<4;
      if (code&0x1000) addr+=14-ty; else addr+=ty; // Y-flip

      if (code&0x0800) zero=TileFlip(8+(tilex<<3),addr,pal);
      else             zero=TileNorm(8+(tilex<<3),addr,pal);

      if (zero) blank=code; // We know this tile is blank now
    }
  }
  else
  {
    for (; tilex < tend; tilex++)
    {
      int addr=0,zero=0;
      int pal;

      code=Pico.vram[nametab+tilex];
      if(code==blank) continue;
      if((code>>15) != prio) {
        est->rendstatus |= PDRAW_WND_DIFF_PRIO;
        continue;
      }

      pal=((code>>9)&0x30);

      if (prio) {
        int *zb = (int *)(est->HighCol+8+(tilex<<3));
        *zb++ &= 0xbfbfbfbf;
        *zb   &= 0xbfbfbfbf;
      } else {
        pal |= 0x40;
      }

      // Get tile address/2:
      addr=(code&0x7ff)<<4;
      if (code&0x1000) addr+=14-ty; else addr+=ty; // Y-flip

      if (code&0x0800) zero=TileFlip(8+(tilex<<3),addr,pal);
      else             zero=TileNorm(8+(tilex<<3),addr,pal);

      if (zero) blank=code; // We know this tile is blank now
    }
  }
}

// --------------------------------------------

static void DrawTilesFromCacheShPrep(void)
{
  // as some layer has covered whole line with hi priority tiles,
  // we can process whole line and then act as if sh/hi mode was off,
  // but leave lo pri op sprite markers alone
  int c = 320/4, *zb = (int *)(Pico.est.HighCol+8);
  Pico.est.rendstatus |= PDRAW_SHHI_DONE;
  while (c--)
  {
    *zb++ &= 0xbfbfbfbf;
  }
}

static void DrawTilesFromCache(int *hc, int sh, int rlim, struct PicoEState *est)
{
  int code, addr, dx;
  int pal;

  // *ts->hc++ = code | (dx<<16) | (ty<<25); // cache it

  if (sh && (est->rendstatus & (PDRAW_SHHI_DONE|PDRAW_PLANE_HI_PRIO)))
  {
    if (!(est->rendstatus & PDRAW_SHHI_DONE))
      DrawTilesFromCacheShPrep();
    sh = 0;
  }

  if (!sh)
  {
    short blank=-1; // The tile we know is blank
    while ((code=*hc++)) {
      int zero;
      if((short)code == blank) continue;
      // Get tile address/2:
      addr=(code&0x7ff)<<4;
      addr+=(unsigned int)code>>25; // y offset into tile
      dx=(code>>16)&0x1ff;

      pal=((code>>9)&0x30);
      if (rlim-dx < 0) goto last_cut_tile;

      if (code&0x0800) zero=TileFlip(dx,addr,pal);
      else             zero=TileNorm(dx,addr,pal);

      if (zero) blank=(short)code;
    }
  }
  else
  {
    while ((code=*hc++)) {
      unsigned char *zb;
      // Get tile address/2:
      addr=(code&0x7ff)<<4;
      addr+=(unsigned int)code>>25; // y offset into tile
      dx=(code>>16)&0x1ff;
      zb = est->HighCol+dx;
      *zb++ &= 0xbf; *zb++ &= 0xbf; *zb++ &= 0xbf; *zb++ &= 0xbf;
      *zb++ &= 0xbf; *zb++ &= 0xbf; *zb++ &= 0xbf; *zb++ &= 0xbf;

      pal=((code>>9)&0x30);
      if (rlim-dx < 0) goto last_cut_tile;

      if (code&0x0800) TileFlip(dx,addr,pal);
      else             TileNorm(dx,addr,pal);
    }
  }
  return;

last_cut_tile:
  {
    unsigned int t, pack=*(unsigned int *)(Pico.vram+addr); // Get 8 pixels
    unsigned char *pd = est->HighCol+dx;
    if (!pack) return;
    if (code&0x0800)
    {
      switch (rlim-dx+8)
      {
        case 7: t=pack&0x00000f00; if (t) pd[6]=(unsigned char)(pal|(t>> 8)); // "break" is left out intentionally
        case 6: t=pack&0x000000f0; if (t) pd[5]=(unsigned char)(pal|(t>> 4));
        case 5: t=pack&0x0000000f; if (t) pd[4]=(unsigned char)(pal|(t    ));
        case 4: t=pack&0xf0000000; if (t) pd[3]=(unsigned char)(pal|(t>>28));
        case 3: t=pack&0x0f000000; if (t) pd[2]=(unsigned char)(pal|(t>>24));
        case 2: t=pack&0x00f00000; if (t) pd[1]=(unsigned char)(pal|(t>>20));
        case 1: t=pack&0x000f0000; if (t) pd[0]=(unsigned char)(pal|(t>>16));
        default: break;
      }
    }
    else
    {
      switch (rlim-dx+8)
      {
        case 7: t=pack&0x00f00000; if (t) pd[6]=(unsigned char)(pal|(t>>20));
        case 6: t=pack&0x0f000000; if (t) pd[5]=(unsigned char)(pal|(t>>24));
        case 5: t=pack&0xf0000000; if (t) pd[4]=(unsigned char)(pal|(t>>28));
        case 4: t=pack&0x0000000f; if (t) pd[3]=(unsigned char)(pal|(t    ));
        case 3: t=pack&0x000000f0; if (t) pd[2]=(unsigned char)(pal|(t>> 4));
        case 2: t=pack&0x00000f00; if (t) pd[1]=(unsigned char)(pal|(t>> 8));
        case 1: t=pack&0x0000f000; if (t) pd[0]=(unsigned char)(pal|(t>>12));
        default: break;
      }
    }
  }
}

// --------------------------------------------

// Index + 0  :    hhhhvvvv ab--hhvv yyyyyyyy yyyyyyyy // a: offscreen h, b: offs. v, h: horiz. size
// Index + 4  :    xxxxxxxx xxxxxxxx pccvhnnn nnnnnnnn // x: x coord + 8

static void DrawSprite(int *sprite, int sh)
{
  int width=0,height=0;
  int row=0,code=0;
  int pal;
  int tile=0,delta=0;
  int sx, sy;
  int (*fTileFunc)(int sx,int addr,int pal);

  // parse the sprite data
  sy=sprite[0];
  code=sprite[1];
  sx=code>>16; // X
  width=sy>>28;
  height=(sy>>24)&7; // Width and height in tiles
  sy=(sy<<16)>>16; // Y

  row=Pico.est.DrawScanline-sy; // Row of the sprite we are on

  if (code&0x1000) row=(height<<3)-1-row; // Flip Y

  tile=code + (row>>3); // Tile number increases going down
  delta=height; // Delta to increase tile by going right
  if (code&0x0800) { tile+=delta*(width-1); delta=-delta; } // Flip X

  tile &= 0x7ff; tile<<=4; tile+=(row&7)<<1; // Tile address
  delta<<=4; // Delta of address

  pal=(code>>9)&0x30;
  pal|=sh<<6;

  if (sh && (code&0x6000) == 0x6000) {
    if(code&0x0800) fTileFunc=TileFlipSH_markop;
    else            fTileFunc=TileNormSH_markop;
  } else {
    if(code&0x0800) fTileFunc=TileFlip;
    else            fTileFunc=TileNorm;
  }

  for (; width; width--,sx+=8,tile+=delta)
  {
    if(sx<=0)   continue;
    if(sx>=328) break; // Offscreen

    tile&=0x7fff; // Clip tile address
    fTileFunc(sx,tile,pal);
  }
}
#endif

static void DrawSpriteInterlace(unsigned int *sprite)
{
  int width=0,height=0;
  int row=0,code=0;
  int pal;
  int tile=0,delta=0;
  int sx, sy;

  // parse the sprite data
  sy=sprite[0];
  height=sy>>24;
  sy=(sy&0x3ff)-0x100; // Y
  width=(height>>2)&3; height&=3;
  width++; height++; // Width and height in tiles

  row=(Pico.est.DrawScanline<<1)-sy; // Row of the sprite we are on

  code=sprite[1];
  sx=((code>>16)&0x1ff)-0x78; // X

  if (code&0x1000) row^=(16<<height)-1; // Flip Y

  tile=code&0x3ff; // Tile number
  tile+=row>>4; // Tile number increases going down
  delta=height; // Delta to increase tile by going right
  if (code&0x0800) { tile+=delta*(width-1); delta=-delta; } // Flip X

  tile<<=5; tile+=(row&15)<<1; // Tile address

  delta<<=5; // Delta of address
  pal=((code>>9)&0x30); // Get palette pointer

  for (; width; width--,sx+=8,tile+=delta)
  {
    if(sx<=0)   continue;
    if(sx>=328) break; // Offscreen

    tile&=0x7fff; // Clip tile address
    if (code&0x0800) TileFlip(sx,tile,pal);
    else             TileNorm(sx,tile,pal);
  }
}


static void DrawAllSpritesInterlace(int pri, int sh)
{
  struct PicoVideo *pvid=&Pico.video;
  int i,u,table,link=0,sline=Pico.est.DrawScanline<<1;
  unsigned int *sprites[80]; // Sprite index

  table=pvid->reg[5]&0x7f;
  if (pvid->reg[12]&1) table&=0x7e; // Lowest bit 0 in 40-cell mode
  table<<=8; // Get sprite table address/2

  for (i=u=0; u < 80 && i < 21; u++)
  {
    unsigned int *sprite;
    int code, sx, sy, height;

    sprite=(unsigned int *)(Pico.vram+((table+(link<<2))&0x7ffc)); // Find sprite

    // get sprite info
    code = sprite[0];
    sx = sprite[1];
    if(((sx>>15)&1) != pri) goto nextsprite; // wrong priority sprite

    // check if it is on this line
    sy = (code&0x3ff)-0x100;
    height = (((code>>24)&3)+1)<<4;
    if(sline < sy || sline >= sy+height) goto nextsprite; // no

    // check if sprite is not hidden offscreen
    sx = (sx>>16)&0x1ff;
    sx -= 0x78; // Get X coordinate + 8
    if(sx <= -8*3 || sx >= 328) goto nextsprite;

    // sprite is good, save it's pointer
    sprites[i++]=sprite;

    nextsprite:
    // Find next sprite
    link=(code>>16)&0x7f;
    if(!link) break; // End of sprites
  }

  // Go through sprites backwards:
  for (i-- ;i>=0; i--)
    DrawSpriteInterlace(sprites[i]);
}


#ifndef _ASM_DRAW_C
/*
 * s/h drawing: lo_layers|40, lo_sprites|40 && mark_op,
 *        hi_layers&=~40, hi_sprites
 *
 * Index + 0  :    hhhhvvvv ----hhvv yyyyyyyy yyyyyyyy // v, h: vert./horiz. size
 * Index + 4  :    xxxxxxxx xxxxxxxx pccvhnnn nnnnnnnn // x: x coord + 8
 */
static void DrawSpritesSHi(unsigned char *sprited, const struct PicoEState *est)
{
  int (*fTileFunc)(int sx,int addr,int pal);
  unsigned char *p;
  int cnt;

  cnt = sprited[0] & 0x7f;
  if (cnt == 0) return;

  p = &sprited[3];

  // Go through sprites backwards:
  for (cnt--; cnt >= 0; cnt--)
  {
    int *sprite, code, pal, tile, sx, sy;
    int offs, delta, width, height, row;

    offs = (p[cnt] & 0x7f) * 2;
    sprite = est->HighPreSpr + offs;
    code = sprite[1];
    pal = (code>>9)&0x30;

    if (pal == 0x30)
    {
      if (code & 0x8000) // hi priority
      {
        if (code&0x800) fTileFunc=TileFlipSH;
        else            fTileFunc=TileNormSH;
      } else {
        if (code&0x800) fTileFunc=TileFlipSH_onlyop_lp;
        else            fTileFunc=TileNormSH_onlyop_lp;
      }
    } else {
      if (!(code & 0x8000)) continue; // non-operator low sprite, already drawn
      if (code&0x800) fTileFunc=TileFlip;
      else            fTileFunc=TileNorm;
    }

    // parse remaining sprite data
    sy=sprite[0];
    sx=code>>16; // X
    width=sy>>28;
    height=(sy>>24)&7; // Width and height in tiles
    sy=(sy<<16)>>16; // Y

    row=est->DrawScanline-sy; // Row of the sprite we are on

    if (code&0x1000) row=(height<<3)-1-row; // Flip Y

    tile=code + (row>>3); // Tile number increases going down
    delta=height; // Delta to increase tile by going right
    if (code&0x0800) { tile+=delta*(width-1); delta=-delta; } // Flip X

    tile &= 0x7ff; tile<<=4; tile+=(row&7)<<1; // Tile address
    delta<<=4; // Delta of address

    for (; width; width--,sx+=8,tile+=delta)
    {
      if(sx<=0)   continue;
      if(sx>=328) break; // Offscreen

      tile&=0x7fff; // Clip tile address
      fTileFunc(sx,tile,pal);
    }
  }
}
#endif // !_ASM_DRAW_C

static void DrawSpritesHiAS(unsigned char *sprited, int sh)
{
  int (*fTileFunc)(int sx,int addr,int pal);
  unsigned char *p;
  int entry, cnt, sh_cnt = 0;

  cnt = sprited[0] & 0x7f;
  if (cnt == 0) return;

  Pico.est.rendstatus |= PDRAW_SPR_LO_ON_HI;

  p = &sprited[3];

  // Go through sprites:
  for (entry = 0; entry < cnt; entry++)
  {
    int *sprite, code, pal, tile, sx, sy;
    int offs, delta, width, height, row;

    offs = (p[entry] & 0x7f) * 2;
    sprite = HighPreSpr + offs;
    code = sprite[1];
    pal = (code>>9)&0x30;

    if (code & 0x8000) // hi priority
    {
      if (sh && pal == 0x30)
      {
        if (code&0x800) fTileFunc=TileFlipAS_noop;
        else            fTileFunc=TileNormAS_noop;
      } else {
        if (code&0x800) fTileFunc=TileFlipAS;
        else            fTileFunc=TileNormAS;
      }
    } else {
      if (code&0x800) fTileFunc=TileFlipAS_onlymark;
      else            fTileFunc=TileNormAS_onlymark;
    }
    if (sh && pal == 0x30)
      p[sh_cnt++] = offs / 2; // re-save for sh/hi pass

    // parse remaining sprite data
    sy=sprite[0];
    sx=code>>16; // X
    width=sy>>28;
    height=(sy>>24)&7; // Width and height in tiles
    sy=(sy<<16)>>16; // Y

    row=Pico.est.DrawScanline-sy; // Row of the sprite we are on

    if (code&0x1000) row=(height<<3)-1-row; // Flip Y

    tile=code + (row>>3); // Tile number increases going down
    delta=height; // Delta to increase tile by going right
    if (code&0x0800) { tile+=delta*(width-1); delta=-delta; } // Flip X

    tile &= 0x7ff; tile<<=4; tile+=(row&7)<<1; // Tile address
    delta<<=4; // Delta of address

    pal |= 0x80;
    for (; width; width--,sx+=8,tile+=delta)
    {
      if(sx<=0)   continue;
      if(sx>=328) break; // Offscreen

      tile&=0x7fff; // Clip tile address
      fTileFunc(sx,tile,pal);
    }
  }

  if (!sh || !(sprited[1]&SPRL_MAY_HAVE_OP)) return;

  /* nasty 1: remove 'sprite' flags */
  {
    int c = 320/4/4, *zb = (int *)(Pico.est.HighCol+8);
    while (c--)
    {
      *zb++ &= 0x7f7f7f7f; *zb++ &= 0x7f7f7f7f;
      *zb++ &= 0x7f7f7f7f; *zb++ &= 0x7f7f7f7f;
    }
  }

  /* nasty 2: sh operator pass */
  sprited[0] = sh_cnt;
  DrawSpritesSHi(sprited, &Pico.est);
}


// Index + 0  :    ----hhvv -lllllll -------y yyyyyyyy
// Index + 4  :    -------x xxxxxxxx pccvhnnn nnnnnnnn
// v
// Index + 0  :    hhhhvvvv ----hhvv yyyyyyyy yyyyyyyy // v, h: vert./horiz. size
// Index + 4  :    xxxxxxxx xxxxxxxx pccvhnnn nnnnnnnn // x: x coord + 8

void PrepareSprites(int full)
{
  const struct PicoVideo *pvid=&Pico.video;
  const struct PicoEState *est=&Pico.est;
  int u,link=0,sh;
  int table=0;
  int *pd = HighPreSpr;
  int max_lines = 224, max_sprites = 80, max_width = 328;
  int max_line_sprites = 20; // 20 sprites, 40 tiles

  if (!(Pico.video.reg[12]&1))
    max_sprites = 64, max_line_sprites = 16, max_width = 264;
  if (PicoOpt & POPT_DIS_SPRITE_LIM)
    max_line_sprites = MAX_LINE_SPRITES;

  if (pvid->reg[1]&8) max_lines = 240;
  sh = Pico.video.reg[0xC]&8; // shadow/hilight?

  table=pvid->reg[5]&0x7f;
  if (pvid->reg[12]&1) table&=0x7e; // Lowest bit 0 in 40-cell mode
  table<<=8; // Get sprite table address/2

  if (!full)
  {
    int pack;
    // updates: tilecode, sx
    for (u=0; u < max_sprites && (pack = *pd); u++, pd+=2)
    {
      unsigned int *sprite;
      int code2, sx, sy, height;

      sprite=(unsigned int *)(Pico.vram+((table+(link<<2))&0x7ffc)); // Find sprite

      // parse sprite info
      code2 = sprite[1];
      sx = (code2>>16)&0x1ff;
      sx -= 0x78; // Get X coordinate + 8
      sy = (pack << 16) >> 16;
      height = (pack >> 24) & 0xf;

      if (sy < max_lines &&
	  sy + (height<<3) > est->DrawScanline && // sprite onscreen (y)?
          (sx > -24 || sx < max_width))                   // onscreen x
      {
        int y = (sy >= est->DrawScanline) ? sy : est->DrawScanline;
        int entry = ((pd - HighPreSpr) / 2) | ((code2>>8)&0x80);
        for (; y < sy + (height<<3) && y < max_lines; y++)
        {
          int i, cnt;
          cnt = HighLnSpr[y][0] & 0x7f;
          if (cnt >= max_line_sprites) continue;              // sprite limit?

          for (i = 0; i < cnt; i++)
            if (((HighLnSpr[y][3+i] ^ entry) & 0x7f) == 0) goto found;

          // this sprite was previously missing
          HighLnSpr[y][3+cnt] = entry;
          HighLnSpr[y][0] = cnt + 1;
found:;
          if (entry & 0x80)
               HighLnSpr[y][1] |= SPRL_HAVE_HI;
          else HighLnSpr[y][1] |= SPRL_HAVE_LO;
        }
      }

      code2 &= ~0xfe000000;
      code2 -=  0x00780000; // Get X coordinate + 8 in upper 16 bits
      pd[1] = code2;

      // Find next sprite
      link=(sprite[0]>>16)&0x7f;
      if (!link) break; // End of sprites
    }
  }
  else
  {
    for (u = 0; u < max_lines; u++)
      *((int *)&HighLnSpr[u][0]) = 0;

    for (u = 0; u < max_sprites; u++)
    {
      unsigned int *sprite;
      int code, code2, sx, sy, hv, height, width;

      sprite=(unsigned int *)(Pico.vram+((table+(link<<2))&0x7ffc)); // Find sprite

      // parse sprite info
      code = sprite[0];
      sy = (code&0x1ff)-0x80;
      hv = (code>>24)&0xf;
      height = (hv&3)+1;

      width  = (hv>>2)+1;
      code2 = sprite[1];
      sx = (code2>>16)&0x1ff;
      sx -= 0x78; // Get X coordinate + 8

      if (sy < max_lines && sy + (height<<3) > est->DrawScanline) // sprite onscreen (y)?
      {
        int entry, y, sx_min, onscr_x, maybe_op = 0;

        sx_min = 8-(width<<3);
        onscr_x = sx_min < sx && sx < max_width;
        if (sh && (code2 & 0x6000) == 0x6000)
          maybe_op = SPRL_MAY_HAVE_OP;

        entry = ((pd - HighPreSpr) / 2) | ((code2>>8)&0x80);
        y = (sy >= est->DrawScanline) ? sy : est->DrawScanline;
        for (; y < sy + (height<<3) && y < max_lines; y++)
        {
	  unsigned char *p = &HighLnSpr[y][0];
          int cnt = p[0];
          if (cnt >= max_line_sprites) continue;              // sprite limit?

          if (p[2] >= max_line_sprites*2) {        // tile limit?
            p[0] |= 0x80;
            continue;
          }
          p[2] += width;

          if (sx == -0x78) {
            if (cnt > 0)
              p[0] |= 0x80; // masked, no more sprites for this line
            continue;
          }
          // must keep the first sprite even if it's offscreen, for masking
          if (cnt > 0 && !onscr_x) continue; // offscreen x

          p[3+cnt] = entry;
          p[0] = cnt + 1;
          p[1] |= (entry & 0x80) ? SPRL_HAVE_HI : SPRL_HAVE_LO;
          p[1] |= maybe_op; // there might be op sprites on this line
          if (cnt > 0 && (code2 & 0x8000) && !(p[3+cnt-1]&0x80))
            p[1] |= SPRL_LO_ABOVE_HI;
        }
      }

      *pd++ = (width<<28)|(height<<24)|(hv<<16)|((unsigned short)sy);
      *pd++ = (sx<<16)|((unsigned short)code2);

      // Find next sprite
      link=(code>>16)&0x7f;
      if (!link) break; // End of sprites
    }
    *pd = 0;

#if 0
    for (u = 0; u < max_lines; u++)
    {
      int y;
      printf("c%03i: %2i, %2i: ", u, HighLnSpr[u][0] & 0x7f, HighLnSpr[u][2]);
      for (y = 0; y < HighLnSpr[u][0] & 0x7f; y++)
        printf(" %i", HighLnSpr[u][y+3]);
      printf("\n");
    }
#endif
  }
}

#ifndef _ASM_DRAW_C
static void DrawAllSprites(unsigned char *sprited, int prio, int sh,
                           struct PicoEState *est)
{
  int rs = est->rendstatus;
  unsigned char *p;
  int cnt;

  if (rs & (PDRAW_SPRITES_MOVED|PDRAW_DIRTY_SPRITES)) {
    //elprintf(EL_STATUS, "PrepareSprites(%i)", (rs>>4)&1);
    PrepareSprites(rs & PDRAW_DIRTY_SPRITES);
    est->rendstatus = rs & ~(PDRAW_SPRITES_MOVED|PDRAW_DIRTY_SPRITES);
  }

  cnt = sprited[0] & 0x7f;
  if (cnt == 0) return;

  p = &sprited[3];

  // Go through sprites backwards:
  for (cnt--; cnt >= 0; cnt--)
  {
    int offs;
    if ((p[cnt] >> 7) != prio) continue;
    offs = (p[cnt]&0x7f) * 2;
    DrawSprite(HighPreSpr + offs, sh);
  }
}


// --------------------------------------------

void BackFill(int reg7, int sh, struct PicoEState *est)
{
  unsigned int back;

  // Start with a blank scanline (background colour):
  back=reg7&0x3f;
  back|=sh<<6;
  back|=back<<8;
  back|=back<<16;

  memset32((int *)(est->HighCol+8), back, 320/4);
}
#endif

// --------------------------------------------

#ifndef _ASM_DRAW_C
void PicoDoHighPal555(int sh, int line, struct PicoEState *est)
{
  unsigned int *spal, *dpal;
  unsigned int t, i;

  Pico.m.dirtyPal = 0;

  spal = (void *)Pico.cram;
  dpal = (void *)est->HighPal;

  for (i = 0; i < 0x40 / 2; i++) {
    t = spal[i];
#ifdef USE_BGR555
    t = ((t & 0x000e000e)<< 1) | ((t & 0x00e000e0)<<3) | ((t & 0x0e000e00)<<4);
#else
    t = ((t & 0x000e000e)<<12) | ((t & 0x00e000e0)<<3) | ((t & 0x0e000e00)>>7);
#endif
    // treat it like it was 4-bit per channel, since in s/h mode it somewhat is that.
    // otherwise intensity difference between this and s/h will be wrong
    t |= (t >> 4) & 0x08610861; // 0x18e318e3
    dpal[i] = t;
  }

  // norm: xxx0, sh: 0xxx, hi: 0xxx + 7
  if (sh)
  {
    // shadowed pixels
    for (i = 0; i < 0x40 / 2; i++)
      dpal[0x40/2 | i] = dpal[0xc0/2 | i] = (dpal[i] >> 1) & 0x738e738e;
    // hilighted pixels
    for (i = 0; i < 0x40 / 2; i++) {
      t = ((dpal[i] >> 1) & 0x738e738e) + 0x738e738e; // 0x7bef7bef;
      t |= (t >> 4) & 0x08610861;
      dpal[0x80/2 | i] = t;
    }
  }
}

void FinalizeLine555(int sh, int line, struct PicoEState *est)
{
  unsigned short *pd=est->DrawLineDest;
  unsigned char  *ps=est->HighCol+8;
  unsigned short *pal=est->HighPal;
  int len;

  if (Pico.m.dirtyPal)
    PicoDoHighPal555(sh, line, est);

  if (Pico.video.reg[12]&1) {
    len = 320;
  } else {
    if (!(PicoOpt&POPT_DIS_32C_BORDER)) pd+=32;
    len = 256;
  }

  {
#ifndef PSP
    int i, mask=0xff;
    if (!sh && (est->rendstatus & PDRAW_SPR_LO_ON_HI))
      mask=0x3f; // accurate sprites, upper bits are priority stuff

    for (i = 0; i < len; i++)
      pd[i] = pal[ps[i] & mask];
#else
    extern void amips_clut(unsigned short *dst, unsigned char *src, unsigned short *pal, int count);
    extern void amips_clut_6bit(unsigned short *dst, unsigned char *src, unsigned short *pal, int count);
    if (!sh && (est->rendstatus & PDRAW_SPR_LO_ON_HI))
         amips_clut_6bit(pd, ps, pal, len);
    else amips_clut(pd, ps, pal, len);
#endif
  }
}
#endif

static void FinalizeLine8bit(int sh, int line, struct PicoEState *est)
{
  unsigned char *pd = est->DrawLineDest;
  int len, rs = est->rendstatus;
  static int dirty_count;

  if (!sh && Pico.m.dirtyPal == 1)
  {
    // a hack for mid-frame palette changes
    if (!(rs & PDRAW_SONIC_MODE))
         dirty_count = 1;
    else dirty_count++;
    rs |= PDRAW_SONIC_MODE;
    est->rendstatus = rs;
    if (dirty_count == 3) {
      blockcpy(est->HighPal, Pico.cram, 0x40*2);
    } else if (dirty_count == 11) {
      blockcpy(est->HighPal+0x40, Pico.cram, 0x40*2);
    }
  }

  if (Pico.video.reg[12]&1) {
    len = 320;
  } else {
    if (!(PicoOpt & POPT_DIS_32C_BORDER))
      pd += 32;
    len = 256;
  }

  if (!sh && (rs & PDRAW_SONIC_MODE)) {
    if (dirty_count >= 11) {
      blockcpy_or(pd, est->HighCol+8, len, 0x80);
    } else {
      blockcpy_or(pd, est->HighCol+8, len, 0x40);
    }
  } else {
    blockcpy(pd, est->HighCol+8, len);
  }
}

static void (*FinalizeLine)(int sh, int line, struct PicoEState *est);

// --------------------------------------------

static int DrawDisplay(int sh)
{
  struct PicoEState *est=&Pico.est;
  unsigned char *sprited = &HighLnSpr[est->DrawScanline][0];
  struct PicoVideo *pvid=&Pico.video;
  int win=0,edge=0,hvwind=0;
  int maxw,maxcells;

  est->rendstatus &= ~(PDRAW_SHHI_DONE|PDRAW_PLANE_HI_PRIO);

  if (pvid->reg[12]&1) {
    maxw = 328; maxcells = 40;
  } else {
    maxw = 264; maxcells = 32;
  }

  // Find out if the window is on this line:
  win=pvid->reg[0x12];
  edge=(win&0x1f)<<3;

  if (win&0x80) { if (est->DrawScanline>=edge) hvwind=1; }
  else          { if (est->DrawScanline< edge) hvwind=1; }

  if (!hvwind) // we might have a vertical window here
  {
    win=pvid->reg[0x11];
    edge=win&0x1f;
    if (win&0x80) {
      if (!edge) hvwind=1;
      else if(edge < (maxcells>>1)) hvwind=2;
    } else {
      if (!edge);
      else if(edge < (maxcells>>1)) hvwind=2;
      else hvwind=1;
    }
  }

  /* - layer B low - */
  if (PicoDrawMask & PDRAW_LAYERB_ON)
    DrawLayer(1|(sh<<1), HighCacheB, 0, maxcells, est);
  /* - layer A low - */
  if (!(PicoDrawMask & PDRAW_LAYERA_ON));
  else if (hvwind == 1)
    DrawWindow(0, maxcells>>1, 0, sh, est);
  else if (hvwind == 2) {
    DrawLayer(0|(sh<<1), HighCacheA, (win&0x80) ?    0 : edge<<1, (win&0x80) ?     edge<<1 : maxcells, est);
    DrawWindow(                      (win&0x80) ? edge :       0, (win&0x80) ? maxcells>>1 : edge, 0, sh, est);
  } else
    DrawLayer(0|(sh<<1), HighCacheA, 0, maxcells, est);
  /* - sprites low - */
  if (!(PicoDrawMask & PDRAW_SPRITES_LOW_ON));
  else if (Pico.est.rendstatus & PDRAW_INTERLACE)
    DrawAllSpritesInterlace(0, sh);
  else if (sprited[1] & SPRL_HAVE_LO)
    DrawAllSprites(sprited, 0, sh, est);

  /* - layer B hi - */
  if ((PicoDrawMask & PDRAW_LAYERB_ON) && HighCacheB[0])
    DrawTilesFromCache(HighCacheB, sh, maxw, est);
  /* - layer A hi - */
  if (!(PicoDrawMask & PDRAW_LAYERA_ON));
  else if (hvwind == 1)
    DrawWindow(0, maxcells>>1, 1, sh, est);
  else if (hvwind == 2) {
    if (HighCacheA[0])
      DrawTilesFromCache(HighCacheA, sh, (win&0x80) ? edge<<4 : maxw, est);
    DrawWindow((win&0x80) ? edge : 0, (win&0x80) ? maxcells>>1 : edge, 1, sh, est);
  } else
    if (HighCacheA[0])
      DrawTilesFromCache(HighCacheA, sh, maxw, est);
  /* - sprites hi - */
  if (!(PicoDrawMask & PDRAW_SPRITES_HI_ON));
  else if (Pico.est.rendstatus & PDRAW_INTERLACE)
    DrawAllSpritesInterlace(1, sh);
  // have sprites without layer pri bit ontop of sprites with that bit
  else if ((sprited[1] & 0xd0) == 0xd0 && (PicoOpt & POPT_ACC_SPRITES))
    DrawSpritesHiAS(sprited, sh);
  else if (sh && (sprited[1] & SPRL_MAY_HAVE_OP))
    DrawSpritesSHi(sprited, est);
  else if (sprited[1] & SPRL_HAVE_HI)
    DrawAllSprites(sprited, 1, 0, est);

#if 0
  {
    int *c, a, b;
    for (a = 0, c = HighCacheA; *c; c++, a++);
    for (b = 0, c = HighCacheB; *c; c++, b++);
    printf("%i:%03i: a=%i, b=%i\n", Pico.m.frame_count,
           Pico.est.DrawScanline, a, b);
  }
#endif

  return 0;
}

// MUST be called every frame
PICO_INTERNAL void PicoFrameStart(void)
{
  int offs = 8, lines = 224;

  // prepare to do this frame
  Pico.est.rendstatus = 0;
  if ((Pico.video.reg[12] & 6) == 6)
    Pico.est.rendstatus |= PDRAW_INTERLACE; // interlace mode
  if (!(Pico.video.reg[12] & 1))
    Pico.est.rendstatus |= PDRAW_32_COLS;
  if (Pico.video.reg[1] & 8) {
    offs = 0;
    lines = 240;
  }

  if (Pico.est.rendstatus != rendstatus_old || lines != rendlines) {
    rendlines = lines;
    // mode_change() might reset rendstatus_old by calling SetColorFormat
    emu_video_mode_change((lines == 240) ? 0 : 8,
      lines, (Pico.video.reg[12] & 1) ? 0 : 1);
    rendstatus_old = Pico.est.rendstatus;
  }

  Pico.est.HighCol = HighColBase + offs * HighColIncrement;
  Pico.est.DrawLineDest = (char *)DrawLineDestBase + offs * DrawLineDestIncrement;
  Pico.est.DrawScanline = 0;
  skip_next_line = 0;

  if (PicoOpt & POPT_ALT_RENDERER)
    return;

  if (Pico.m.dirtyPal)
    Pico.m.dirtyPal = 2; // reset dirty if needed
  PrepareSprites(1);
}

static void DrawBlankedLine(int line, int offs, int sh, int bgc)
{
  if (PicoScanBegin != NULL)
    PicoScanBegin(line + offs);

  BackFill(bgc, sh, &Pico.est);

  if (FinalizeLine != NULL)
    FinalizeLine(sh, line, &Pico.est);

  if (PicoScanEnd != NULL)
    PicoScanEnd(line + offs);

  Pico.est.HighCol += HighColIncrement;
  Pico.est.DrawLineDest = (char *)Pico.est.DrawLineDest + DrawLineDestIncrement;
}

static void PicoLine(int line, int offs, int sh, int bgc)
{
  int skip = 0;

  if (skip_next_line > 0) {
    skip_next_line--;
    return;
  }

  Pico.est.DrawScanline = line;
  if (PicoScanBegin != NULL)
    skip = PicoScanBegin(line + offs);

  if (skip) {
    skip_next_line = skip - 1;
    return;
  }

  // Draw screen:
  BackFill(bgc, sh, &Pico.est);
  if (Pico.video.reg[1]&0x40)
    DrawDisplay(sh);

  if (FinalizeLine != NULL)
    FinalizeLine(sh, line, &Pico.est);

  if (PicoScanEnd != NULL)
    skip_next_line = PicoScanEnd(line + offs);

  Pico.est.HighCol += HighColIncrement;
  Pico.est.DrawLineDest = (char *)Pico.est.DrawLineDest + DrawLineDestIncrement;
}

void PicoDrawSync(int to, int blank_last_line)
{
  int line, offs = 0;
  int sh = (Pico.video.reg[0xC] & 8) >> 3; // shadow/hilight?
  int bgc = Pico.video.reg[7];

  pprof_start(draw);

  if (rendlines != 240)
    offs = 8;

  for (line = Pico.est.DrawScanline; line < to; line++)
  {
    PicoLine(line, offs, sh, bgc);
  }

  // last line
  if (line <= to)
  {
    if (blank_last_line)
         DrawBlankedLine(line, offs, sh, bgc);
    else PicoLine(line, offs, sh, bgc);
    line++;
  }
  Pico.est.DrawScanline = line;

  pprof_end(draw);
}

// also works for fast renderer
void PicoDrawUpdateHighPal(void)
{
  struct PicoEState *est = &Pico.est;
  int sh = (Pico.video.reg[0xC] & 8) >> 3; // shadow/hilight?
  if (PicoOpt & POPT_ALT_RENDERER)
    sh = 0; // no s/h support

  PicoDoHighPal555(sh, 0, &Pico.est);
  if (est->rendstatus & PDRAW_SONIC_MODE) {
    // FIXME?
    memcpy(est->HighPal + 0x40, est->HighPal, 0x40*2);
    memcpy(est->HighPal + 0x80, est->HighPal, 0x40*2);
  }
}

void PicoDrawSetOutFormat(pdso_t which, int use_32x_line_mode)
{
  switch (which)
  {
    case PDF_8BIT:
      FinalizeLine = FinalizeLine8bit;
      break;

    case PDF_RGB555:
      if ((PicoAHW & PAHW_32X) && use_32x_line_mode)
        FinalizeLine = FinalizeLine32xRGB555;
      else
        FinalizeLine = FinalizeLine555;
      break;

    default:
      FinalizeLine = NULL;
      break;
  }
  PicoDrawSetOutFormat32x(which, use_32x_line_mode);
  PicoDrawSetOutputMode4(which);
  rendstatus_old = -1;
}

// note: may be called on the middle of frame
void PicoDrawSetOutBuf(void *dest, int increment)
{
  DrawLineDestBase = dest;
  DrawLineDestIncrement = increment;
  Pico.est.DrawLineDest = (unsigned char*)DrawLineDestBase + Pico.est.DrawScanline * increment;
}

void PicoDrawSetInternalBuf(void *dest, int increment)
{
  if (dest != NULL) {
    HighColBase = dest;
    HighColIncrement = increment;
    Pico.est.HighCol = HighColBase + Pico.est.DrawScanline * increment;
  }
  else {
    HighColBase = DefHighCol;
    HighColIncrement = 0;
  }
}

void PicoDrawSetCallbacks(int (*begin)(unsigned int num), int (*end)(unsigned int num))
{
  PicoScanBegin = NULL;
  PicoScanEnd = NULL;
  PicoScan32xBegin = NULL;
  PicoScan32xEnd = NULL;

  if ((PicoAHW & PAHW_32X) && FinalizeLine != FinalizeLine32xRGB555) {
    PicoScan32xBegin = begin;
    PicoScan32xEnd = end;
  }
  else {
    PicoScanBegin = begin;
    PicoScanEnd = end;
  }
}

void PicoDrawInit(void)
{
  Pico.est.DrawLineDest = DefOutBuff;
  Pico.est.HighCol = HighColBase;
  Pico.est.HighPreSpr = HighPreSpr;
  rendstatus_old = -1;
}

// vim:ts=2:sw=2:expandtab
