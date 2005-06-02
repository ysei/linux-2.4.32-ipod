/*
 * TQM8xx(L) board specific definitions
 *
 * Copyright (c) 1999,2000,2001 Wolfgang Denk (wd@denx.de)
 */

#ifndef __MACH_TQM8xx_H
#define __MACH_TQM8xx_H

#include <linux/config.h>

#include <asm/ppcboot.h>

#define	TQM_IMMR_BASE	0xFFF00000	/* phys. addr of IMMR */
#define	TQM_IMAP_SIZE	(64 * 1024)	/* size of mapped area */

#define	IMAP_ADDR	TQM_IMMR_BASE	/* physical base address of IMMR area */
#define IMAP_SIZE	TQM_IMAP_SIZE	/* mapped size of IMMR area */

/*-----------------------------------------------------------------------
 * PCMCIA stuff
 *-----------------------------------------------------------------------
 *
 */
#define PCMCIA_MEM_SIZE		( 64 << 20 )

#define	MAX_HWIFS	1	/* overwrite default in include/asm-ppc/ide.h */

/*
 * Definitions for IDE0 Interface
 */
#define IDE0_BASE_OFFSET		0
#define IDE0_DATA_REG_OFFSET		(PCMCIA_MEM_SIZE + 0x320)
#define IDE0_ERROR_REG_OFFSET		(2 * PCMCIA_MEM_SIZE + 0x320 + 1)
#define IDE0_NSECTOR_REG_OFFSET		(2 * PCMCIA_MEM_SIZE + 0x320 + 2)
#define IDE0_SECTOR_REG_OFFSET		(2 * PCMCIA_MEM_SIZE + 0x320 + 3)
#define IDE0_LCYL_REG_OFFSET		(2 * PCMCIA_MEM_SIZE + 0x320 + 4)
#define IDE0_HCYL_REG_OFFSET		(2 * PCMCIA_MEM_SIZE + 0x320 + 5)
#define IDE0_SELECT_REG_OFFSET		(2 * PCMCIA_MEM_SIZE + 0x320 + 6)
#define IDE0_STATUS_REG_OFFSET		(2 * PCMCIA_MEM_SIZE + 0x320 + 7)
#define IDE0_CONTROL_REG_OFFSET		0x0106
#define IDE0_IRQ_REG_OFFSET		0x000A	/* not used */

#define	IDE0_INTERRUPT			13


/* We don't use the 8259.
*/
#define NR_8259_INTS	0

#endif	/* __MACH_TQM8xx_H */
