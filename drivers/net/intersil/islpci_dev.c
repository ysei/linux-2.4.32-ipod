/*  $Header$
 *  
 *  Copyright (C) 2002 Intersil Americas Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/version.h>
#ifdef MODULE
#ifdef MODVERSIONS
#include <linux/modversions.h>
#endif
#include <linux/module.h>
#else
#define MOD_INC_USE_COUNT
#define MOD_DEC_USE_COUNT
#endif

#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/etherdevice.h>

#include <asm/io.h>
#ifdef CONFIG_ARCH_ISL3893
#include <asm/arch/pci.h>
#endif
#include "isl_gen.h"
#include "isl_38xx.h"

#ifdef WIRELESS_IOCTLS
#include "isl_ioctl.h"
#endif

#include "islpci_dev.h"
#include "islpci_mgt.h"
#include "islpci_eth.h"
#include "islpci_hotplug.h"

#ifdef WDS_LINKS
#include "isl_mgt.h"
#include "isl_wds.h"
#endif

/******************************************************************************
        Global variable definition section
******************************************************************************/
extern int pc_debug;
extern int init_mode;

struct net_device *root_islpci_device = NULL;

int firmware_uploaded = 0;


/******************************************************************************
    Device Interrupt Handler
******************************************************************************/
void islpci_interrupt(int irq, void *config, struct pt_regs *regs)
{
    u32 reg;
    islpci_private *private_config = config;
    void *device = private_config->device_id;
    u16 status;
    
    // received an interrupt request on a shared IRQ line
    // first check whether the device is in sleep mode
    if( reg = readl( device + ISL38XX_CTRL_STAT_REG),
        reg & ISL38XX_CTRL_STAT_SLEEPMODE )
        // device is in sleep mode, IRQ was generated by someone else
        return;
    
    // lock the interrupt handler
    spin_lock( &private_config->slock );
     
#ifdef CONFIG_ARCH_ISL3893
    // check the interrupt source on the host registers
    reg = (u32) uPCI->ARMIntReg;
    reg &= (u32) uPCI->ARMIntEnReg;
    
//    pci_unmap_single( private_config->pci_device, private_config->control_block, 
//                    sizeof(struct isl38xx_cb), PCI_DMA_FROMDEVICE );
    flush_cache_all();

#else
    // check whether there is any source of interrupt on the device
    reg = readl(device + ISL38XX_INT_IDENT_REG);

    // also check the contents of the Interrupt Enable Register, because this
    // will filter out interrupt sources from other devices on the same irq !
    reg &= readl(device + ISL38XX_INT_EN_REG);
#endif

    if (reg &= ISL38XX_INT_SOURCES, reg != 0)
    {
#ifdef CONFIG_ARCH_ISL3893
        // write the acknowledge register on the host
        uPCI->ARMIntAckReg = reg;
#else
        // reset the request bits in the Identification register
        writel(reg, device + ISL38XX_INT_ACK_REG);
#endif

#if VERBOSE > SHOW_ERROR_MESSAGES 
        DEBUG(SHOW_FUNCTION_CALLS,
	        "IRQ: Identification register value 0x%x \n", reg );
#endif

        // check for each bit in the register separately
        if (reg & ISL38XX_INT_IDENT_UPDATE)
        {
#if VERBOSE > SHOW_ERROR_MESSAGES 
            // Queue has been updated
            DEBUG(SHOW_TRACING, "IRQ: Update flag \n");

            DEBUG(SHOW_QUEUE_INDEXES,
                "CB drv Qs: [%i][%i][%i][%i][%i][%i]\n",
                le32_to_cpu(private_config->control_block->driver_curr_frag[0]),
                le32_to_cpu(private_config->control_block->driver_curr_frag[1]),
                le32_to_cpu(private_config->control_block->driver_curr_frag[2]),
                le32_to_cpu(private_config->control_block->driver_curr_frag[3]),
                le32_to_cpu(private_config->control_block->driver_curr_frag[4]),
                le32_to_cpu(private_config->control_block->driver_curr_frag[5])
                );

            DEBUG(SHOW_QUEUE_INDEXES,
                "CB dev Qs: [%i][%i][%i][%i][%i][%i]\n",
                le32_to_cpu(private_config->control_block->device_curr_frag[0]),
                le32_to_cpu(private_config->control_block->device_curr_frag[1]),
                le32_to_cpu(private_config->control_block->device_curr_frag[2]),
                le32_to_cpu(private_config->control_block->device_curr_frag[3]),
                le32_to_cpu(private_config->control_block->device_curr_frag[4]),
                le32_to_cpu(private_config->control_block->device_curr_frag[5])
                );
#endif

            // device is in active state, update the powerstate flag
            private_config->powerstate = ISL38XX_PSM_ACTIVE_STATE;

            // check all three queues in priority order
            // call the PIMFOR receive function until the queue is empty
            while (isl38xx_in_queue(private_config->control_block,
                ISL38XX_CB_RX_MGMTQ) != 0)
            {
#if VERBOSE > SHOW_ERROR_MESSAGES 
                DEBUG(SHOW_TRACING, "Received frame in Management Queue\n");
#endif
                islpci_mgt_receive(private_config);

                // call the pimfor transmit function for processing the next
                // management frame in the shadow queue
                islpci_mgt_transmit( private_config );
            }

            while (isl38xx_in_queue(private_config->control_block,
                ISL38XX_CB_RX_DATA_LQ) != 0)
            {
#if VERBOSE > SHOW_ERROR_MESSAGES 
                DEBUG(SHOW_TRACING, "Received frame in Data Low Queue \n");
#endif
                islpci_eth_receive(private_config);
            }

		// check whether the data transmit queues were full
            if (private_config->data_low_tx_full)
            {
                // check whether the transmit is not full anymore
                if( ISL38XX_CB_TX_QSIZE - isl38xx_in_queue( private_config->control_block, 
		            ISL38XX_CB_TX_DATA_LQ ) >= ISL38XX_MIN_QTHRESHOLD )
                {
                    // nope, the driver is ready for more network frames
                    netif_wake_queue(private_config->my_module);

                    // reset the full flag
                    private_config->data_low_tx_full = 0;
                }
            }
        }

        if (reg & ISL38XX_INT_IDENT_INIT)
        {
            // Device has been initialized
#if VERBOSE > SHOW_ERROR_MESSAGES 
            DEBUG(SHOW_TRACING, "IRQ: Init flag, device initialized \n");
#endif

            // perform card initlialization by sending objects
            islpci_mgt_initialize( private_config );
        }

        if (reg & ISL38XX_INT_IDENT_SLEEP)
        {
            // Device intends to move to powersave state
#if VERBOSE > SHOW_ERROR_MESSAGES 
            DEBUG(SHOW_ANYTHING, "IRQ: Sleep flag \n");
#endif
            isl38xx_handle_sleep_request( private_config->control_block,
                &private_config->powerstate,
                private_config->remapped_device_base );
        }

        if (reg & ISL38XX_INT_IDENT_WAKEUP)
        {
            // Device has been woken up to active state
#if VERBOSE > SHOW_ERROR_MESSAGES 
            DEBUG(SHOW_ANYTHING, "IRQ: Wakeup flag \n");
#endif
        
            isl38xx_handle_wakeup( private_config->control_block,
                &private_config->powerstate,
                private_config->remapped_device_base );
        }
    }

    // unlock the interrupt handler
    spin_unlock( &private_config->slock );

    return;
}


/******************************************************************************
    Network Interface Control & Statistical functions
******************************************************************************/

int islpci_open(struct net_device *nwdev)
{
    islpci_private *private_config = nwdev->priv;
#ifdef CONFIG_ARCH_ISL3893
    char cardmode[4] = { 0, 0, 0, 0 };
#endif
#ifdef WDS_LINKS
  struct wds_priv *wdsp = private_config->wdsp;
#endif

#if VERBOSE > SHOW_ERROR_MESSAGES 
    DEBUG(SHOW_FUNCTION_CALLS, "islpci_open (%p)\n", nwdev );
#endif

    // lock the driver code
    //driver_lock( &private_config->slock, &flags );
    
#ifdef CONFIG_ARCH_ISL3893
    /* check whether the firmware has been uploaded in the device */
    if( !firmware_uploaded )
    {
        DEBUG(SHOW_ERROR_MESSAGES, "islpci_open failed: Firmware not uploaded\n" );
        return -EIO;    
    }

    // set the mode again for starting the device
    cardmode[0] = (char) init_mode;
    islpci_mgt_queue(private_config, PIMFOR_OP_SET,
        OID_INL_MODE, 0, &cardmode[0], 4, 0);
#endif

    MOD_INC_USE_COUNT;

#ifdef WDS_LINKS
    open_wds_links(wdsp);
#endif
    
    netif_start_queue(nwdev);
//      netif_mark_up( nwdev );

    // unlock the driver code
    //driver_unlock( &private_config->slock, &flags );

    return 0;
}

int islpci_close(struct net_device *nwdev)
{
//    islpci_private *private_config = nwdev->priv;
//	unsigned long flags;
        
#ifdef WDS_LINKS
  islpci_private *private_config = nwdev->priv;
  struct wds_priv *wdsp = private_config->wdsp;
#endif

#if VERBOSE > SHOW_ERROR_MESSAGES 
    DEBUG(SHOW_FUNCTION_CALLS, "islpci_close \n");
#endif

    // lock the driver code
    //driver_lock( &private_config->slock, &flags );

#ifdef WDS_LINKS
    close_wds_links(wdsp);
#endif
    netif_stop_queue(nwdev);
//      netif_mark_down( nwdev );

    MOD_DEC_USE_COUNT;

    // unlock the driver code
    //driver_unlock( &private_config->slock, &flags );

    return 0;
}

int islpci_reset(islpci_private * private_config, char *firmware )
{
    isl38xx_control_block *control_block = private_config->control_block;
    queue_entry *entry;
//	unsigned long flags;

#if VERBOSE > SHOW_ERROR_MESSAGES 
    DEBUG(SHOW_FUNCTION_CALLS, "isl38xx_reset \n");
#endif

    // lock the driver code
    //driver_lock( &private_config->slock, &flags );

    // stop the transmission of network frames to the driver
    netif_stop_queue(private_config->my_module);

    // disable all device interrupts
    writel(0, private_config->remapped_device_base + ISL38XX_INT_EN_REG);

    // flush all management queues
    while( isl38xx_in_queue(control_block, ISL38XX_CB_TX_MGMTQ) != 0 )
    {
        islpci_get_queue(private_config->remapped_device_base,
                &private_config->mgmt_tx_shadowq, &entry);
        islpci_put_queue(private_config->remapped_device_base,
                &private_config->mgmt_tx_freeq, entry);

        // decrement the real management index
        private_config->index_mgmt_tx--;
    }

//    while( isl38xx_in_queue(control_block, ISL38XX_CB_RX_MGMTQ) != 0 )
//    {
//        islpci_get_queue(private_config->remapped_device_base,
//                &private_config->mgmt_rx_shadowq, &entry);
//        islpci_put_queue(private_config->remapped_device_base,
//                &private_config->mgmt_rx_freeq, entry);

        // decrement the real management index
//        private_config->index_mgmt_rx--;
//    }

#if VERBOSE > SHOW_ERROR_MESSAGES 
    DEBUG(SHOW_TRACING, "Firmware file: %s \n", firmware );
#endif
	
    if (isl38xx_upload_firmware( firmware, private_config->remapped_device_base,
        private_config->device_host_address ) == -1)
    {
        // error uploading the firmware
        DEBUG(SHOW_ERROR_MESSAGES, "ERROR: could not upload the firmware \n" );

        // unlock the driver code
        //driver_unlock( &private_config->slock, &flags );
        return -1;
    }
    
    // unlock the driver code
    //driver_unlock( &private_config->slock, &flags );

    return 0;
}

void islpci_set_multicast_list(struct net_device *dev)
{
#if VERBOSE > SHOW_ERROR_MESSAGES 
    DEBUG(SHOW_FUNCTION_CALLS, "islpci_set_multicast_list \n");
#endif
}

struct net_device_stats *islpci_statistics(struct net_device *nwdev)
{
    islpci_private *private_config = nwdev->priv;
    char firmware[64];

    DEBUG(SHOW_FUNCTION_CALLS, "islpci_statistics \n");

#ifdef CONFIG_ARCH_ISL3893
    
    if ( ! firmware_uploaded ) {

        if( private_config->pci_dev_id == PCIDEVICE_ISL3890 )
            strcpy( firmware, ISL3890_IMAGE_FILE );
        else
            strcpy( firmware, ISL3877_IMAGE_FILE );

#if VERBOSE > SHOW_ERROR_MESSAGES 
    DEBUG(SHOW_TRACING, "Upload firmware: %s, %p, %p \n", 
          firmware, private_config->remapped_device_base, private_config->device_host_address );
#endif

        if (isl38xx_upload_firmware( firmware, private_config->remapped_device_base,
            private_config->device_host_address ) == -1)
        {
            DEBUG(SHOW_ERROR_MESSAGES, "ERROR: %s could not upload the firmware \n", DRIVER_NAME );
        }
        else 
        {       
            /* set the firmware uploaded flag for skipping the firmware upload
               the next time this function is called and for activating the
               device on the open call */
            firmware_uploaded = 1;
        }
    }
#endif

    return &private_config->statistics;
}


/******************************************************************************
    Network device configuration functions
******************************************************************************/
int islpci_alloc_memory( islpci_private *private_config, int mode )
{
    long offset, counter, pages=0;
    long allocated, assigned=0;
    void *queue_base_addr;
    dma_addr_t queue_bus_addr;
    queue_entry *pointerq;

    // handle the consistent DMA allocation depending on the memory allocation
    // definitions made in isl_38xx.h.

#if VERBOSE > SHOW_ERROR_MESSAGES 
    DEBUG(SHOW_FUNCTION_CALLS, "islpci_alloc_memory \n");
    
    // First step is to determine the required memory areas for the driver
    // Area 1: Control Block for the device interface
    // Area 2: Power Save Mode Buffer for temporary frame storage. Be aware that
    //         the number of supported stations in the AP determines the minimal
    //         size of the buffer !
    // Area 3: Driver Queuing System resources
    DEBUG(SHOW_TRACING, "Area 1: Control Block fixed size:  %u bytes\n", CONTROL_BLOCK_SIZE );
    DEBUG(SHOW_TRACING, "Area 2: PSM Buffer total size:     %u bytes\n", PSM_BUFFER_SIZE );
    DEBUG(SHOW_TRACING, "Area 3: Q Management block size:   %u bytes\n", MGMT_QBLOCK );
#endif

    // handle the allocation mode
    if( mode == ALLOC_MODE_PAGED )
    {
        // check whether the selected allocation mode fits the memory requirements
        if( (PSM_BUFFER_SIZE + CONTROL_BLOCK_SIZE) > ALLOC_PAGE1_SIZE )
        {
            // required buffer is too large, change the memory requirement settings
            DEBUG(SHOW_ERROR_MESSAGES, "Error: PSM Buffer & CB exceed page limit\n" );
            return -1;
        }

        // set the allocated parameter for first page
        allocated = ALLOC_PAGE1_SIZE;

#if VERBOSE > SHOW_ERROR_MESSAGES 
        DEBUG(SHOW_TRACING, "Consistent DMA Paged Allocation Mode\n" );
#endif
    }
    else
    {
        // set the allocated parameter for single alloc
        allocated = HOST_MEM_BLOCK;

#if VERBOSE > SHOW_ERROR_MESSAGES 
        DEBUG(SHOW_TRACING, "Consistent DMA Single Allocation Mode\n" );
#endif
    }

    // perform the first allocation for either first page or single alloc
    if( private_config->driver_mem_address = pci_alloc_consistent( 
        private_config->pci_device, allocated, 
        &private_config->device_host_address ), 
        private_config->driver_mem_address == NULL)
    {
        // error allocating the block of PCI memory
        DEBUG( SHOW_ERROR_MESSAGES,
            "ERROR: Could not allocate DMA accessable memory \n" );
        return -1;
    }
    
#if VERBOSE > SHOW_ERROR_MESSAGES 
    DEBUG(SHOW_TRACING, "Device memory block at 0x%x \n", 
        private_config->device_host_address );
    DEBUG(SHOW_TRACING, "Driver memory block at 0x%p \n", 
        private_config->driver_mem_address );
    DEBUG(SHOW_TRACING, "Memory size alloc 0x%lx bytes\n", allocated );
#endif
        
    // assign the Control Block to the first address of the allocated area
    private_config->control_block = 
        (isl38xx_control_block *) private_config->driver_mem_address;
    assigned += CONTROL_BLOCK_SIZE;
            
    // set the Power Save Buffer pointer directly behind the CB
    private_config->device_psm_buffer = 
        private_config->device_host_address + CONTROL_BLOCK_SIZE;
    assigned += PSM_BUFFER_SIZE;

#if VERBOSE > SHOW_ERROR_MESSAGES 
    DEBUG(SHOW_TRACING, "Control Block block at 0x%p sized %u bytes\n", 
        private_config->control_block, CONTROL_BLOCK_SIZE );
    DEBUG(SHOW_TRACING, "PSM Buffer at 0x%p sized %u bytes\n", 
        (void *) private_config->device_psm_buffer, PSM_BUFFER_SIZE );
#endif

    // set the working pointers in page 1 when there is enough room
    if( ((mode==ALLOC_MODE_PAGED) && (allocated-assigned>MGMT_FRAME_SIZE)) ||
        (mode==ALLOC_MODE_SINGLE) )
    {
        // set the working pointers in the current page 1
        queue_base_addr = private_config->driver_mem_address;
        queue_base_addr += (CONTROL_BLOCK_SIZE + PSM_BUFFER_SIZE);
        queue_bus_addr = private_config->device_host_address;
        queue_bus_addr += (CONTROL_BLOCK_SIZE + PSM_BUFFER_SIZE);

#if VERBOSE > SHOW_ERROR_MESSAGES 
        DEBUG(SHOW_TRACING, "Queue base addr at 0x%p with %u bytes\n", 
            (void *) queue_base_addr, allocated-assigned );
#endif
    }
    else
    {
        // first step will be to allocate a new page in the for-next
        queue_base_addr = 0;
        queue_bus_addr = 0;

#if VERBOSE > SHOW_ERROR_MESSAGES 
        DEBUG(SHOW_TRACING, "Queue base addr starts in new page\n" );
#endif
    }        

    // determine the offset for each queue entry configuration
    // allign on 16 byte boundary
    offset = sizeof(queue_entry) / 16;
    offset *= 16;
    offset += ((sizeof(queue_entry) % 16) != 0 ? 16 : 0);

    // assign memory for the management frames 
    for (counter = 0; counter < MGMT_TX_FRAME_COUNT; counter++)
    {
        // check whether the frame can be stored in the current page
        if( allocated - assigned < MGMT_FRAME_SIZE )
        {
            // increment and check the page counter 
            if( pages++, pages == ALLOC_MAX_PAGES )
            {
                // too many pages required, increment the page count
                DEBUG( SHOW_ERROR_MESSAGES,
                    "ERROR: Too many pages required, increment page count or size\n" );
                return -1;
            }

            // allocate a new area to store the new frame
            if( queue_base_addr = pci_alloc_consistent( private_config->pci_device,
                ALLOC_PAGEX_SIZE, &queue_bus_addr ), queue_base_addr == NULL)
            {
                // error allocating the block of PCI memory
                DEBUG( SHOW_ERROR_MESSAGES,
                    "ERROR: Could not allocate DMA accessable memory \n" );
                return -1;
            }
            
            // store the new base address in the private structure for freeing
            private_config->q_base_addr[pages-1] = queue_base_addr;            
            private_config->q_bus_addr[pages-1] = queue_bus_addr;            

#if VERBOSE > SHOW_ERROR_MESSAGES 
            DEBUG( SHOW_TRACING, "New page %lu at %p with size %u bytes\n", 
                pages, queue_base_addr, ALLOC_PAGEX_SIZE );
#endif
            
            // update the working registers
            allocated = ALLOC_PAGEX_SIZE;
            assigned = 0;
        }

        // configure the queue entry at the beginning of the memory block
        pointerq = (queue_entry *) queue_base_addr;
        pointerq->host_address = (u32) queue_base_addr + offset;
        pointerq->dev_address = (u32) queue_bus_addr + offset;
        pointerq->size = MGMT_FRAME_SIZE - offset;
        pointerq->fragments = 1;

        // add the configured entry to the management tx root queue
        islpci_put_queue(private_config->remapped_device_base, 
            &private_config->mgmt_tx_freeq, pointerq);

        // update the working pointers and registers
        queue_base_addr += MGMT_FRAME_SIZE;
        queue_bus_addr += MGMT_FRAME_SIZE;
        assigned += MGMT_FRAME_SIZE;
    }

    for (counter = 0; counter < MGMT_RX_FRAME_COUNT; counter++)
    {
        // check whether the frame can be stored in the current page
        if( allocated - assigned < MGMT_FRAME_SIZE )
        {
            // increment and check the page counter 
            if( pages++, pages == ALLOC_MAX_PAGES )
            {
                // too many pages required, increment the page count
                DEBUG( SHOW_ERROR_MESSAGES,
                    "ERROR: Too many pages required, increment page count or size\n" );
                return -1;
            }

            // allocate a new area to store the new frame
            if( queue_base_addr = pci_alloc_consistent( private_config->pci_device,
                ALLOC_PAGEX_SIZE, &queue_bus_addr ), queue_base_addr == NULL)
            {
                // error allocating the block of PCI memory
                DEBUG( SHOW_ERROR_MESSAGES,
                    "ERROR: Could not allocate DMA accessable memory \n" );
                return -1;
            }
            
            // store the new base address in the private structure for freeing
            private_config->q_base_addr[pages-1] = queue_base_addr;            
            private_config->q_bus_addr[pages-1] = queue_bus_addr;            

#if VERBOSE > SHOW_ERROR_MESSAGES 
            DEBUG( SHOW_TRACING, "New page %lu at %p with size %u bytes\n", 
                pages, queue_base_addr, ALLOC_PAGEX_SIZE );
#endif
            
            // update the working registers
            allocated = ALLOC_PAGEX_SIZE;
            assigned = 0;
        }

        // configure the queue entry at the beginning of the memory block
        pointerq = (queue_entry *) queue_base_addr;
        pointerq->host_address = (u32) queue_base_addr + offset;
        pointerq->dev_address = (u32) queue_bus_addr + offset;
        pointerq->size = MGMT_FRAME_SIZE - offset;
        pointerq->fragments = 1;

        // add the configured entry to the management rx root queue
        islpci_put_queue(private_config->remapped_device_base, 
            &private_config->mgmt_rx_freeq, pointerq);

        // update the working pointers and registers
        queue_base_addr += MGMT_FRAME_SIZE;
        queue_bus_addr += MGMT_FRAME_SIZE;
        assigned += MGMT_FRAME_SIZE;
    }

#if VERBOSE > SHOW_ERROR_MESSAGES 
    DEBUG(SHOW_TRACING, "Queue end addr at 0x%p\n", (void *) queue_base_addr );
#endif

    return 0;
}

int islpci_free_memory( islpci_private *private_config, int mode )
{
    long counter;
    
#if VERBOSE > SHOW_ERROR_MESSAGES 
    DEBUG(SHOW_FUNCTION_CALLS, "islpci_free_memory \n");
#endif
    // handle the allocation mode
    if( mode == ALLOC_MODE_PAGED )
    {
        // free the separate pages
        pci_free_consistent( private_config->pci_device, ALLOC_PAGE1_SIZE, 
            private_config->driver_mem_address, 
            private_config->device_host_address);

        // check the additional pages to be freed
        for( counter=0; counter < ALLOC_MAX_PAGES; counter++ )
        {
            // free the page is allocated
            if( private_config->q_base_addr[counter] )
            {
                pci_free_consistent( private_config->pci_device, 
                    ALLOC_PAGEX_SIZE, private_config->q_base_addr[counter],
                    private_config->q_bus_addr[counter] );
            }
        }        
    }
    else
    {
        // single allocation mode
        pci_free_consistent( private_config->pci_device, HOST_MEM_BLOCK, 
            private_config->driver_mem_address, 
            private_config->device_host_address);
    }

    return 0;
}

struct net_device * islpci_probe(struct net_device *nwdev, struct pci_dev *pci_device, long ioaddr, int irq)
{
    unsigned char mac_addr[ETH_ALEN];
    long counter;
    queue_entry *pointerq;
    islpci_private *private_config;
    struct sk_buff *skb;
    
    if (nwdev == NULL)
    {
    	nwdev = init_etherdev(0,0);
    	if (nwdev == NULL)
    	    return NULL;
    }
    
    firmware_uploaded = 0;

    // setup the structure members
    nwdev->base_addr = ioaddr;
    nwdev->irq = irq;

    // initialize the function pointers
    nwdev->open = &islpci_open;
    nwdev->stop = &islpci_close;
    nwdev->get_stats = &islpci_statistics;
#ifdef WIRELESS_IOCTLS
    nwdev->do_ioctl = &isl_ioctl;
#endif

    nwdev->hard_start_xmit = &islpci_eth_transmit;

//    nwdev->set_multicast_list = &set_rx_mode;

#ifdef HAVE_TX_TIMEOUT
    nwdev->watchdog_timeo = ISLPCI_TX_TIMEOUT;
    nwdev->tx_timeout = &islpci_eth_tx_timeout;
#endif

    // FIXme
    mac_addr[0] = 0x01;
    mac_addr[1] = 0x01;
    mac_addr[2] = 0x01;
    mac_addr[3] = 0x01;
    mac_addr[4] = 0x01;
    mac_addr[5] = 0x01;
    mac_addr[6] = 0x01;
    mac_addr[7] = 0x01;
    memcpy(nwdev->dev_addr, mac_addr, ETH_ALEN);
    ether_setup(nwdev);
    
    // allocate a private device structure to the network device 
#ifdef CONFIG_ARCH_ISL3893
    if ( ( private_config = kmalloc(sizeof(islpci_private), GFP_KERNEL ) ) == NULL )
#else
    if ( ( private_config = kmalloc(sizeof(islpci_private), GFP_KERNEL | GFP_DMA ) ) == NULL )
#endif
    {
        // error allocating the DMA accessable memory area
        DEBUG(SHOW_ERROR_MESSAGES, "ERROR: Could not allocate additional "
            "memory \n");
        return NULL;
    }

    memset(private_config, 0, sizeof(islpci_private));
    nwdev->priv = private_config;
    private_config->next_module = root_islpci_device;
    root_islpci_device = nwdev;
    private_config->my_module = nwdev;
    private_config->pci_device = pci_device;

    // remap the PCI device base address to accessable
    if (private_config->remapped_device_base = ioremap(nwdev->base_addr, 
        ISL38XX_PCI_MEM_SIZE), private_config->remapped_device_base == NULL)
    {
        // error in remapping the PCI device memory address range
        DEBUG(SHOW_ERROR_MESSAGES, "ERROR: PCI memory remapping failed \n");
        return NULL;
    }
    
#ifdef CONFIG_ARCH_ISL3893
    // make sure the device is not running (anymore)
    writel(ISL38XX_DEV_INT_ABORT, private_config->remapped_device_base 
        + ISL38XX_DEV_INT_REG);
#endif

    // save the start and end address of the PCI memory area
    nwdev->mem_start = (unsigned long) private_config->remapped_device_base;
    nwdev->mem_end = nwdev->mem_start + ISL38XX_PCI_MEM_SIZE;

#if VERBOSE > SHOW_ERROR_MESSAGES 
    DEBUG(SHOW_TRACING, "PCI Memory remapped to 0x%p \n", 
        private_config->remapped_device_base );
#endif

    // initialize all the queues in the private configuration structure
    islpci_init_queue(&private_config->mgmt_tx_freeq);
    islpci_init_queue(&private_config->mgmt_rx_freeq);
    islpci_init_queue(&private_config->mgmt_tx_shadowq);
    islpci_init_queue(&private_config->mgmt_rx_shadowq);
    islpci_init_queue(&private_config->ioctl_rx_queue);
    islpci_init_queue(&private_config->trap_rx_queue);
    islpci_init_queue(&private_config->pimfor_rx_queue);
    
#ifdef WDS_LINKS
    private_config->wdsp = kmalloc(sizeof(struct wds_priv), GFP_KERNEL );
    memset(private_config->wdsp, 0, sizeof(struct wds_priv));
#endif    
    
    // allocate the consistent DMA memory depending on the mode    
    if( islpci_alloc_memory( private_config, ALLOC_MEMORY_MODE ))
        return NULL;
 
    // clear the indexes in the frame pointer
    for (counter = 0; counter < ISL38XX_CB_QCOUNT; counter++)
    {
        private_config->control_block->driver_curr_frag[counter] = cpu_to_le32(0);
        private_config->control_block->device_curr_frag[counter] = cpu_to_le32(0);
    }

    // Joho, ready setting up the queueing stuff, now fill the shadow rx queues
    // of both the management and data queues
    for (counter = 0; counter < ISL38XX_CB_MGMT_QSIZE; counter++)
    {
        // get an entry from the freeq and put it in the shadow queue
        if (islpci_get_queue(private_config->remapped_device_base, 
            &private_config->mgmt_rx_freeq, &pointerq))
        {
            // Oops, no more entries in the freeq ?
            DEBUG(SHOW_ERROR_MESSAGES, "Error: No more entries in freeq\n");
            return NULL;
        }
        else
        {
            // use the entry for the device interface management rx queue
            // these should always be inline !
            private_config->control_block->rx_data_mgmt[counter].address =
                cpu_to_le32(pointerq->dev_address);

            // put the entry got from the freeq in the shadow queue
            islpci_put_queue(private_config->remapped_device_base, 
                &private_config->mgmt_rx_shadowq, pointerq);
        }
    }

    for (counter = 0; counter < ISL38XX_CB_RX_QSIZE; counter++)
    {
        // allocate an sk_buff for received data frames storage
		// each frame on receive size consists of 1 fragment
	    // include any required allignment operations
        if (skb = dev_alloc_skb(MAX_FRAGMENT_SIZE+2), skb == NULL)
        {
            // error allocating an sk_buff structure elements
            DEBUG(SHOW_ERROR_MESSAGES, "Error allocating skb \n");
            return NULL;
        }

		// add the new allocated sk_buff to the buffer array
		private_config->data_low_rx[counter] = skb;

		// map the allocated skb data area to pci
        private_config->pci_map_rx_address[counter] = pci_map_single(
        	private_config->pci_device, (void *) skb->data, MAX_FRAGMENT_SIZE+2,
            PCI_DMA_FROMDEVICE );
        if( private_config->pci_map_rx_address[counter] == (dma_addr_t) NULL )
        {
            // error mapping the buffer to device accessable memory address
            DEBUG(SHOW_ERROR_MESSAGES, "Error mapping DMA address\n");
            return NULL;
        }

		// update the fragment address values in the contro block rx queue
        private_config->control_block->rx_data_low[counter].address = cpu_to_le32( (u32) 
		 	private_config->pci_map_rx_address[counter] );
    }

    // since the receive queues are filled with empty fragments, now we can
    // set the corresponding indexes in the Control Block
    private_config->control_block->driver_curr_frag[ISL38XX_CB_RX_DATA_LQ] =
        cpu_to_le32(ISL38XX_CB_RX_QSIZE);
    private_config->control_block->driver_curr_frag[ISL38XX_CB_RX_MGMTQ] =
        cpu_to_le32(ISL38XX_CB_MGMT_QSIZE);

    // reset the real index registers and full flags
    private_config->index_mgmt_rx = 0;
	private_config->index_mgmt_tx = 0;
    private_config->free_data_rx = 0;
    private_config->free_data_tx = 0;
    private_config->data_low_tx_full = 0;

    // reset the queue read locks, process wait counter
    private_config->ioctl_queue_lock = 0;
    private_config->processes_in_wait = 0;

    // set the power save state
    private_config->powerstate = ISL38XX_PSM_ACTIVE_STATE;
    private_config->resetdevice = 0;

    return nwdev;
}


