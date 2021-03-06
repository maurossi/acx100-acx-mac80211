#ifndef _INLINES_H_
#define _INLINES_H_

/* ##################################################
 * BOM Data Access
 * Locking in mem
 *
 * Locking in mem is more complex than for pci, because the different
 * data-access functions below need to be protected against incoming
 * interrupts.
 *
 * Data-access on the mem device is always going in several non-atomic
 * steps, involving 2 or more register writes (e.g. ACX_SLV_REG_ADDR,
 * ACX_SLV_REG_DATA).
 *
 * If an interrupt is serviced while a data-access function is
 * ongoing, this may cause access interference with the involved
 * operations, since the irq routine is also using the same
 * data-access functions.
 *
 * In case of interference, this often manifests during driver
 * operations as failure of device cmds and subsequent hanging of the
 * device. It especially appeared during sw-scans while a connection
 * was up.
 *
 * For this reason, irqs shall be off while data access functions are
 * executed, and for this we'll use the acx-spinlock.
 *
 * In pci we don't have this problem, because all data-access
 * functions are atomic enough and we use dma (and the sw-scan problem
 * is also not observed in pci, which indicates confirmation).
 *
 * Apart from this, the pure acx-sem locking is already coordinating
 * accesses well enough, that simple driver operation without
 * inbetween scans work without problems.
 *
 * Different locking approaches are possible to solve this (e.g. fine
 * vs coarse-grained).
 *
 * The chosen approach is:
 *
 * 1) Mem.c data-access functions all contain a check to insure that
 * they are executed under the acx-spinlock.  => This is the red line
 * that tells, if something needs coverage.
 *
 * 2) The scope of acx-spinlocking is local, in this case here only to
 * mem.c.  All common.c functions are already protected by the sem.
 *
 * 3) In order to consolidate locking calls and also to account for
 * the logic of the various write_flush() calls around, locking in mem
 * should be:
 *
 * a) as coarse-grained as possible, and ...
 *
 * b) ... as fine-grained as required.  Basically that means that
 * before functions that sleep are called, unlocking needs to be done.
 * And locks are taken again inside the sleeping function.
 * Specifically the cmd-functions are used in this path.
 *
 * Once stable, the locking checks in the data-access functions could
 * be #defined away.  Mem.c is anyway more used two smaller cpus (pxa
 * UP e.g.), so the implied runtime constraints by the lock won't take
 * much effect.
 */

#define acxmem_lock_flags	unsigned long flags = 0
#define acxmem_lock()						\
	if (IS_MEM(adev))					\
		 spin_lock_irqsave(&adev->spinlock, flags); 	\
	else							\
		__acquire(&adev->spinlock)

#define acxmem_unlock()						\
	if (IS_MEM(adev))					\
		 spin_unlock_irqrestore(&adev->spinlock, flags);\
	else							\
		__release(&adev->spinlock)

/* Endianess: read[lw], write[lw] do little-endian conversion internally */
#define acx_readl(v)		readl((v))
#define acx_readw(v)		readw((v))
#define acx_readb(v)		readb((v))
#define acx_writel(v, r)	writel((v), (r))
#define acx_writew(v, r)	writew((v), (r))
#define acx_writeb(v, r)	writeb((v), (r))

/* This controls checking of spin-locking in the mem-interface */
#define ACXMEM_SPIN_CHECK 0

#if ACXMEM_SPIN_CHECK
#define ACXMEM_WARN_NOT_SPIN_LOCKED					\
	if (!spin_is_locked(&adev->spinlock)) {				\
		logf0(L_ANY, "mem: warning: data access not locked!\n");\
		dump_stack();						\
	}
#else
#define ACXMEM_WARN_NOT_SPIN_LOCKED do { } while (0)
#endif

enum acxreg_t {
	ACX_SOFT_RESET	  = 0x0000,
	ACX_SLV_REG_ADDR  = 0x0004,
	ACX_SLV_REG_DATA  = 0x0008,
	ACX_SLV_REG_ADATA = 0x000c,
	ACX_SLV_MEM_CP    = 0x0010,
	ACX_SLV_MEM_ADDR  = 0x0014, /*redundant with IO_ACX_SLV_MEM_ADDR */
	ACX_SLV_MEM_DATA  = 0x0018, /*redundant with IO_ACX_SLV_MEM_DATA*/
	ACX_SLV_MEM_CTL   = 0x001c, /*redundant with IO_ACX_SLV_END_CTL */
};

#define INLINE_IO static inline

INLINE_IO u32 read_id_register(acx_device_t *adev)
{
	ACXMEM_WARN_NOT_SPIN_LOCKED;
	acx_writel(0x24, adev->iobase + ACX_SLV_REG_ADDR);
	return acx_readl(adev->iobase + ACX_SLV_REG_DATA);
}

#define check_IO_ACX_ECPU_CTRL(adev, addr, offset)	\
	if (offset > IO_ACX_ECPU_CTRL)			\
		addr = offset;				\
	else						\
		addr = adev->io[offset];

/* note the buried return */
#define ret_addr_lt20_rd_(adev, addr, _lwb)			\
	if (addr < 0x20)					\
		return acx_read##_lwb(adev->iobase + addr);

#define ret_addr_lt20_rdl(adev, addr)	ret_addr_lt20_rd_(adev, addr, l)
#define ret_addr_lt20_rdw(adev, addr)	ret_addr_lt20_rd_(adev, addr, w)
#define ret_addr_lt20_rdb(adev, addr)	ret_addr_lt20_rd_(adev, addr, b)

/* note the buried return */
#define ret_addr_lt20_wr_(adev, addr, _lwb, val)		\
	if (addr < 0x20) {					\
		acx_write##_lwb(val, adev->iobase + addr);	\
		return;						\
	}

#define ret_addr_lt20_wrl(adev, addr, val)	\
	ret_addr_lt20_wr_(adev, addr, l, val)
#define ret_addr_lt20_wrw(adev, addr, val)	\
	ret_addr_lt20_wr_(adev, addr, w, val)
#define ret_addr_lt20_wrb(adev, addr, val)	\
	ret_addr_lt20_wr_(adev, addr, b, val)

INLINE_IO u32 read_reg32(acx_device_t *adev, unsigned int offset)
{
	u32 val;
	u32 addr;

	if (IS_PCI(adev)) {
		#if ACX_IO_WIDTH == 32
		return acx_readl(adev->iobase + adev->io[offset]);
		#else
		return acx_readw(adev->iobase + adev->io[offset])
		    + (acx_readw(adev->iobase + adev->io[offset] + 2) << 16);
		#endif
	}
	/* else IS_MEM */

	ACXMEM_WARN_NOT_SPIN_LOCKED;
	check_IO_ACX_ECPU_CTRL(adev, addr, offset);
	ret_addr_lt20_rdl(adev, addr);

	acx_writel(addr, adev->iobase + ACX_SLV_REG_ADDR);
	val = acx_readl(adev->iobase + ACX_SLV_REG_DATA);

	return val;
}

INLINE_IO u16 read_reg16(acx_device_t *adev, unsigned int offset)
{
	u16 lo;
	u32 addr;

	if (IS_PCI(adev))
		return acx_readw(adev->iobase + adev->io[offset]);

	/* else IS_MEM */

	ACXMEM_WARN_NOT_SPIN_LOCKED;
	check_IO_ACX_ECPU_CTRL(adev, addr, offset);
	ret_addr_lt20_rdw(adev, addr);

	acx_writel(addr, adev->iobase + ACX_SLV_REG_ADDR);
	lo = acx_readw(adev->iobase + ACX_SLV_REG_DATA);

	return lo;
}

INLINE_IO u8 read_reg8(acx_device_t *adev, unsigned int offset)
{
	u8 lo;
	u32 addr;

	if (IS_PCI(adev))
		return readb(adev->iobase + adev->io[offset]);

	/* else IS_MEM */

	ACXMEM_WARN_NOT_SPIN_LOCKED;
	check_IO_ACX_ECPU_CTRL(adev, addr, offset);
	ret_addr_lt20_rdb(adev, addr);

	acx_writel(addr, adev->iobase + ACX_SLV_REG_ADDR);
	lo = acx_readw(adev->iobase + ACX_SLV_REG_DATA);

	return (u8) lo;
}

INLINE_IO void write_reg32(acx_device_t *adev, unsigned int offset, u32 val)
{
	u32 addr;

	if (IS_PCI(adev)) {
		#if ACX_IO_WIDTH == 32
		acx_writel(val, adev->iobase + adev->io[offset]);
		#else
		acx_writew(val & 0xffff,
			(u8 *) adev->iobase + adev->io[offset]);
		acx_writew(val >> 16,
			(u8 *) adev->iobase + adev->io[offset] + 2);
		#endif
	}
	/* else IS_MEM */

	ACXMEM_WARN_NOT_SPIN_LOCKED;
	check_IO_ACX_ECPU_CTRL(adev, addr, offset);
	ret_addr_lt20_wrl(adev, addr, val);

	acx_writel(addr, adev->iobase + ACX_SLV_REG_ADDR);
	acx_writel(val, adev->iobase + ACX_SLV_REG_DATA);
}

INLINE_IO void write_reg16(acx_device_t *adev, unsigned int offset, u16 val)
{
	u32 addr;

	if (IS_PCI(adev)) {
		acx_writew(val, adev->iobase + adev->io[offset]);
		return;
	}
	/* else IS_MEM */

	ACXMEM_WARN_NOT_SPIN_LOCKED;
	check_IO_ACX_ECPU_CTRL(adev, addr, offset);
	ret_addr_lt20_wrw(adev, addr, val);

	acx_writel(addr, adev->iobase + ACX_SLV_REG_ADDR);
	acx_writew(val, adev->iobase + ACX_SLV_REG_DATA);
}

INLINE_IO void write_reg8(acx_device_t *adev, unsigned int offset, u8 val)
{
	u32 addr;

	if (IS_PCI(adev)) {
		writeb(val, adev->iobase + adev->io[offset]);
		return;
	}
	/* else IS_MEM */

	ACXMEM_WARN_NOT_SPIN_LOCKED;
	check_IO_ACX_ECPU_CTRL(adev, addr, offset);
	ret_addr_lt20_wrb(adev, addr, val);

	acx_writel(addr, adev->iobase + ACX_SLV_REG_ADDR);
	writeb(val, adev->iobase + ACX_SLV_REG_DATA);
}


/* Handle PCI posting properly:
 * Make sure that writes reach the adapter in case they require to be
 * executed *before* the next write, by reading a random (and safely
 * accessible) register.  This call has to be made if there is no read
 * following (which would flush the data to the adapter), yet the
 * written data has to reach the adapter immediately.
 */

INLINE_IO void write_flush(acx_device_t *adev)
{
	/* readb(adev->iobase + adev->io[IO_ACX_INFO_MAILBOX_OFFS]); */
	if (IS_PCI(adev)) {
		/* faster version (accesses the first register,
		 * IO_ACX_SOFT_RESET, which should also be safe): */
		readb(adev->iobase);
		return;
	}
	/* else IS_MEM */
	/* faster version (accesses the first register,
	 * IO_ACX_SOFT_RESET, which should also be safe): */
	ACXMEM_WARN_NOT_SPIN_LOCKED;
	(void) acx_readl(adev->iobase);
}

INLINE_IO void set_regbits(acx_device_t *adev, unsigned int offset,
			u32 bits)
{
	u32 tmp;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	tmp = read_reg32(adev, offset);
	tmp = tmp | bits;
	write_reg32(adev, offset, tmp);
	write_flush(adev);
}

INLINE_IO void clear_regbits(acx_device_t *adev, unsigned int offset, u32 bits)
{
	u32 tmp;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	tmp = read_reg32(adev, offset);
	tmp = tmp & ~bits;
	write_reg32(adev, offset, tmp);
	write_flush(adev);
}

/*
 * Copy from PXA memory to the ACX memory.  This assumes both the PXA
 * and ACX addresses are 32 bit aligned.  Count is in bytes.
 */
INLINE_IO void write_slavemem32(acx_device_t *adev, u32 slave_address, u32 val)
{
	ACXMEM_WARN_NOT_SPIN_LOCKED;

	write_reg32(adev, IO_ACX_SLV_MEM_CTL, 0x0);
	write_reg32(adev, IO_ACX_SLV_MEM_ADDR, slave_address);
	udelay(10);
	write_reg32(adev, IO_ACX_SLV_MEM_DATA, val);
}

INLINE_IO u32 read_slavemem32(acx_device_t *adev, u32 slave_address)
{
	u32 val;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	write_reg32(adev, IO_ACX_SLV_MEM_CTL, 0x0);
	write_reg32(adev, IO_ACX_SLV_MEM_ADDR, slave_address);
	udelay(10);
	val = read_reg32(adev, IO_ACX_SLV_MEM_DATA);

	return val;
}

INLINE_IO void write_slavemem8(acx_device_t *adev, u32 slave_address, u8 val)
{
	u32 data;
	u32 base;
	int offset;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	/*
	 * Get the word containing the target address and the byte
	 * offset in that word.
	 */
	base = slave_address & ~3;
	offset = (slave_address & 3) * 8;

	data = read_slavemem32(adev, base);
	data &= ~(0xff << offset);
	data |= val << offset;
	write_slavemem32(adev, base, data);
}

INLINE_IO u8 read_slavemem8(acx_device_t *adev, u32 slave_address)
{
	u8 val;
	u32 base;
	u32 data;
	int offset;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	base = slave_address & ~3;
	offset = (slave_address & 3) * 8;

	data = read_slavemem32(adev, base);

	val = (data >> offset) & 0xff;

	return val;
}

/*
 * doesn't split across word boundaries
 */
INLINE_IO void write_slavemem16(acx_device_t *adev, u32 slave_address, u16 val)
{
	u32 data;
	u32 base;
	int offset;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	/*
	 * Get the word containing the target address and the byte
	 * offset in that word.
	 */
	base = slave_address & ~3;
	offset = (slave_address & 3) * 8;

	data = read_slavemem32(adev, base);
	data &= ~(0xffff << offset);
	data |= val << offset;
	write_slavemem32(adev, base, data);
}

/*
 * doesn't split across word boundaries
 */
INLINE_IO u16 read_slavemem16(acx_device_t *adev, u32 slave_address)
{
	u16 val;
	u32 base;
	u32 data;
	int offset;

	ACXMEM_WARN_NOT_SPIN_LOCKED;

	base = slave_address & ~3;
	offset = (slave_address & 3) * 8;

	data = read_slavemem32(adev, base);

	val = (data >> offset) & 0xffff;

	return val;
}

#endif /* _INLINES_H_ */
