#include "../pico_int.h"
#include "../memory.h"

#if 0
#undef ash2_end_run
#undef SekEndRun
#define ash2_end_run(x)
#define SekEndRun(x)
#endif

static const char str_mars[] = "MARS";

struct Pico32xMem *Pico32xMem;

static void bank_switch(int b);

// poll detection
#define POLL_THRESHOLD 6

struct poll_det {
  u32 addr, cycles, cyc_max;
  int cnt, flag;
};
static struct poll_det m68k_poll, sh2_poll[2];

static int p32x_poll_detect(struct poll_det *pd, u32 a, u32 cycles, int is_vdp)
{
  int ret = 0, flag = pd->flag;

  if (is_vdp)
    flag <<= 3;

  if (a - 2 <= pd->addr && pd->addr <= a + 2 && cycles - pd->cycles < pd->cyc_max) {
    pd->cnt++;
    if (pd->cnt > POLL_THRESHOLD) {
      if (!(Pico32x.emu_flags & flag)) {
        elprintf(EL_32X, "%s poll addr %08x, cyc %u",
          flag & (P32XF_68KPOLL|P32XF_68KVPOLL) ? "m68k" :
          (flag & (P32XF_MSH2POLL|P32XF_MSH2VPOLL) ? "msh2" : "ssh2"), a, cycles - pd->cycles);
        ret = 1;
      }
      Pico32x.emu_flags |= flag;
    }
  }
  else {
    pd->cnt = 0;
    pd->addr = a;
  }
  pd->cycles = cycles;

  return ret;
}

static int p32x_poll_undetect(struct poll_det *pd, int is_vdp)
{
  int ret = 0, flag = pd->flag;
  if (is_vdp)
    flag <<= 3; // VDP only
  else
    flag |= flag << 3; // both
  if (Pico32x.emu_flags & flag) {
    elprintf(EL_32X, "poll %02x -> %02x", Pico32x.emu_flags, Pico32x.emu_flags & ~flag);
    ret = 1;
  }
  Pico32x.emu_flags &= ~flag;
  pd->addr = pd->cnt = 0;
  return ret;
}

void p32x_poll_event(int cpu_mask, int is_vdp)
{
  if (cpu_mask & 1)
    p32x_poll_undetect(&sh2_poll[0], is_vdp);
  if (cpu_mask & 2)
    p32x_poll_undetect(&sh2_poll[1], is_vdp);
}

// SH2 faking
//#define FAKE_SH2
int p32x_csum_faked;
#ifdef FAKE_SH2
static const u16 comm_fakevals[] = {
  0x4d5f, 0x4f4b, // M_OK
  0x535f, 0x4f4b, // S_OK
  0x4D41, 0x5346, // MASF - Brutal Unleashed
  0x5331, 0x4d31, // Darxide
  0x5332, 0x4d32,
  0x5333, 0x4d33,
  0x0000, 0x0000, // eq for doom
  0x0002, // Mortal Kombat
//  0, // pad
};

static u32 sh2_comm_faker(u32 a)
{
  static int f = 0;
  if (a == 0x28 && !p32x_csum_faked) {
    p32x_csum_faked = 1;
    return *(unsigned short *)(Pico.rom + 0x18e);
  }
  if (f >= sizeof(comm_fakevals) / sizeof(comm_fakevals[0]))
    f = 0;
  return comm_fakevals[f++];
}
#endif

// DMAC handling
static struct {
  unsigned int sar0, dar0, tcr0; // src addr, dst addr, transfer count
  unsigned int chcr0; // chan ctl
  unsigned int sar1, dar1, tcr1; // same for chan 1
  unsigned int chcr1;
  int pad[4];
  unsigned int dmaor;
} * dmac0;

static void dma_68k2sh2_do(void)
{
  unsigned short *dreqlen = &Pico32x.regs[0x10 / 2];
  int i;

  if (dmac0->tcr0 != *dreqlen)
    elprintf(EL_32X|EL_ANOMALY, "tcr0 and dreq len differ: %d != %d", dmac0->tcr0, *dreqlen);

  // HACK: assume bus is busy and SH2 is halted
  // XXX: use different mechanism for this, not poll det
  Pico32x.emu_flags |= P32XF_MSH2POLL; // id ? P32XF_SSH2POLL : P32XF_MSH2POLL;

  for (i = 0; i < Pico32x.dmac_ptr && dmac0->tcr0 > 0; i++) {
    extern void p32x_sh2_write16(u32 a, u32 d, int id);
      elprintf(EL_32X, "dmaw [%08x] %04x, left %d", dmac0->dar0, Pico32x.dmac_fifo[i], *dreqlen);
    p32x_sh2_write16(dmac0->dar0, Pico32x.dmac_fifo[i], 0);
    dmac0->dar0 += 2;
    dmac0->tcr0--;
    (*dreqlen)--;
  }

  Pico32x.dmac_ptr = 0; // HACK
  Pico32x.regs[6 / 2] &= ~P32XS_FULL;
  if (*dreqlen == 0)
    Pico32x.regs[6 / 2] &= ~P32XS_68S; // transfer complete
  if (dmac0->tcr0 == 0) {
    dmac0->chcr0 |= 2; // DMA has ended normally
    p32x_poll_undetect(&sh2_poll[0], 0);
  }
}

// ------------------------------------------------------------------
// 68k regs

static u32 p32x_reg_read16(u32 a)
{
  a &= 0x3e;

  if (a == 2) // INTM, INTS
    return ((Pico32x.sh2irqi[0] & P32XI_CMD) >> 4) | ((Pico32x.sh2irqi[1] & P32XI_CMD) >> 3);
#if 0
  if ((a & 0x30) == 0x20)
    return sh2_comm_faker(a);
#else
  if ((a & 0x30) == 0x20 && p32x_poll_detect(&m68k_poll, a, SekCyclesDoneT(), 0)) {
    SekEndTimeslice(16);
  }
#endif

  if ((a & 0x30) == 0x30)
    return p32x_pwm_read16(a);

  return Pico32x.regs[a / 2];
}

static void p32x_reg_write8(u32 a, u32 d)
{
  u16 *r = Pico32x.regs;
  a &= 0x3f;

  // for things like bset on comm port
  m68k_poll.cnt = 0;

  if (a == 1 && !(r[0] & 1)) {
    r[0] |= 1;
    Pico32xStartup();
    return;
  }

  if (!(r[0] & 1))
    return;

  switch (a) {
    case 0: // adapter ctl
      r[0] = (r[0] & 0x83) | ((d << 8) & P32XS_FM);
      return;
    case 3: // irq ctl
      if ((d & 1) && !(Pico32x.sh2irqi[0] & P32XI_CMD)) {
        Pico32x.sh2irqi[0] |= P32XI_CMD;
        p32x_update_irls();
        SekEndRun(16);
      }
      if ((d & 2) && !(Pico32x.sh2irqi[1] & P32XI_CMD)) {
        Pico32x.sh2irqi[1] |= P32XI_CMD;
        p32x_update_irls();
        SekEndRun(16);
      }
      return;
    case 5: // bank
      d &= 7;
      if (r[4 / 2] != d) {
        r[4 / 2] = d;
        bank_switch(d);
      }
      return;
    case 7: // DREQ ctl
      r[6 / 2] = (r[6 / 2] & P32XS_FULL) | (d & (P32XS_68S|P32XS_DMA|P32XS_RV));
      return;
    case 0x1b: // TV
      r[0x1a / 2] = d;
      return;
  }

  if ((a & 0x30) == 0x20) {
    u8 *r8 = (u8 *)r;
    r8[a ^ 1] = d;
    p32x_poll_undetect(&sh2_poll[0], 0);
    p32x_poll_undetect(&sh2_poll[1], 0);
    // if some SH2 is busy waiting, it needs to see the result ASAP
    if (SekCyclesLeftNoMCD > 32)
      SekEndRun(32);
    return;
  }
}

static void p32x_reg_write16(u32 a, u32 d)
{
  u16 *r = Pico32x.regs;
  a &= 0x3e;

  // for things like bset on comm port
  m68k_poll.cnt = 0;

  switch (a) {
    case 0x00: // adapter ctl
      r[0] = (r[0] & 0x83) | (d & P32XS_FM);
      return;
    case 0x10: // DREQ len
      r[a / 2] = d & ~3;
      return;
    case 0x12: // FIFO reg
      if (!(r[6 / 2] & P32XS_68S)) {
        elprintf(EL_32X|EL_ANOMALY, "DREQ FIFO w16 without 68S?");
	return;
      }
      if (Pico32x.dmac_ptr < DMAC_FIFO_LEN) {
        Pico32x.dmac_fifo[Pico32x.dmac_ptr++] = d;
        if ((Pico32x.dmac_ptr & 3) == 0 && (dmac0->chcr0 & 3) == 1 && (dmac0->dmaor & 1))
          dma_68k2sh2_do();
        if (Pico32x.dmac_ptr == DMAC_FIFO_LEN)
          r[6 / 2] |= P32XS_FULL;
      }
      break;
  }

  // DREQ src, dst
  if      ((a & 0x38) == 0x08) {
    r[a / 2] = d;
    return;
  }
  // comm port
  else if ((a & 0x30) == 0x20 && r[a / 2] != d) {
    r[a / 2] = d;
    p32x_poll_undetect(&sh2_poll[0], 0);
    p32x_poll_undetect(&sh2_poll[1], 0);
    // same as for w8
    if (SekCyclesLeftNoMCD > 32)
      SekEndRun(32);
    return;
  }
  // PWM
  else if ((a & 0x30) == 0x30) {
    p32x_pwm_write16(a, d);
    return;
  }

  p32x_reg_write8(a + 1, d);
}

// ------------------------------------------------------------------
// VDP regs
static u32 p32x_vdp_read16(u32 a)
{
  a &= 0x0e;

  return Pico32x.vdp_regs[a / 2];
}

static void p32x_vdp_write8(u32 a, u32 d)
{
  u16 *r = Pico32x.vdp_regs;
  a &= 0x0f;

  // for FEN checks between writes
  sh2_poll[0].cnt = 0;

  // TODO: verify what's writeable
  switch (a) {
    case 0x01:
      // priority inversion is handled in palette
      if ((r[0] ^ d) & P32XV_PRI)
        Pico32x.dirty_pal = 1;
      r[0] = (r[0] & P32XV_nPAL) | (d & 0xff);
      if ((d & 3) == 3)
        elprintf(EL_32X|EL_ANOMALY, "TODO: mode3");
      break;
    case 0x05: // fill len
      r[4 / 2] = d & 0xff;
      break;
    case 0x0b:
      d &= 1;
      Pico32x.pending_fb = d;
      // if we are blanking and FS bit is changing
      if (((r[0x0a/2] & P32XV_VBLK) || (r[0] & P32XV_Mx) == 0) && ((r[0x0a/2] ^ d) & P32XV_FS)) {
        r[0x0a/2] ^= 1;
	Pico32xSwapDRAM(d ^ 1);
        elprintf(EL_32X, "VDP FS: %d", r[0x0a/2] & P32XV_FS);
      }
      break;
  }
}

static void p32x_vdp_write16(u32 a, u32 d)
{
  a &= 0x0e;
  if (a == 6) { // fill start
    Pico32x.vdp_regs[6 / 2] = d;
    return;
  }
  if (a == 8) { // fill data
    u16 *dram = Pico32xMem->dram[(Pico32x.vdp_regs[0x0a/2] & P32XV_FS) ^ 1];
    int len = Pico32x.vdp_regs[4 / 2] + 1;
    a = Pico32x.vdp_regs[6 / 2];
    while (len--) {
      dram[a] = d;
      a = (a & 0xff00) | ((a + 1) & 0xff);
    }
    Pico32x.vdp_regs[6 / 2] = a;
    Pico32x.vdp_regs[8 / 2] = d;
    return;
  }

  p32x_vdp_write8(a | 1, d);
}

// ------------------------------------------------------------------
// SH2 regs

static u32 p32x_sh2reg_read16(u32 a, int cpuid)
{
  u16 *r = Pico32x.regs;
  a &= 0xfe; // ?

  switch (a) {
    case 0x00: // adapter/irq ctl
      return (r[0] & P32XS_FM) | Pico32x.sh2_regs[0] | Pico32x.sh2irq_mask[cpuid];
    case 0x04: // H count (often as comm too)
      if (p32x_poll_detect(&sh2_poll[cpuid], a, ash2_cycles_done(), 0))
        ash2_end_run(8);
      return Pico32x.sh2_regs[4 / 2];
    case 0x10: // DREQ len
      return r[a / 2];
  }

  // DREQ src, dst
  if ((a & 0x38) == 0x08)
    return r[a / 2];
  // comm port
  if ((a & 0x30) == 0x20) {
    if (p32x_poll_detect(&sh2_poll[cpuid], a, ash2_cycles_done(), 0))
      ash2_end_run(8);
    return r[a / 2];
  }
  if ((a & 0x30) == 0x30) {
    sh2_poll[cpuid].cnt = 0;
    return p32x_pwm_read16(a);
  }

  return 0;
}

static void p32x_sh2reg_write8(u32 a, u32 d, int cpuid)
{
  a &= 0xff;
  switch (a) {
    case 0: // FM
      Pico32x.regs[0] &= ~P32XS_FM;
      Pico32x.regs[0] |= (d << 8) & P32XS_FM;
      return;
    case 1: // 
      Pico32x.sh2irq_mask[cpuid] = d & 0x8f;
      Pico32x.sh2_regs[0] &= ~0x80;
      Pico32x.sh2_regs[0] |= d & 0x80;
      p32x_update_irls();
      return;
    case 5: // H count
      Pico32x.sh2_regs[4 / 2] = d & 0xff;
      p32x_poll_undetect(&sh2_poll[cpuid ^ 1], 0);
      return;
  }

  if ((a & 0x30) == 0x20) {
    u8 *r8 = (u8 *)Pico32x.regs;
    r8[a ^ 1] = d;
    p32x_poll_undetect(&m68k_poll, 0);
    p32x_poll_undetect(&sh2_poll[cpuid ^ 1], 0);
    return;
  }
}

static void p32x_sh2reg_write16(u32 a, u32 d, int cpuid)
{
  a &= 0xfe;

  // comm
  if ((a & 0x30) == 0x20 && Pico32x.regs[a/2] != d) {
    Pico32x.regs[a / 2] = d;
    p32x_poll_undetect(&m68k_poll, 0);
    p32x_poll_undetect(&sh2_poll[cpuid ^ 1], 0);
    return;
  }
  // PWM
  else if ((a & 0x30) == 0x30) {
    p32x_pwm_write16(a, d);
    return;
  }

  switch (a) {
    case 0: // FM
      Pico32x.regs[0] &= ~P32XS_FM;
      Pico32x.regs[0] |= d & P32XS_FM;
      break;
    case 0x14: Pico32x.sh2irqs &= ~P32XI_VRES; goto irls;
    case 0x16: Pico32x.sh2irqs &= ~P32XI_VINT; goto irls;
    case 0x18: Pico32x.sh2irqs &= ~P32XI_HINT; goto irls;
    case 0x1a: Pico32x.sh2irqi[cpuid] &= ~P32XI_CMD; goto irls;
    case 0x1c:
      Pico32x.sh2irqs &= ~P32XI_PWM;
      p32x_pwm_irq_check(0);
      goto irls;
  }

  p32x_sh2reg_write8(a | 1, d, cpuid);
  return;

irls:
  p32x_update_irls();
}

// ------------------------------------------------------------------
// SH2 internal peripherals
static u32 sh2_peripheral_read8(u32 a, int id)
{
  u8 *r = (void *)Pico32xMem->sh2_peri_regs[id];
  u32 d;

  a &= 0x1ff;
  d = r[a];
  if (a == 4)
    d = 0x84; // SCI SSR

  elprintf(EL_32X, "%csh2 peri r8  [%08x]       %02x @%06x", id ? 's' : 'm', a | ~0x1ff, d, sh2_pc(id));
  return d;
}

static u32 sh2_peripheral_read32(u32 a, int id)
{
  u32 d;
  a &= 0x1fc;
  d = Pico32xMem->sh2_peri_regs[id][a / 4];

  elprintf(EL_32X, "%csh2 peri r32 [%08x] %08x @%06x", id ? 's' : 'm', a | ~0x1ff, d, sh2_pc(id));
  return d;
}

static void sh2_peripheral_write8(u32 a, u32 d, int id)
{
  u8 *r = (void *)Pico32xMem->sh2_peri_regs[id];
  elprintf(EL_32X, "%csh2 peri w8  [%08x]       %02x @%06x", id ? 's' : 'm', a, d, sh2_pc(id));

  a &= 0x1ff;
  r[a] = d;
}

static void sh2_peripheral_write32(u32 a, u32 d, int id)
{
  u32 *r = Pico32xMem->sh2_peri_regs[id];
  elprintf(EL_32X, "%csh2 peri w32 [%08x] %08x @%06x", id ? 's' : 'm', a, d, sh2_pc(id));

  a &= 0x1fc;
  r[a / 4] = d;

  switch (a) {
    // division unit (TODO: verify):
    case 0x104: // DVDNT: divident L, starts divide
      elprintf(EL_32X, "%csh2 divide %08x / %08x", id ? 's' : 'm', d, r[0x100 / 4]);
      if (r[0x100 / 4]) {
        signed int divisor = r[0x100 / 4];
                       r[0x118 / 4] = r[0x110 / 4] = (signed int)d % divisor;
        r[0x104 / 4] = r[0x11c / 4] = r[0x114 / 4] = (signed int)d / divisor;
      }
      break;
    case 0x114:
      elprintf(EL_32X, "%csh2 divide %08x%08x / %08x @%08x",
        id ? 's' : 'm', r[0x110 / 4], d, r[0x100 / 4], sh2_pc(id));
      if (r[0x100 / 4]) {
        signed long long divident = (signed long long)r[0x110 / 4] << 32 | d;
        signed int divisor = r[0x100 / 4];
        // XXX: undocumented mirroring to 0x118,0x11c?
        r[0x118 / 4] = r[0x110 / 4] = divident % divisor;
        r[0x11c / 4] = r[0x114 / 4] = divident / divisor;
      }
      break;
  }

  if ((a == 0x1b0 || a == 0x18c) && (dmac0->chcr0 & 3) == 1 && (dmac0->dmaor & 1)) {
    elprintf(EL_32X, "sh2 DMA %08x -> %08x, cnt %d, chcr %04x @%06x",
      dmac0->sar0, dmac0->dar0, dmac0->tcr0, dmac0->chcr0, sh2_pc(id));
    dmac0->tcr0 &= 0xffffff;

    // HACK: assume 68k starts writing soon and end the timeslice
    ash2_end_run(16);

    // DREQ is only sent after first 4 words are written.
    // we do multiple of 4 words to avoid messing up alignment
    if (dmac0->sar0 == 0x20004012 && Pico32x.dmac_ptr && (Pico32x.dmac_ptr & 3) == 0) {
      elprintf(EL_32X, "68k -> sh2 DMA");
      dma_68k2sh2_do();
    }
  }
}

// ------------------------------------------------------------------
// default 32x handlers
u32 PicoRead8_32x(u32 a)
{
  u32 d = 0;
  if ((a & 0xffc0) == 0x5100) { // a15100
    d = p32x_reg_read16(a);
    goto out_16to8;
  }

  if (!(Pico32x.regs[0] & 1))
    goto no_vdp;

  if ((a & 0xfff0) == 0x5180) { // a15180
    d = p32x_vdp_read16(a);
    goto out_16to8;
  }

  if ((a & 0xfe00) == 0x5200) { // a15200
    d = Pico32xMem->pal[(a & 0x1ff) / 2];
    goto out_16to8;
  }

no_vdp:
  if ((a & 0xfffc) == 0x30ec) { // a130ec
    d = str_mars[a & 3];
    goto out;
  }

  elprintf(EL_UIO, "m68k unmapped r8  [%06x] @%06x", a, SekPc);
  return d;

out_16to8:
  if (a & 1)
    d &= 0xff;
  else
    d >>= 8;

out:
  elprintf(EL_32X, "m68k 32x r8  [%06x]   %02x @%06x", a, d, SekPc);
  return d;
}

u32 PicoRead16_32x(u32 a)
{
  u32 d = 0;
  if ((a & 0xffc0) == 0x5100) { // a15100
    d = p32x_reg_read16(a);
    goto out;
  }

  if (!(Pico32x.regs[0] & 1))
    goto no_vdp;

  if ((a & 0xfff0) == 0x5180) { // a15180
    d = p32x_vdp_read16(a);
    goto out;
  }

  if ((a & 0xfe00) == 0x5200) { // a15200
    d = Pico32xMem->pal[(a & 0x1ff) / 2];
    goto out;
  }

no_vdp:
  if ((a & 0xfffc) == 0x30ec) { // a130ec
    d = !(a & 2) ? ('M'<<8)|'A' : ('R'<<8)|'S';
    goto out;
  }

  elprintf(EL_UIO, "m68k unmapped r16 [%06x] @%06x", a, SekPc);
  return d;

out:
  elprintf(EL_32X, "m68k 32x r16 [%06x] %04x @%06x", a, d, SekPc);
  return d;
}

void PicoWrite8_32x(u32 a, u32 d)
{
  if ((a & 0xfc00) == 0x5000)
    elprintf(EL_32X, "m68k 32x w8  [%06x]   %02x @%06x", a, d & 0xff, SekPc);

  if ((a & 0xffc0) == 0x5100) { // a15100
    p32x_reg_write8(a, d);
    return;
  }

  if (!(Pico32x.regs[0] & 1))
    goto no_vdp;

  if ((a & 0xfff0) == 0x5180) { // a15180
    p32x_vdp_write8(a, d);
    return;
  }

  // TODO: verify
  if ((a & 0xfe00) == 0x5200) { // a15200
    elprintf(EL_32X|EL_ANOMALY, "m68k 32x PAL w8  [%06x]   %02x @%06x", a, d & 0xff, SekPc);
    ((u8 *)Pico32xMem->pal)[(a & 0x1ff) ^ 1] = d;
    Pico32x.dirty_pal = 1;
    return;
  }

no_vdp:
  elprintf(EL_UIO, "m68k unmapped w8  [%06x]   %02x @%06x", a, d & 0xff, SekPc);
}

void PicoWrite16_32x(u32 a, u32 d)
{
  if ((a & 0xfc00) == 0x5000)
    elprintf(EL_UIO, "m68k 32x w16 [%06x] %04x @%06x", a, d & 0xffff, SekPc);

  if ((a & 0xffc0) == 0x5100) { // a15100
    p32x_reg_write16(a, d);
    return;
  }

  if (!(Pico32x.regs[0] & 1))
    goto no_vdp;

  if ((a & 0xfff0) == 0x5180) { // a15180
    p32x_vdp_write16(a, d);
    return;
  }

  if ((a & 0xfe00) == 0x5200) { // a15200
    Pico32xMem->pal[(a & 0x1ff) / 2] = d;
    Pico32x.dirty_pal = 1;
    return;
  }

no_vdp:
  elprintf(EL_UIO, "m68k unmapped w16 [%06x] %04x @%06x", a, d & 0xffff, SekPc);
}

// hint vector is writeable
static void PicoWrite8_hint(u32 a, u32 d)
{
  if ((a & 0xfffc) == 0x0070) {
    Pico32xMem->m68k_rom[a ^ 1] = d;
    return;
  }

  elprintf(EL_UIO, "m68k unmapped w8  [%06x]   %02x @%06x", a, d & 0xff, SekPc);
}

static void PicoWrite16_hint(u32 a, u32 d)
{
  if ((a & 0xfffc) == 0x0070) {
    ((u16 *)Pico32xMem->m68k_rom)[a/2] = d;
    return;
  }

  elprintf(EL_UIO, "m68k unmapped w16 [%06x] %04x @%06x", a, d & 0xffff, SekPc);
}

void Pico32xSwapDRAM(int b)
{
  cpu68k_map_set(m68k_read8_map,   0x840000, 0x85ffff, Pico32xMem->dram[b], 0);
  cpu68k_map_set(m68k_read16_map,  0x840000, 0x85ffff, Pico32xMem->dram[b], 0);
  cpu68k_map_set(m68k_write8_map,  0x840000, 0x85ffff, Pico32xMem->dram[b], 0);
  cpu68k_map_set(m68k_write16_map, 0x840000, 0x85ffff, Pico32xMem->dram[b], 0);
}

static void bank_switch(int b)
{
  unsigned int rs, bank;

  bank = b << 20;
  if (bank >= Pico.romsize) {
    elprintf(EL_32X|EL_ANOMALY, "missing bank @ %06x", bank);
    return;
  }

  // 32X ROM (unbanked, XXX: consider mirroring?)
  rs = (Pico.romsize + M68K_BANK_MASK) & ~M68K_BANK_MASK;
  rs -= bank;
  if (rs > 0x100000)
    rs = 0x100000;
  cpu68k_map_set(m68k_read8_map,   0x900000, 0x900000 + rs - 1, Pico.rom + bank, 0);
  cpu68k_map_set(m68k_read16_map,  0x900000, 0x900000 + rs - 1, Pico.rom + bank, 0);

  elprintf(EL_32X, "bank %06x-%06x -> %06x", 0x900000, 0x900000 + rs - 1, bank);
}

// -----------------------------------------------------------------
//                              SH2  
// -----------------------------------------------------------------

u32 p32x_sh2_read8(u32 a, int id)
{
  u32 d = 0;

  if (id == 0 && a < sizeof(Pico32xMem->sh2_rom_m))
    return Pico32xMem->sh2_rom_m[a ^ 1];
  if (id == 1 && a < sizeof(Pico32xMem->sh2_rom_s))
    return Pico32xMem->sh2_rom_s[a ^ 1];

  if ((a & 0xdffc0000) == 0x06000000)
    return Pico32xMem->sdram[(a & 0x3ffff) ^ 1];

  if ((a & 0xdfc00000) == 0x02000000)
    if ((a & 0x003fffff) < Pico.romsize)
      return Pico.rom[(a & 0x3fffff) ^ 1];

  if ((a & ~0xfff) == 0xc0000000)
    return Pico32xMem->data_array[id][(a & 0xfff) ^ 1];

  if ((a & 0xdffc0000) == 0x04000000) {
    /* XXX: overwrite readable as normal? */
    u8 *dram = (u8 *)Pico32xMem->dram[(Pico32x.vdp_regs[0x0a/2] & P32XV_FS) ^ 1];
    return dram[(a & 0x1ffff) ^ 1];
  }

  if ((a & 0xdfffff00) == 0x4000) {
    d = p32x_sh2reg_read16(a, id);
    goto out_16to8;
  }

  if ((a & 0xdfffff00) == 0x4100) {
    d = p32x_vdp_read16(a);
    if (p32x_poll_detect(&sh2_poll[id], a, ash2_cycles_done(), 1))
      ash2_end_run(8);
    goto out_16to8;
  }

  if ((a & 0xdfffff00) == 0x4200) {
    d = Pico32xMem->pal[(a & 0x1ff) / 2];
    goto out_16to8;
  }

  if ((a & 0xfffffe00) == 0xfffffe00)
    return sh2_peripheral_read8(a, id);

  elprintf(EL_UIO, "%csh2 unmapped r8  [%08x]       %02x @%06x",
    id ? 's' : 'm', a, d, sh2_pc(id));
  return d;

out_16to8:
  if (a & 1)
    d &= 0xff;
  else
    d >>= 8;

  elprintf(EL_32X, "%csh2 r8  [%08x]       %02x @%06x",
    id ? 's' : 'm', a, d, sh2_pc(id));
  return d;
}

u32 p32x_sh2_read16(u32 a, int id)
{
  u32 d = 0;

  if (id == 0 && a < sizeof(Pico32xMem->sh2_rom_m))
    return *(u16 *)(Pico32xMem->sh2_rom_m + a);
  if (id == 1 && a < sizeof(Pico32xMem->sh2_rom_s))
    return *(u16 *)(Pico32xMem->sh2_rom_s + a);

  if ((a & 0xdffc0000) == 0x06000000)
    return ((u16 *)Pico32xMem->sdram)[(a & 0x3ffff) / 2];

  if ((a & 0xdfc00000) == 0x02000000)
    if ((a & 0x003fffff) < Pico.romsize)
      return ((u16 *)Pico.rom)[(a & 0x3fffff) / 2];

  if ((a & ~0xfff) == 0xc0000000)
    return ((u16 *)Pico32xMem->data_array[id])[(a & 0xfff) / 2];

  if ((a & 0xdffe0000) == 0x04000000)
    return Pico32xMem->dram[(Pico32x.vdp_regs[0x0a/2] & P32XV_FS) ^ 1][(a & 0x1ffff) / 2];

  if ((a & 0xdfffff00) == 0x4000) {
    d = p32x_sh2reg_read16(a, id);
    if (!(EL_LOGMASK & EL_PWM) && (a & 0x30) == 0x30) // hide PWM
      return d;
    goto out;
  }

  if ((a & 0xdfffff00) == 0x4100) {
    d = p32x_vdp_read16(a);
    if (p32x_poll_detect(&sh2_poll[id], a, ash2_cycles_done(), 1))
      ash2_end_run(8);
    goto out;
  }

  if ((a & 0xdfffff00) == 0x4200) {
    d = Pico32xMem->pal[(a & 0x1ff) / 2];
    goto out;
  }

  elprintf(EL_UIO, "%csh2 unmapped r16 [%08x]     %04x @%06x",
    id ? 's' : 'm', a, d, sh2_pc(id));
  return d;

out:
  elprintf(EL_32X, "%csh2 r16 [%08x]     %04x @%06x",
    id ? 's' : 'm', a, d, sh2_pc(id));
  return d;
}

u32 p32x_sh2_read32(u32 a, int id)
{
  if ((a & 0xfffffe00) == 0xfffffe00)
    return sh2_peripheral_read32(a, id);

//  elprintf(EL_UIO, "sh2 r32 [%08x] %08x @%06x", a, d, ash2_pc());
  return (p32x_sh2_read16(a, id) << 16) | p32x_sh2_read16(a + 2, id);
}

void p32x_sh2_write8(u32 a, u32 d, int id)
{
  if ((a & 0xdffffc00) == 0x4000)
    elprintf(EL_32X, "%csh2 w8  [%08x]       %02x @%06x",
      id ? 's' : 'm', a, d & 0xff, sh2_pc(id));

  if ((a & 0xdffc0000) == 0x06000000) {
    Pico32xMem->sdram[(a & 0x3ffff) ^ 1] = d;
    return;
  }

  if ((a & 0xdffc0000) == 0x04000000) {
    u8 *dram;
    if (!(a & 0x20000) || d) {
      dram = (u8 *)Pico32xMem->dram[(Pico32x.vdp_regs[0x0a/2] & P32XV_FS) ^ 1];
      dram[(a & 0x1ffff) ^ 1] = d;
    }
    return;
  }

  if ((a & ~0xfff) == 0xc0000000) {
    Pico32xMem->data_array[id][(a & 0xfff) ^ 1] = d;
    return;
  }

  if ((a & 0xdfffff00) == 0x4100) {
    p32x_vdp_write8(a, d);
    return;
  }

  if ((a & 0xdfffff00) == 0x4000) {
    p32x_sh2reg_write8(a, d, id);
    return;
  }

  if ((a & 0xfffffe00) == 0xfffffe00) {
    sh2_peripheral_write8(a, d, id);
    return;
  }

  elprintf(EL_UIO, "%csh2 unmapped w8  [%08x]       %02x @%06x",
    id ? 's' : 'm', a, d & 0xff, sh2_pc(id));
}

void p32x_sh2_write16(u32 a, u32 d, int id)
{
  if ((a & 0xdffffc00) == 0x4000 && ((EL_LOGMASK & EL_PWM) || (a & 0x30) != 0x30)) // hide PWM
    elprintf(EL_32X, "%csh2 w16 [%08x]     %04x @%06x",
      id ? 's' : 'm', a, d & 0xffff, sh2_pc(id));

  // ignore "Associative purge space"
  if ((a & 0xf8000000) == 0x40000000)
    return;

  if ((a & 0xdffc0000) == 0x06000000) {
    ((u16 *)Pico32xMem->sdram)[(a & 0x3ffff) / 2] = d;
    return;
  }

  if ((a & ~0xfff) == 0xc0000000) {
    ((u16 *)Pico32xMem->data_array[id])[(a & 0xfff) / 2] = d;
    return;
  }

  if ((a & 0xdffc0000) == 0x04000000) {
    u16 *pd = &Pico32xMem->dram[(Pico32x.vdp_regs[0x0a/2] & P32XV_FS) ^ 1][(a & 0x1ffff) / 2];
    if (!(a & 0x20000)) {
      *pd = d;
      return;
    }
    // overwrite
    if (!(d & 0xff00)) d |= *pd & 0xff00;
    if (!(d & 0x00ff)) d |= *pd & 0x00ff;
    *pd = d;
    return;
  }

  if ((a & 0xdfffff00) == 0x4100) {
    sh2_poll[id].cnt = 0; // for poll before VDP accesses
    p32x_vdp_write16(a, d);
    return;
  }

  if ((a & 0xdffffe00) == 0x4200) {
    Pico32xMem->pal[(a & 0x1ff) / 2] = d;
    Pico32x.dirty_pal = 1;
    return;
  }

  if ((a & 0xdfffff00) == 0x4000) {
    p32x_sh2reg_write16(a, d, id);
    return;
  }

  elprintf(EL_UIO, "%csh2 unmapped w16 [%08x]     %04x @%06x",
    id ? 's' : 'm', a, d & 0xffff, sh2_pc(id));
}

void p32x_sh2_write32(u32 a, u32 d, int id)
{
  if ((a & 0xfffffe00) == 0xfffffe00) {
    sh2_peripheral_write32(a, d, id);
    return;
  }

  p32x_sh2_write16(a, d >> 16, id);
  p32x_sh2_write16(a + 2, d, id);
}

#define HWSWAP(x) (((x) << 16) | ((x) >> 16))
void PicoMemSetup32x(void)
{
  unsigned short *ps;
  unsigned int *pl;
  unsigned int rs;
  int i;

  Pico32xMem = calloc(1, sizeof(*Pico32xMem));
  if (Pico32xMem == NULL) {
    elprintf(EL_STATUS, "OOM");
    return;
  }

  dmac0 = (void *)&Pico32xMem->sh2_peri_regs[0][0x180 / 4];

  // generate 68k ROM
  ps = (unsigned short *)Pico32xMem->m68k_rom;
  pl = (unsigned int *)Pico32xMem->m68k_rom;
  for (i = 1; i < 0xc0/4; i++)
    pl[i] = HWSWAP(0x880200 + (i - 1) * 6);

  // fill with nops
  for (i = 0xc0/2; i < 0x100/2; i++)
    ps[i] = 0x4e71;

#if 0
  ps[0xc0/2] = 0x46fc;
  ps[0xc2/2] = 0x2700; // move #0x2700,sr
  ps[0xfe/2] = 0x60fe; // jump to self
#else
  ps[0xfe/2] = 0x4e75; // rts
#endif

  // fill remaining mem with ROM
  memcpy(Pico32xMem->m68k_rom + 0x100, Pico.rom + 0x100, sizeof(Pico32xMem->m68k_rom) - 0x100);

  // 32X ROM
  // TODO: move
  {
    FILE *f = fopen("32X_M_BIOS.BIN", "rb");
    int i;
    if (f == NULL) {
      printf("missing 32X_M_BIOS.BIN\n");
      exit(1);
    }
    fread(Pico32xMem->sh2_rom_m, 1, sizeof(Pico32xMem->sh2_rom_m), f);
    fclose(f);
    f = fopen("32X_S_BIOS.BIN", "rb");
    if (f == NULL) {
      printf("missing 32X_S_BIOS.BIN\n");
      exit(1);
    }
    fread(Pico32xMem->sh2_rom_s, 1, sizeof(Pico32xMem->sh2_rom_s), f);
    fclose(f);
    // byteswap
    for (i = 0; i < sizeof(Pico32xMem->sh2_rom_m); i += 2) {
      int t = Pico32xMem->sh2_rom_m[i];
      Pico32xMem->sh2_rom_m[i] = Pico32xMem->sh2_rom_m[i + 1];
      Pico32xMem->sh2_rom_m[i + 1] = t;
    }
    for (i = 0; i < sizeof(Pico32xMem->sh2_rom_s); i += 2) {
      int t = Pico32xMem->sh2_rom_s[i];
      Pico32xMem->sh2_rom_s[i] = Pico32xMem->sh2_rom_s[i + 1];
      Pico32xMem->sh2_rom_s[i + 1] = t;
    }
  }

  // cartridge area becomes unmapped
  // XXX: we take the easy way and don't unmap ROM,
  // so that we can avoid handling the RV bit.
  // m68k_map_unmap(0x000000, 0x3fffff);

  // MD ROM area
  rs = sizeof(Pico32xMem->m68k_rom);
  cpu68k_map_set(m68k_read8_map,   0x000000, rs - 1, Pico32xMem->m68k_rom, 0);
  cpu68k_map_set(m68k_read16_map,  0x000000, rs - 1, Pico32xMem->m68k_rom, 0);
  cpu68k_map_set(m68k_write8_map,  0x000000, rs - 1, PicoWrite8_hint, 1); // TODO verify
  cpu68k_map_set(m68k_write16_map, 0x000000, rs - 1, PicoWrite16_hint, 1);

  // DRAM area
  Pico32xSwapDRAM(1);

  // 32X ROM (unbanked, XXX: consider mirroring?)
  rs = (Pico.romsize + M68K_BANK_MASK) & ~M68K_BANK_MASK;
  if (rs > 0x80000)
    rs = 0x80000;
  cpu68k_map_set(m68k_read8_map,   0x880000, 0x880000 + rs - 1, Pico.rom, 0);
  cpu68k_map_set(m68k_read16_map,  0x880000, 0x880000 + rs - 1, Pico.rom, 0);

  // 32X ROM (banked)
  bank_switch(0);

  // setup poll detector
  m68k_poll.flag = P32XF_68KPOLL;
  m68k_poll.cyc_max = 64;
  sh2_poll[0].flag = P32XF_MSH2POLL;
  sh2_poll[0].cyc_max = 16;
  sh2_poll[1].flag = P32XF_SSH2POLL;
  sh2_poll[1].cyc_max = 16;
}

