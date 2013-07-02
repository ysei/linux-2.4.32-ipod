/*
 * linux/arch/h8300/platform/h8s/ints_h8s.c
 * Interrupt handling CPU variants
 *
 * Yoshinori Sato <ysato@users.sourceforge.jp>
 *
 */

#include <linux/config.h>
#include <linux/init.h>
#include <linux/errno.h>

#include <asm/ptrace.h>
#include <asm/traps.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <asm/regs267x.h>

/* saved vector list */
const int __initdata h8300_saved_vectors[]={
#if defined(CONFIG_GDB_DEBUG)
	TRACE_VEC,
	TRAP3_VEC,
#endif
	-1
};

/* trap entry table */
const unsigned long __initdata h8300_trap_table[NR_TRAPS]={
	0,0,0,0,0,
	(unsigned long)trace_break,  /* TRACE */
	0,0,
	(unsigned long)system_call,  /* TRAPA #0 */
	0,0,0,0,0,0,0
};

/* IRQ pin assignment */
struct irq_pins {
	unsigned char port_no;
	unsigned char bit_no;
};
/* ISTR = 0 */
const static struct irq_pins irq_assign_table0[16]={
        {H8300_GPIO_P5,H8300_GPIO_B0},{H8300_GPIO_P5,H8300_GPIO_B1},
	{H8300_GPIO_P5,H8300_GPIO_B2},{H8300_GPIO_P5,H8300_GPIO_B3},
	{H8300_GPIO_P5,H8300_GPIO_B4},{H8300_GPIO_P5,H8300_GPIO_B5},
	{H8300_GPIO_P5,H8300_GPIO_B6},{H8300_GPIO_P5,H8300_GPIO_B7},
	{H8300_GPIO_P6,H8300_GPIO_B0},{H8300_GPIO_P6,H8300_GPIO_B1},
	{H8300_GPIO_P6,H8300_GPIO_B2},{H8300_GPIO_P6,H8300_GPIO_B3},
	{H8300_GPIO_P6,H8300_GPIO_B4},{H8300_GPIO_P6,H8300_GPIO_B5},
	{H8300_GPIO_PF,H8300_GPIO_B1},{H8300_GPIO_PF,H8300_GPIO_B2},
};
/* ISTR = 1 */
const static struct irq_pins irq_assign_table1[16]={
	{H8300_GPIO_P8,H8300_GPIO_B0},{H8300_GPIO_P8,H8300_GPIO_B1},
	{H8300_GPIO_P8,H8300_GPIO_B2},{H8300_GPIO_P8,H8300_GPIO_B3},
	{H8300_GPIO_P8,H8300_GPIO_B4},{H8300_GPIO_P8,H8300_GPIO_B5},
	{H8300_GPIO_PH,H8300_GPIO_B2},{H8300_GPIO_PH,H8300_GPIO_B3},
	{H8300_GPIO_P2,H8300_GPIO_B0},{H8300_GPIO_P2,H8300_GPIO_B1},
	{H8300_GPIO_P2,H8300_GPIO_B2},{H8300_GPIO_P2,H8300_GPIO_B3},
	{H8300_GPIO_P2,H8300_GPIO_B4},{H8300_GPIO_P2,H8300_GPIO_B5},
	{H8300_GPIO_P2,H8300_GPIO_B6},{H8300_GPIO_P2,H8300_GPIO_B7},
};

int h8300_enable_irq_pin(unsigned int irq)
{
	if (irq >= EXT_IRQ0 && irq <= EXT_IRQ15) {
		unsigned short ptn = 1 << (irq - EXT_IRQ0);
		unsigned int port_no,bit_no;
		if (*(volatile unsigned short *)ITSR & ptn) {
			port_no = irq_assign_table1[irq - EXT_IRQ0].port_no;
			bit_no = irq_assign_table1[irq - EXT_IRQ0].bit_no;
		} else {
			port_no = irq_assign_table0[irq - EXT_IRQ0].port_no;
			bit_no = irq_assign_table0[irq - EXT_IRQ0].bit_no;
		}
		if (H8300_GPIO_RESERVE(port_no, bit_no) == 0)
			return -EBUSY;                   /* pin already use */
		H8300_GPIO_DDR(port_no, bit_no, H8300_GPIO_INPUT);
		*(volatile unsigned short *)ISR &= ~ptn; /* ISR clear */
	}		
	return 0;
}

void h8300_disable_irq_pin(unsigned int irq)
{
	if (irq >= EXT_IRQ0 && irq <= EXT_IRQ15) {
		/* disable interrupt & release IRQ pin */
		unsigned short port_no,bit_no;
		*(volatile unsigned short *)ISR &= ~(1 << (irq - EXT_IRQ0));
		*(volatile unsigned short *)IER &= ~(1 << (irq - EXT_IRQ0));
		if (*(volatile unsigned short *)ITSR & (1 << (irq - EXT_IRQ0))) {
			port_no = irq_assign_table1[irq - EXT_IRQ0].port_no;
			bit_no = irq_assign_table1[irq - EXT_IRQ0].bit_no;
		} else {
			port_no = irq_assign_table0[irq - EXT_IRQ0].port_no;
			bit_no = irq_assign_table0[irq - EXT_IRQ0].bit_no;
		}
		H8300_GPIO_FREE(port_no, bit_no);
	}
}
