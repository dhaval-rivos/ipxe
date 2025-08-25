/*
 * Copyright (C) 2012 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * (EEPROM code originally implemented for rtl8139.c)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * You can also choose to distribute this program under the terms of
 * the Unmodified Binary Distribution Licence (as given in the file
 * COPYING.UBDL), provided that you have satisfied its requirements.
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <byteswap.h>
#include <ipxe/netdevice.h>
#include <ipxe/ethernet.h>
#include <ipxe/if_ether.h>
#include <ipxe/iobuf.h>
#include <ipxe/malloc.h>
#include <ipxe/pci.h>
#include <ipxe/dma.h>
#include <ipxe/nvs.h>
#include <ipxe/threewire.h>
#include <ipxe/bitbash.h>
#include <ipxe/mii.h>
#include "realtek.h"

/** @file
 *
 * Realtek 10/100/1000 network card driver
 *
 * Based on the following datasheets:
 *
 *    http://www.datasheetarchive.com/dl/Datasheets-8/DSA-153536.pdf
 *    http://www.datasheetarchive.com/indexdl/Datasheet-028/DSA00494723.pdf
 */

/******************************************************************************
 *
 * Debugging
 *
 ******************************************************************************
 */

/**
 * Dump all registers (for debugging)
 *
 * @v rtl		Realtek device
 */
static __attribute__ (( unused )) void realtek_dump ( struct realtek_nic *rtl ){
	uint8_t regs[256];
	unsigned int i;

	/* Do nothing unless debug output is enabled */
	if ( ! DBG_LOG )
		return;

	/* Dump registers (via byte accesses; may not work for all registers) */
	for ( i = 0 ; i < sizeof ( regs ) ; i++ )
		regs[i] = readb ( rtl->regs + i );
	DBGC ( rtl, "REALTEK %p register dump:\n", rtl );
	DBGC_HDA ( rtl, 0, regs, sizeof ( regs ) );
}

/******************************************************************************
 *
 * EEPROM interface
 *
 ******************************************************************************
 */

/** Pin mapping for SPI bit-bashing interface */
static const uint8_t realtek_eeprom_bits[] = {
	[SPI_BIT_SCLK]	= RTL_9346CR_EESK,
	[SPI_BIT_MOSI]	= RTL_9346CR_EEDI,
	[SPI_BIT_MISO]	= RTL_9346CR_EEDO,
	[SPI_BIT_SS(0)]	= RTL_9346CR_EECS,
};

/**
 * Open bit-bashing interface
 *
 * @v basher		Bit-bashing interface
 */
static void realtek_spi_open_bit ( struct bit_basher *basher ) {
	struct realtek_nic *rtl = container_of ( basher, struct realtek_nic,
						 spibit.basher );

	/* Enable EEPROM access */
	writeb ( RTL_9346CR_EEM_EEPROM, rtl->regs + RTL_9346CR );
	readb ( rtl->regs + RTL_9346CR ); /* Ensure write reaches chip */
}

/**
 * Close bit-bashing interface
 *
 * @v basher		Bit-bashing interface
 */
static void realtek_spi_close_bit ( struct bit_basher *basher ) {
	struct realtek_nic *rtl = container_of ( basher, struct realtek_nic,
						 spibit.basher );

	/* Disable EEPROM access */
	writeb ( RTL_9346CR_EEM_NORMAL, rtl->regs + RTL_9346CR );
	readb ( rtl->regs + RTL_9346CR ); /* Ensure write reaches chip */
}

/**
 * Read input bit
 *
 * @v basher		Bit-bashing interface
 * @v bit_id		Bit number
 * @ret zero		Input is a logic 0
 * @ret non-zero	Input is a logic 1
 */
static int realtek_spi_read_bit ( struct bit_basher *basher,
				  unsigned int bit_id ) {
	struct realtek_nic *rtl = container_of ( basher, struct realtek_nic,
						 spibit.basher );
	uint8_t mask = realtek_eeprom_bits[bit_id];
	uint8_t reg;

	DBG_DISABLE ( DBGLVL_IO );
	reg = readb ( rtl->regs + RTL_9346CR );
	DBG_ENABLE ( DBGLVL_IO );
	return ( reg & mask );
}

/**
 * Set/clear output bit
 *
 * @v basher		Bit-bashing interface
 * @v bit_id		Bit number
 * @v data		Value to write
 */
static void realtek_spi_write_bit ( struct bit_basher *basher,
				    unsigned int bit_id, unsigned long data ) {
	struct realtek_nic *rtl = container_of ( basher, struct realtek_nic,
						 spibit.basher );
	uint8_t mask = realtek_eeprom_bits[bit_id];
	uint8_t reg;

	DBG_DISABLE ( DBGLVL_IO );
	reg = readb ( rtl->regs + RTL_9346CR );
	reg &= ~mask;
	reg |= ( data & mask );
	writeb ( reg, rtl->regs + RTL_9346CR );
	readb ( rtl->regs + RTL_9346CR ); /* Ensure write reaches chip */
	DBG_ENABLE ( DBGLVL_IO );
}

/** SPI bit-bashing interface */
static struct bit_basher_operations realtek_basher_ops = {
	.open = realtek_spi_open_bit,
	.close = realtek_spi_close_bit,
	.read = realtek_spi_read_bit,
	.write = realtek_spi_write_bit,
};

/**
 * Initialise EEPROM
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 */
static int __unused realtek_init_eeprom ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	uint16_t id;
	int rc;

	/* Initialise SPI bit-bashing interface */
	rtl->spibit.basher.op = &realtek_basher_ops;
	rtl->spibit.bus.mode = SPI_MODE_THREEWIRE;
	init_spi_bit_basher ( &rtl->spibit );

	/* Detect EEPROM type and initialise three-wire device */
	if ( readl ( rtl->regs + RTL_RCR ) & RTL_RCR_9356SEL ) {
		DBGC ( rtl, "REALTEK %p EEPROM is a 93C56\n", rtl );
		init_at93c56 ( &rtl->eeprom, 16 );
	} else {
		DBGC ( rtl, "REALTEK %p EEPROM is a 93C46\n", rtl );
		init_at93c46 ( &rtl->eeprom, 16 );
	}

	/* Check for EEPROM presence.  Some onboard NICs will have no
	 * EEPROM connected, with the BIOS being responsible for
	 * programming the initial register values.
	 */
	if ( ( rc = nvs_read ( &rtl->eeprom.nvs, RTL_EEPROM_ID,
			       &id, sizeof ( id ) ) ) != 0 ) {
		DBGC ( rtl, "REALTEK %p could not read EEPROM ID: %s\n",
		       rtl, strerror ( rc ) );
		return rc;
	}
	if ( id != cpu_to_le16 ( RTL_EEPROM_ID_MAGIC ) ) {
		DBGC ( rtl, "REALTEK %p EEPROM ID incorrect (%#04x); assuming "
		       "no EEPROM\n", rtl, le16_to_cpu ( id ) );
		return -ENODEV;
	}

	/* Initialise space for non-volatile options, if available
	 *
	 * We use offset 0x40 (i.e. address 0x20), length 0x40.  This
	 * block is marked as VPD in the Realtek datasheets, so we use
	 * it only if we detect that the card is not supporting VPD.
	 */
	if ( readb ( rtl->regs + RTL_CONFIG1 ) & RTL_CONFIG1_VPD ) {
		DBGC ( rtl, "REALTEK %p EEPROM in use for VPD; cannot use "
		       "for options\n", rtl );
	} else {
		nvo_init ( &rtl->nvo, &rtl->eeprom.nvs, RTL_EEPROM_VPD,
			   RTL_EEPROM_VPD_LEN, NULL, &netdev->refcnt );
	}

	return 0;
}

/******************************************************************************
 *
 * MII interface
 *
 ******************************************************************************
 */

/**
 * Read from MII register
 *
 * @v mdio		MII interface
 * @v phy		PHY address
 * @v reg		Register address
 * @ret value		Data read, or negative error
 */
static int realtek_mii_read ( struct mii_interface *mdio,
			      unsigned int phy __unused, unsigned int reg ) {
	struct realtek_nic *rtl =
		container_of ( mdio, struct realtek_nic, mdio );
	unsigned int i;
	uint32_t value;

	/* Fail if PHYAR register is not present */
	if ( ! rtl->have_phy_regs )
		return -ENOTSUP;

	/* Initiate read */
	writel ( RTL_PHYAR_VALUE ( 0, reg, 0 ), rtl->regs + RTL_PHYAR );

	/* Wait for read to complete */
	for ( i = 0 ; i < RTL_MII_MAX_WAIT_US ; i++ ) {

		/* If read is not complete, delay 1us and retry */
		value = readl ( rtl->regs + RTL_PHYAR );
		if ( ! ( value & RTL_PHYAR_FLAG ) ) {
			udelay ( 1 );
			continue;
		}

		/* Return register value */
		return ( RTL_PHYAR_DATA ( value ) );
	}

	DBGC ( rtl, "REALTEK %p timed out waiting for MII read\n", rtl );
	return -ETIMEDOUT;
}

/**
 * Write to MII register
 *
 * @v mdio		MII interface
 * @v phy		PHY address
 * @v reg		Register address
 * @v data		Data to write
 * @ret rc		Return status code
 */
static int realtek_mii_write ( struct mii_interface *mdio,
			       unsigned int phy __unused, unsigned int reg,
			       unsigned int data ) {
	struct realtek_nic *rtl =
		container_of ( mdio, struct realtek_nic, mdio );
	unsigned int i;

	/* Fail if PHYAR register is not present */
	if ( ! rtl->have_phy_regs )
		return -ENOTSUP;

	/* Initiate write */
	writel ( RTL_PHYAR_VALUE ( RTL_PHYAR_FLAG, reg, data ),
		 rtl->regs + RTL_PHYAR );

	/* Wait for write to complete */
	for ( i = 0 ; i < RTL_MII_MAX_WAIT_US ; i++ ) {

		/* If write is not complete, delay 1us and retry */
		if ( readl ( rtl->regs + RTL_PHYAR ) & RTL_PHYAR_FLAG ) {
			udelay ( 1 );
			continue;
		}

		return 0;
	}

	DBGC ( rtl, "REALTEK %p timed out waiting for MII write\n", rtl );
	return -ETIMEDOUT;
}

/** Realtek MII operations */
static struct mii_operations realtek_mii_operations = {
	.read = realtek_mii_read,
	.write = realtek_mii_write,
};

/* ----- 8125-only tiny helpers (no generic handlers) ----- */
static inline void realtek_enable_cplus(struct realtek_nic *rtl) {
    uint16_t cpcr = readw(rtl->regs + RTL_CPCR);
    cpcr |= (RTL_CPCR_MULRW | RTL_CPCR_CPRX | RTL_CPCR_CPTX);
    cpcr &= ~RTL_CPCR_VLAN;
    writew(cpcr, rtl->regs + RTL_CPCR);
}

static int realtek_phy_up(struct realtek_nic *rtl) {
    if (!rtl->have_phy_regs)
        return 0;
    /* Advertise 1G (best-effort; ignore errors on non-1G PHYs) */
    int v = mii_read(&rtl->mii, MII_CTRL1000);
    if (v >= 0)
        mii_write(&rtl->mii, MII_CTRL1000,
                  v | ADVERTISE_1000FULL | ADVERTISE_1000HALF);
    mii_reset(&rtl->mii);
    mii_restart(&rtl->mii);
    return 0;
}

/* -------------------------------------------------------- */

/**
 * Check if MAC address is all zeros (invalid)
 *
 * @v mac_addr	MAC address to check
 * @ret is_zero	True if MAC is all zeros, false otherwise
 */
static int realtek_is_mac_zero(const uint8_t *mac_addr) {
    int i;
    for (i = 0; i < ETH_ALEN; i++) {
        if (mac_addr[i] != 0)
            return 0;
    }
    return 1;
}

/**
 * Try to read MAC address from hardware registers
 *
 * @v netdev	Network device
 * @ret rc	Return status code (0 if valid MAC found)
 */
static int realtek_read_mac_from_hardware(struct net_device *netdev) {
    struct realtek_nic *rtl = netdev->priv;
    uint8_t temp_mac[ETH_ALEN];
    int i;
    
    /* Read MAC from IDR registers */
    for (i = 0; i < ETH_ALEN; i++) {
        temp_mac[i] = readb(rtl->regs + RTL_IDR0 + i);
    }
    
    DBGC(rtl, "REALTEK %p hardware MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
         rtl, temp_mac[0], temp_mac[1], temp_mac[2],
         temp_mac[3], temp_mac[4], temp_mac[5]);
    
    /* Check if MAC is valid (not all zeros) */
    if (realtek_is_mac_zero(temp_mac)) {
        DBGC(rtl, "REALTEK %p hardware MAC is all zeros - invalid\n", rtl);
        return -ENOENT;
    }
    
    /* Copy valid MAC to netdev */
    memcpy(netdev->hw_addr, temp_mac, ETH_ALEN);
    DBGC(rtl, "REALTEK %p using hardware MAC: %s\n", rtl, eth_ntoa(netdev->hw_addr));
    return 0;
}

/******************************************************************************
 *
 * RTL8125 OCP register access
 *
 * These functions are based on the Linux r8169 driver's r8168_mac_ocp_read()
 * and r8168_mac_ocp_write() functions, which properly handle the ERIAR register
 * bit settings required for reliable OCP access across all RTL8125 revisions.
 *
 ******************************************************************************
 */

/**
 * Check if RTL8125 is ready for OCP access
 *
 * @v rtl		Realtek device
 * @ret ready		1 if ready, 0 if not ready
 */
static int realtek_ocp_ready ( struct realtek_nic *rtl ) {
	uint32_t eriar_value;
	int i;
	
	/* Check if ERIAR register is accessible and not busy */
	for ( i = 0 ; i < 100 ; i++ ) {
		eriar_value = readl ( rtl->regs + RTL_ERIAR );
		if ( ! ( eriar_value & RTL_ERIAR_FLAG ) )
			return 1;
		udelay ( 10 );
	}
	
	DBGC ( rtl, "REALTEK %p OCP not ready (ERIAR busy: 0x%08x)\n", rtl, eriar_value );
	return 0;
}

/**
 * Read RTL8125 MAC OCP register (corrected to match Linux r8169 exactly)
 *
 * @v rtl		Realtek device
 * @v addr		OCP register address
 * @ret value		Register value
 */
static uint16_t realtek_ocp_read ( struct realtek_nic *rtl, uint16_t addr ) {
	unsigned int i;
	uint32_t eriar_value;

	DBGC2 ( rtl, "REALTEK %p OCP read from address 0x%04x\n", rtl, addr );

	/* Check if OCP is ready for access */
	if ( ! realtek_ocp_ready ( rtl ) ) {
		DBGC ( rtl, "REALTEK %p OCP not ready for read at 0x%04x\n", rtl, addr );
		return 0xffff;
	}

	/* Linux r8169 for RTL8125 MAC OCP read:
	 * 1. Write ERIAR with FLAG=0 (bit 31 clear), TYPE=MAC_OCP, address (full 16-bit)
	 * 2. Wait for FLAG to become 1 (bit 31 set) indicating completion
	 * 3. Read result from ERIDR
	 */
	eriar_value = RTL_ERIAR_READ | RTL_ERIAR_MAC_OCP | ( addr & RTL_ERIAR_ADDR_MASK );
	
	DBGC2 ( rtl, "REALTEK %p OCP writing ERIAR=0x%08x for read (addr=0x%04x)\n", rtl, eriar_value, addr );
	writel ( eriar_value, rtl->regs + RTL_ERIAR );

	/* Wait for completion - bit 31 becomes 1 when read operation is complete */
	for ( i = 0 ; i < 2000 ; i++ ) {
		eriar_value = readl ( rtl->regs + RTL_ERIAR );
		if ( eriar_value & RTL_ERIAR_FLAG )  /* Wait for bit 31 to SET for reads */
			break;
		udelay ( 1 );
	}

	if ( i >= 2000 ) {
		DBGC ( rtl, "REALTEK %p OCP read timeout at address 0x%04x (ERIAR=0x%08x)\n", 
		       rtl, addr, eriar_value );
		return 0xffff;
	}

	/* Read result from ERIDR */
	uint16_t result = ( readl ( rtl->regs + RTL_ERIDR ) & 0xffff );
	DBGC2 ( rtl, "REALTEK %p OCP read completed: addr=0x%04x value=0x%04x (took %d iterations)\n", 
	        rtl, addr, result, i );
	return result;
}

/**
 * Write RTL8125 MAC OCP register (based on Linux r8168_mac_ocp_write)
 *
 * @v rtl		Realtek device
 * @v addr		OCP register address
 * @v value		Register value
 */
/**
 * Write RTL8125 MAC OCP register (corrected to match Linux r8169 exactly)
 *
 * @v rtl		Realtek device
 * @v addr		OCP register address
 * @v value		Register value
 */
static void realtek_ocp_write ( struct realtek_nic *rtl, uint16_t addr, uint16_t value ) {
	unsigned int i;
	uint32_t eriar_value;

	DBGC2 ( rtl, "REALTEK %p OCP write to address 0x%04x value 0x%04x\n", rtl, addr, value );

	/* Check if OCP is ready for access */
	if ( ! realtek_ocp_ready ( rtl ) ) {
		DBGC ( rtl, "REALTEK %p OCP not ready for write at 0x%04x\n", rtl, addr );
		return;
	}

	/* Write value to ERIDR first */
	writel ( value, rtl->regs + RTL_ERIDR );
	
	/* Linux r8169 for RTL8125 MAC OCP write:
	 * 1. Write data to ERIDR
	 * 2. Write ERIAR with FLAG=1 (bit 31 set), TYPE=MAC_OCP, address (full 16-bit)  
	 * 3. Wait for FLAG to become 0 (bit 31 clear) indicating completion
	 */
	eriar_value = RTL_ERIAR_WRITE | RTL_ERIAR_MAC_OCP | ( addr & RTL_ERIAR_ADDR_MASK );
	
	DBGC2 ( rtl, "REALTEK %p OCP writing ERIAR=0x%08x for write (addr=0x%04x)\n", rtl, eriar_value, addr );
	writel ( eriar_value, rtl->regs + RTL_ERIAR );

	/* Wait for completion - bit 31 becomes 0 when write operation is complete */
	for ( i = 0 ; i < 2000 ; i++ ) {
		eriar_value = readl ( rtl->regs + RTL_ERIAR );
		if ( ! ( eriar_value & RTL_ERIAR_FLAG ) )  /* Wait for bit 31 to CLEAR for writes */
			break;
		udelay ( 1 );
	}

	if ( i >= 2000 ) {
		DBGC ( rtl, "REALTEK %p OCP write timeout at address 0x%04x (ERIAR=0x%08x)\n", 
		       rtl, addr, eriar_value );
	} else {
		DBGC2 ( rtl, "REALTEK %p OCP write completed: addr=0x%04x value=0x%04x (took %d iterations)\n", 
		        rtl, addr, value, i );
	}
}

/**
 * Disable RTL8125 new TX descriptor format
 *
 * @v rtl		Realtek device
 */
static void realtek_disable_new_desc ( struct realtek_nic *rtl ) {
	uint16_t val;

	DBGC ( rtl, "REALTEK %p disabling RTL8125 new TX descriptor format\n", rtl );
	
	/* First, let's check if OCP access is working at all */
	DBGC ( rtl, "REALTEK %p testing OCP access with a simple read\n", rtl );
	
	/* Read current value of OCP register 0xeb58 */
	val = realtek_ocp_read ( rtl, 0xeb58 );
	DBGC ( rtl, "REALTEK %p OCP 0xeb58 original value: 0x%04x\n", rtl, val );
	
	/* Check if we got a valid response (not 0xffff timeout value) */
	if ( val == 0xffff ) {
		DBGC ( rtl, "REALTEK %p OCP access failed, skipping new descriptor disable\n", rtl );
		DBGC ( rtl, "REALTEK %p this may not be a problem - some RTL8125 variants work without OCP\n", rtl );
		return;
	}
	
	/* Clear bit 0 to disable new descriptor format */
	val &= ~0x0001;
	DBGC ( rtl, "REALTEK %p OCP 0xeb58 modified value: 0x%04x\n", rtl, val );
	
	/* Write back the modified value */
	realtek_ocp_write ( rtl, 0xeb58, val );
	
	/* Verify the write */
	uint16_t verify = realtek_ocp_read ( rtl, 0xeb58 );
	DBGC ( rtl, "REALTEK %p OCP 0xeb58 verification read: 0x%04x\n", rtl, verify );
	
	if ( verify == val ) {
		DBGC ( rtl, "REALTEK %p RTL8125 new descriptor format successfully disabled\n", rtl );
	} else {
		DBGC ( rtl, "REALTEK %p RTL8125 new descriptor format disable verification failed\n", rtl );
		DBGC ( rtl, "REALTEK %p continuing anyway - device may still work\n", rtl );
	}
}

/******************************************************************************
 *
 * Device reset
 *
 ******************************************************************************
 */

/**
 * Reset hardware
 *
 * @v rtl		Realtek device
 * @v pci		PCI device (for device-specific handling)
 * @ret rc		Return status code
 */
static int realtek_reset ( struct realtek_nic *rtl, struct pci_device *pci ) {
	unsigned int i;
	unsigned int max_wait = RTL_RESET_MAX_WAIT_MS;

	DBGC ( rtl, "REALTEK %p starting device reset\n", rtl );

	/* RTL8125 family may need longer reset time */
	if ( pci && RTL_IS_8125_FAMILY ( pci->vendor, pci->device ) ) {
		max_wait = RTL_RESET_MAX_WAIT_MS * 2;
		DBGC ( rtl, "REALTEK %p RTL8125 family using extended reset timeout (%dms)\n", rtl, max_wait );
	}

	/* Issue reset */
	DBGC ( rtl, "REALTEK %p issuing reset command\n", rtl );
	writeb ( RTL_CR_RST, rtl->regs + RTL_CR );

	/* Wait for reset to complete */
	for ( i = 0 ; i < max_wait ; i++ ) {

		/* If reset is not complete, delay 1ms and retry */
		if ( readb ( rtl->regs + RTL_CR ) & RTL_CR_RST ) {
			mdelay ( 1 );
			continue;
		}

		DBGC ( rtl, "REALTEK %p reset completed after %dms\n", rtl, i );

		/* RTL8125 family specific post-reset initialization */
		if ( pci && RTL_IS_8125_FAMILY ( pci->vendor, pci->device ) ) {
			DBGC ( rtl, "REALTEK %p RTL8125 family post-reset settling delay\n", rtl );
			/* Additional settling time for RTL8125 family */
			mdelay ( 10 );
			
			/* Additional delay to ensure device is fully ready */
			DBGC ( rtl, "REALTEK %p RTL8125 family additional stabilization delay\n", rtl );
			mdelay ( 50 );
		}

		DBGC ( rtl, "REALTEK %p reset successful\n", rtl );
		return 0;
	}

	DBGC ( rtl, "REALTEK %p timed out waiting for reset (waited %dms)\n", rtl, max_wait );
	return -ETIMEDOUT;
}

/**
 * Configure PHY for Gigabit operation
 *
 * @v rtl		Realtek device
 * @ret rc		Return status code
 */
static int realtek_phy_speed ( struct realtek_nic *rtl ) {
	int ctrl1000;
	int rc;

	/* Read CTRL1000 register */
	ctrl1000 = mii_read ( &rtl->mii, MII_CTRL1000 );
	if ( ctrl1000 < 0 ) {
		rc = ctrl1000;
		DBGC ( rtl, "REALTEK %p could not read CTRL1000: %s\n",
		       rtl, strerror ( rc ) );
		return rc;
	}

	/* Advertise 1000Mbps speeds */
	ctrl1000 |= ( ADVERTISE_1000FULL | ADVERTISE_1000HALF );
	if ( ( rc = mii_write ( &rtl->mii, MII_CTRL1000, ctrl1000 ) ) != 0 ) {
		DBGC ( rtl, "REALTEK %p could not write CTRL1000: %s\n",
		       rtl, strerror ( rc ) );
		return rc;
	}

	return 0;
}

/**
 * Reset PHY
 *
 * @v rtl		Realtek device
 * @ret rc		Return status code
 */
static int realtek_phy_reset ( struct realtek_nic *rtl ) {
	int rc;

	/* Do nothing if we have no separate PHY register access */
	if ( ! rtl->have_phy_regs )
		return 0;

	/* Perform MII reset */
	if ( ( rc = mii_reset ( &rtl->mii ) ) != 0 ) {
		DBGC ( rtl, "REALTEK %p could not reset MII: %s\n",
		       rtl, strerror ( rc ) );
		return rc;
	}

	/* Some cards (e.g. RTL8169SC) do not advertise Gigabit by
	 * default.  Try to enable advertisement of Gigabit speeds.
	 */
	if ( ( rc = realtek_phy_speed ( rtl ) ) != 0 ) {
		/* Ignore failures, since the register may not be
		 * present on non-Gigabit PHYs (e.g. RTL8101).
		 */
	}

	/* Some cards (e.g. RTL8211B) have a hardware errata that
	 * requires the MII_MMD_DATA register to be cleared before the
	 * link will come up.
	 */
	if ( ( rc = mii_write ( &rtl->mii, MII_MMD_DATA, 0 ) ) != 0 ) {
		/* Ignore failures, since the register may not be
		 * present on all PHYs.
		 */
	}

	/* Restart autonegotiation */
	if ( ( rc = mii_restart ( &rtl->mii ) ) != 0 ) {
		DBGC ( rtl, "REALTEK %p could not restart MII: %s\n",
		       rtl, strerror ( rc ) );
		return rc;
	}

	return 0;
}

/******************************************************************************
 *
 * Link state
 *
 ******************************************************************************
 */

/**
 * Check link state
 *
 * @v netdev		Network device
 */
static void realtek_check_link ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	uint8_t phystatus;
	uint8_t msr;
	int link_up;

	DBGC2 ( rtl, "REALTEK %p checking link state\n", rtl );

	/* Determine link state */
	if ( rtl->have_phy_regs ) {
		mii_dump ( &rtl->mii );
		phystatus = readb ( rtl->regs + RTL_PHYSTATUS );
		link_up = ( phystatus & RTL_PHYSTATUS_LINKSTS );
		DBGC ( rtl, "REALTEK %p PHY status is %02x (%s%s%s%s%s%s, "
		       "Link%s, %sDuplex)\n", rtl, phystatus,
		       ( ( phystatus & RTL_PHYSTATUS_ENTBI ) ? "TBI" : "GMII" ),
		       ( ( phystatus & RTL_PHYSTATUS_TXFLOW ) ?
			 ", TxFlow" : "" ),
		       ( ( phystatus & RTL_PHYSTATUS_RXFLOW ) ?
			 ", RxFlow" : "" ),
		       ( ( phystatus & RTL_PHYSTATUS_1000MF ) ?
			 ", 1000Mbps" : "" ),
		       ( ( phystatus & RTL_PHYSTATUS_100M ) ?
			 ", 100Mbps" : "" ),
		       ( ( phystatus & RTL_PHYSTATUS_10M ) ?
			 ", 10Mbps" : "" ),
		       ( ( phystatus & RTL_PHYSTATUS_LINKSTS ) ?
			 "Up" : "Down" ),
		       ( ( phystatus & RTL_PHYSTATUS_FULLDUP ) ?
			 "Full" : "Half" ) );
	} else {
		msr = readb ( rtl->regs + RTL_MSR );
		link_up = ( ! ( msr & RTL_MSR_LINKB ) );
		DBGC ( rtl, "REALTEK %p media status is %02x (Link%s, "
		       "%dMbps%s%s%s%s%s)\n", rtl, msr,
		       ( ( msr & RTL_MSR_LINKB ) ? "Down" : "Up" ),
		       ( ( msr & RTL_MSR_SPEED_10 ) ? 10 : 100 ),
		       ( ( msr & RTL_MSR_TXFCE ) ? ", TxFlow" : "" ),
		       ( ( msr & RTL_MSR_RXFCE ) ? ", RxFlow" : "" ),
		       ( ( msr & RTL_MSR_AUX_STATUS ) ? ", AuxPwr" : "" ),
		       ( ( msr & RTL_MSR_TXPF ) ? ", TxPause" : "" ),
		       ( ( msr & RTL_MSR_RXPF ) ? ", RxPause" : "" ) );
	}

	/* Report link state */
	if ( link_up ) {
		netdev_link_up ( netdev );
	} else {
		netdev_link_down ( netdev );
	}
}

/******************************************************************************
 *
 * Network device interface
 *
 ******************************************************************************
 */

/**
 * Create receive buffer (legacy mode)
 *
 * @v rtl		Realtek device
 * @ret rc		Return status code
 */
static int realtek_create_buffer ( struct realtek_nic *rtl ) {
	struct realtek_rx_buffer *rxbuf = &rtl->rxbuf;
	size_t len = ( RTL_RXBUF_LEN + RTL_RXBUF_PAD );

	/* Do nothing unless in legacy mode */
	if ( ! rtl->legacy )
		return 0;

	/* Allocate buffer */
	rxbuf->data = dma_alloc ( rtl->dma, &rxbuf->map, len,
				  RTL_RXBUF_ALIGN );
	if ( ! rxbuf->data )
		return -ENOMEM;

	/* Program buffer address */
	writel ( dma ( &rxbuf->map, rxbuf->data ), rtl->regs + RTL_RBSTART );
	DBGC ( rtl, "REALTEK %p receive buffer is at [%08lx,%08lx,%08lx)\n",
	       rtl, virt_to_phys ( rxbuf->data ),
	       ( virt_to_phys ( rxbuf->data ) + RTL_RXBUF_LEN ),
	       ( virt_to_phys ( rxbuf->data ) + len ) );

	return 0;
}

/**
 * Destroy receive buffer (legacy mode)
 *
 * @v rtl		Realtek device
 */
static void realtek_destroy_buffer ( struct realtek_nic *rtl ) {
	struct realtek_rx_buffer *rxbuf = &rtl->rxbuf;
	size_t len = ( RTL_RXBUF_LEN + RTL_RXBUF_PAD );

	/* Do nothing unless in legacy mode */
	if ( ! rtl->legacy )
		return;

	/* Clear buffer address */
	writel ( 0, rtl->regs + RTL_RBSTART );

	/* Free buffer */
	dma_free ( &rxbuf->map, rxbuf->data, len );
	rxbuf->data = NULL;
	rxbuf->offset = 0;
}

/**
 * Create descriptor ring
 *
 * @v rtl		Realtek device
 * @v ring		Descriptor ring
 * @ret rc		Return status code
 */
static int realtek_create_ring ( struct realtek_nic *rtl,
				 struct realtek_ring *ring ) {
	physaddr_t address;

	/* Do nothing in legacy mode */
	if ( rtl->legacy )
		return 0;

	/* Allocate descriptor ring */
	ring->desc = dma_alloc ( rtl->dma, &ring->map, ring->len,
				 RTL_RING_ALIGN );
	if ( ! ring->desc )
		return -ENOMEM;

	/* Initialise descriptor ring */
	memset ( ring->desc, 0, ring->len );

	/* Program ring address */
	address = dma ( &ring->map, ring->desc );
	writel ( ( ( ( uint64_t ) address ) >> 32 ),
		 rtl->regs + ring->reg + 4 );
	writel ( ( address & 0xffffffffUL ), rtl->regs + ring->reg );
	DBGC ( rtl, "REALTEK %p ring %02x is at [%08lx,%08lx)\n",
	       rtl, ring->reg, virt_to_phys ( ring->desc ),
	       ( virt_to_phys ( ring->desc ) + ring->len ) );

	return 0;
}

/**
 * Destroy descriptor ring
 *
 * @v rtl		Realtek device
 * @v ring		Descriptor ring
 */
static void realtek_destroy_ring ( struct realtek_nic *rtl,
				   struct realtek_ring *ring ) {

	/* Reset producer and consumer counters */
	ring->prod = 0;
	ring->cons = 0;

	/* Do nothing more if in legacy mode */
	if ( rtl->legacy )
		return;

	/* Clear ring address */
	writel ( 0, rtl->regs + ring->reg );
	writel ( 0, rtl->regs + ring->reg + 4 );

	/* Free descriptor ring */
	dma_free ( &ring->map, ring->desc, ring->len );
	ring->desc = NULL;
}

/**
 * Refill receive descriptor ring
 *
 * @v rtl		Realtek device
 */
static void realtek_refill_rx ( struct realtek_nic *rtl ) {
	struct realtek_descriptor *rx;
	struct io_buffer *iobuf;
	unsigned int rx_idx;
	int is_last;

	/* Do nothing in legacy mode */
	if ( rtl->legacy )
		return;

	while ( ( rtl->rx.prod - rtl->rx.cons ) < RTL_NUM_RX_DESC ) {

		/* Allocate I/O buffer */
		iobuf = alloc_rx_iob ( RTL_RX_MAX_LEN, rtl->dma );
		if ( ! iobuf ) {
			/* Wait for next refill */
			return;
		}

		/* Get next receive descriptor */
		rx_idx = ( rtl->rx.prod++ % RTL_NUM_RX_DESC );
		is_last = ( rx_idx == ( RTL_NUM_RX_DESC - 1 ) );
		rx = &rtl->rx.desc[rx_idx];

		/* Populate receive descriptor */
		rx->address = cpu_to_le64 ( iob_dma ( iobuf ) );
		rx->length = cpu_to_le16 ( RTL_RX_MAX_LEN );
		wmb();
		rx->flags = ( cpu_to_le16 ( RTL_DESC_OWN ) |
			      ( is_last ? cpu_to_le16 ( RTL_DESC_EOR ) : 0 ) );
		wmb();

		/* Record I/O buffer */
		assert ( rtl->rx_iobuf[rx_idx] == NULL );
		rtl->rx_iobuf[rx_idx] = iobuf;

		DBGC2 ( rtl, "REALTEK %p RX %d is [%lx,%lx)\n",
			rtl, rx_idx, virt_to_phys ( iobuf->data ),
			( virt_to_phys ( iobuf->data ) + RTL_RX_MAX_LEN ) );
	}
}

/**
 * Program MAC address into hardware registers
 *
 * @v netdev		Network device
 */
static void realtek_set_mac_address ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	unsigned int i;
	int success = 0;

	DBGC ( rtl, "REALTEK %p programming MAC address: %s\n", 
	       rtl, eth_ntoa ( netdev->hw_addr ) );

	/* RTL8125 family may need special MAC programming approach */
	if ( rtl->use_8125 ) {
		DBGC ( rtl, "REALTEK %p RTL8125 family detected, trying enhanced MAC programming\n", rtl );
		
		/* Method 1: Try to unlock MAC registers first */
		DBGC ( rtl, "REALTEK %p attempting to unlock MAC registers\n", rtl );
		writeb ( 0xc0, rtl->regs + RTL_9346CR );  /* Unlock registers */
		udelay ( 10 );
		
		/* Try programming MAC address */
		for ( i = 0 ; i < ETH_ALEN ; i++ ) {
			writeb ( netdev->hw_addr[i], rtl->regs + RTL_IDR0 + i );
		}
		
		/* Verify this method worked */
		success = 1;
		for ( i = 0 ; i < ETH_ALEN ; i++ ) {
			uint8_t hw_byte = readb ( rtl->regs + RTL_IDR0 + i );
			if ( hw_byte != netdev->hw_addr[i] ) {
				success = 0;
				break;
			}
		}
		
		if ( success ) {
			DBGC ( rtl, "REALTEK %p RTL8125 MAC programming successful with unlock method\n", rtl );
		} else {
			DBGC ( rtl, "REALTEK %p RTL8125 MAC programming failed with unlock method\n", rtl );
			
			/* Method 2: Try with different unlock sequence */
			DBGC ( rtl, "REALTEK %p trying alternative unlock sequence\n", rtl );
			writeb ( 0x00, rtl->regs + RTL_9346CR );  /* Lock first */
			udelay ( 10 );
			writeb ( 0xc0, rtl->regs + RTL_9346CR );  /* Then unlock */
			udelay ( 100 );  /* Longer delay */
			
			for ( i = 0 ; i < ETH_ALEN ; i++ ) {
				writeb ( netdev->hw_addr[i], rtl->regs + RTL_IDR0 + i );
				udelay ( 1 );  /* Small delay between writes */
			}
			
			/* Verify alternative method */
			success = 1;
			for ( i = 0 ; i < ETH_ALEN ; i++ ) {
				uint8_t hw_byte = readb ( rtl->regs + RTL_IDR0 + i );
				if ( hw_byte != netdev->hw_addr[i] ) {
					success = 0;
					break;
				}
			}
			
			if ( success ) {
				DBGC ( rtl, "REALTEK %p RTL8125 MAC programming successful with alternative method\n", rtl );
			} else {
				DBGC ( rtl, "REALTEK %p RTL8125 MAC programming failed - IDR registers may be read-only\n", rtl );
				DBGC ( rtl, "REALTEK %p RTL8125 will continue with software MAC - hardware may not receive packets\n", rtl );
			}
		}
		
		/* Lock registers back */
		writeb ( 0x00, rtl->regs + RTL_9346CR );
		
	} else {
		/* Legacy chips - use standard method */
		for ( i = 0 ; i < ETH_ALEN ; i++ ) {
			writeb ( netdev->hw_addr[i], rtl->regs + RTL_IDR0 + i );
		}
		success = 1;  /* Assume success for legacy chips */
	}
	
	/* Read back and verify MAC address for all chips */
	DBGC ( rtl, "REALTEK %p verifying MAC address in hardware:\n", rtl );
	for ( i = 0 ; i < ETH_ALEN ; i++ ) {
		uint8_t hw_byte = readb ( rtl->regs + RTL_IDR0 + i );
		DBGC ( rtl, "REALTEK %p   IDR%d: wrote %02x, read %02x %s\n", 
		       rtl, i, netdev->hw_addr[i], hw_byte,
		       (hw_byte == netdev->hw_addr[i]) ? "OK" : "MISMATCH!" );
	}
}

/**
 * Open network device
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 */
static int realtek_open ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	uint32_t tcr;
	uint32_t rcr;
	int rc;

	DBGC ( rtl, "REALTEK %p opening with MAC address: %s\n", 
	       rtl, eth_ntoa ( netdev->hw_addr ) );

	/* Create transmit descriptor ring */
	if ( ( rc = realtek_create_ring ( rtl, &rtl->tx ) ) != 0 )
		goto err_create_tx;

	/* Create receive descriptor ring */
	if ( ( rc = realtek_create_ring ( rtl, &rtl->rx ) ) != 0 )
		goto err_create_rx;

	/* Create receive buffer */
	if ( ( rc = realtek_create_buffer ( rtl ) ) != 0 )
		goto err_create_buffer;

	/* Program MAC address into hardware registers */
	realtek_set_mac_address ( netdev );

	/* On 8125 ensure C+ (descriptor mode) is on */
	if ( rtl->use_8125 )
		realtek_enable_cplus ( rtl );

	/* Accept all packets */
	writel ( 0xffffffffUL, rtl->regs + RTL_MAR0 );
	writel ( 0xffffffffUL, rtl->regs + RTL_MAR4 );

	/* Configure TCR/RCR BEFORE enabling TE|RE (safer for 8125) */
	/* Configure transmitter */
	tcr = readl ( rtl->regs + RTL_TCR );
	tcr &= ~RTL_TCR_MXDMA_MASK;
	tcr |= RTL_TCR_MXDMA_DEFAULT;
	writel ( tcr, rtl->regs + RTL_TCR );

	/* Configure receiver */
	rcr = readl ( rtl->regs + RTL_RCR );
	rcr &= ~( RTL_RCR_STOP_WORKING | RTL_RCR_RXFTH_MASK |
		  RTL_RCR_RBLEN_MASK | RTL_RCR_MXDMA_MASK );
	rcr |= ( RTL_RCR_RXFTH_DEFAULT | RTL_RCR_RBLEN_DEFAULT |
		 RTL_RCR_MXDMA_DEFAULT | RTL_RCR_WRAP | RTL_RCR_AB |
		 RTL_RCR_AM | RTL_RCR_APM | RTL_RCR_AAP );
	writel ( rcr, rtl->regs + RTL_RCR );

	/* Now enable TX/RX */
	/* Enable transmitter and receiver.  RTL8139 requires that
	 * this happens before writing to RCR.
	 */
	writeb ( ( RTL_CR_TE | RTL_CR_RE ), rtl->regs + RTL_CR );

	/* Fill receive ring */
	realtek_refill_rx ( rtl );

	/* Update link state */
	realtek_check_link ( netdev );

	/* Final MAC address check after everything is initialized */
	DBGC ( rtl, "REALTEK %p final MAC address check at end of open: %s\n", 
	       rtl, eth_ntoa ( netdev->hw_addr ) );
	realtek_set_mac_address ( netdev );

	return 0;

	realtek_destroy_buffer ( rtl );
 err_create_buffer:
	realtek_destroy_ring ( rtl, &rtl->rx );
 err_create_rx:
	realtek_destroy_ring ( rtl, &rtl->tx );
 err_create_tx:
	return rc;
}

/**
 * Close network device
 *
 * @v netdev		Network device
 */
static void realtek_close ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	unsigned int i;

	/* Disable receiver and transmitter */
	writeb ( 0, rtl->regs + RTL_CR );

	/* Destroy receive buffer */
	realtek_destroy_buffer ( rtl );

	/* Destroy receive descriptor ring */
	realtek_destroy_ring ( rtl, &rtl->rx );

	/* Discard any unused receive buffers */
	for ( i = 0 ; i < RTL_NUM_RX_DESC ; i++ ) {
		if ( rtl->rx_iobuf[i] )
			free_rx_iob ( rtl->rx_iobuf[i] );
		rtl->rx_iobuf[i] = NULL;
	}

	/* Destroy transmit descriptor ring */
	realtek_destroy_ring ( rtl, &rtl->tx );

	/* Reset legacy transmit descriptor index, if applicable */
	if ( rtl->legacy )
		realtek_reset ( rtl, NULL );
}

/**
 * Transmit packet
 *
 * @v netdev		Network device
 * @v iobuf		I/O buffer
 * @ret rc		Return status code
 */
static int realtek_transmit ( struct net_device *netdev,
			      struct io_buffer *iobuf ) {
	struct realtek_nic *rtl = netdev->priv;
	struct realtek_descriptor *tx;
	unsigned int tx_idx;
	int is_last;

	/* Get next transmit descriptor */
	if ( ( rtl->tx.prod - rtl->tx.cons ) >= RTL_NUM_TX_DESC ) {
		netdev_tx_defer ( netdev, iobuf );
		return 0;
	}
	tx_idx = ( rtl->tx.prod % RTL_NUM_TX_DESC );

	DBGC2 ( rtl, "REALTEK %p transmit packet %d length %zd\n", 
	        rtl, tx_idx, iob_len ( iobuf ) );

	/* Pad and align packet, if needed */
	if ( rtl->legacy )
		iob_pad ( iobuf, ETH_ZLEN );

	/* Update producer index */
	rtl->tx.prod++;

	/* Transmit packet */
	if ( rtl->legacy ) {

		DBGC2 ( rtl, "REALTEK %p legacy transmit: idx=%d addr=0x%08lx len=%zd\n",
		        rtl, tx_idx, iob_dma ( iobuf ), iob_len ( iobuf ) );
		/* Add to transmit ring */
		writel ( iob_dma ( iobuf ), rtl->regs + RTL_TSAD ( tx_idx ) );
		writel ( ( RTL_TSD_ERTXTH_DEFAULT | iob_len ( iobuf ) ),
			 rtl->regs + RTL_TSD ( tx_idx ) );

	} else {

		/* Populate transmit descriptor */
		is_last = ( tx_idx == ( RTL_NUM_TX_DESC - 1 ) );
		tx = &rtl->tx.desc[tx_idx];
		tx->address = cpu_to_le64 ( iob_dma ( iobuf ) );
		tx->length = cpu_to_le16 ( iob_len ( iobuf ) );
		wmb();
		tx->flags = ( cpu_to_le16 ( RTL_DESC_OWN | RTL_DESC_FS |
					    RTL_DESC_LS ) |
			      ( is_last ? cpu_to_le16 ( RTL_DESC_EOR ) : 0 ) );
		wmb();

		DBGC2 ( rtl, "REALTEK %p descriptor transmit: idx=%d addr=0x%016llx len=%d flags=0x%04x\n",
		        rtl, tx_idx, (unsigned long long)le64_to_cpu(tx->address), 
		        le16_to_cpu(tx->length), le16_to_cpu(tx->flags) );

		/* Notify card that there are packets ready to transmit */
		if ( rtl->use_8125 ) {
			DBGC2 ( rtl, "REALTEK %p RTL8125 doorbell: writing 0x0001 to 0x%02x\n", 
			        rtl, RTL_8125_TPPOLL );
			writew ( 0x0001, rtl->regs + RTL_8125_TPPOLL );
		} else {
			DBGC2 ( rtl, "REALTEK %p legacy doorbell: writing 0x%02x to 0x%02x\n", 
			        rtl, RTL_TPPOLL_NPQ, rtl->tppoll );
			writeb ( RTL_TPPOLL_NPQ, rtl->regs + rtl->tppoll );
		}
	}

	DBGC2 ( rtl, "REALTEK %p TX %d is [%lx,%lx)\n",
		rtl, tx_idx, virt_to_phys ( iobuf->data ),
		virt_to_phys ( iobuf->data ) + iob_len ( iobuf ) );

	return 0;
}

/**
 * Poll for completed packets
 *
 * @v netdev		Network device
 */
static void realtek_poll_tx ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	struct realtek_descriptor *tx;
	unsigned int tx_idx;

	/* Check for completed packets */
	while ( rtl->tx.cons != rtl->tx.prod ) {

		/* Get next transmit descriptor */
		tx_idx = ( rtl->tx.cons % RTL_NUM_TX_DESC );

		/* Stop if descriptor is still in use */
		if ( rtl->legacy ) {

			/* Check ownership bit in transmit status register */
			uint32_t tsd = readl ( rtl->regs + RTL_TSD ( tx_idx ) );
			if ( ! ( tsd & RTL_TSD_OWN ) )
				return;
			DBGC2 ( rtl, "REALTEK %p legacy TX %d completed (TSD=0x%08x)\n", 
			        rtl, tx_idx, tsd );

		} else {

			/* Check ownership bit in descriptor */
			tx = &rtl->tx.desc[tx_idx];
			if ( tx->flags & cpu_to_le16 ( RTL_DESC_OWN ) )
				return;
			DBGC2 ( rtl, "REALTEK %p descriptor TX %d completed (flags=0x%04x)\n", 
			        rtl, tx_idx, le16_to_cpu(tx->flags) );
		}

		DBGC2 ( rtl, "REALTEK %p TX %d complete\n", rtl, tx_idx );

		/* Complete TX descriptor */
		rtl->tx.cons++;
		netdev_tx_complete_next ( netdev );
	}
}

/**
 * Poll for received packets (legacy mode)
 *
 * @v netdev		Network device
 */
static void realtek_legacy_poll_rx ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	struct realtek_legacy_header *rx;
	struct io_buffer *iobuf;
	size_t len;

	/* Check for received packets */
	while ( ! ( readb ( rtl->regs + RTL_CR ) & RTL_CR_BUFE ) ) {

		/* Extract packet from receive buffer */
		rx = ( rtl->rxbuf.data + rtl->rxbuf.offset );
		len = le16_to_cpu ( rx->length );
		if ( rx->status & cpu_to_le16 ( RTL_STAT_ROK ) ) {

			DBGC2 ( rtl, "REALTEK %p RX offset %x+%zx\n",
				rtl, rtl->rxbuf.offset, len );

			/* Allocate I/O buffer */
			iobuf = alloc_iob ( len );
			if ( ! iobuf ) {
				netdev_rx_err ( netdev, NULL, -ENOMEM );
				/* Leave packet for next poll */
				break;
			}

			/* Copy data to I/O buffer */
			memcpy ( iob_put ( iobuf, len ), rx->data, len );
			iob_unput ( iobuf, 4 /* strip CRC */ );

			/* Hand off to network stack */
			netdev_rx ( netdev, iobuf );

		} else {

			DBGC ( rtl, "REALTEK %p RX offset %x+%zx error %04x\n",
			       rtl, rtl->rxbuf.offset, len,
			       le16_to_cpu ( rx->status ) );
			netdev_rx_err ( netdev, NULL, -EIO );
		}

		/* Update buffer offset */
		rtl->rxbuf.offset += ( sizeof ( *rx ) + len );
		rtl->rxbuf.offset = ( ( rtl->rxbuf.offset + 3 ) & ~3 );
		rtl->rxbuf.offset = ( rtl->rxbuf.offset % RTL_RXBUF_LEN );
		writew ( ( rtl->rxbuf.offset - 16 ), rtl->regs + RTL_CAPR );

		/* Give chip time to react before rechecking RTL_CR */
		readw ( rtl->regs + RTL_CAPR );
	}
}

/**
 * Poll for received packets
 *
 * @v netdev		Network device
 */
static void realtek_poll_rx ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	struct realtek_descriptor *rx;
	struct io_buffer *iobuf;
	unsigned int rx_idx;
	size_t len;

	/* Poll receive buffer if in legacy mode */
	if ( rtl->legacy ) {
		realtek_legacy_poll_rx ( netdev );
		return;
	}

	/* Check for received packets */
	while ( rtl->rx.cons != rtl->rx.prod ) {

		/* Get next receive descriptor */
		rx_idx = ( rtl->rx.cons % RTL_NUM_RX_DESC );
		rx = &rtl->rx.desc[rx_idx];

		/* Stop if descriptor is still in use */
		if ( rx->flags & cpu_to_le16 ( RTL_DESC_OWN ) )
			return;

		/* Populate I/O buffer */
		iobuf = rtl->rx_iobuf[rx_idx];
		rtl->rx_iobuf[rx_idx] = NULL;
		len = ( le16_to_cpu ( rx->length ) & RTL_DESC_SIZE_MASK );
		iob_put ( iobuf, ( len - 4 /* strip CRC */ ) );

		/* Hand off to network stack */
		if ( rx->flags & cpu_to_le16 ( RTL_DESC_RES ) ) {
			DBGC ( rtl, "REALTEK %p RX %d error (length %zd, "
			       "flags %04x)\n", rtl, rx_idx, len,
			       le16_to_cpu ( rx->flags ) );
			netdev_rx_err ( netdev, iobuf, -EIO );
		} else {
			uint8_t *packet = ( uint8_t * ) iobuf->data;
			DBGC2 ( rtl, "REALTEK %p RX %d complete (length "
				"%zd)\n", rtl, rx_idx, len );
			DBGC2 ( rtl, "REALTEK %p RX packet dest: %02x:%02x:%02x:%02x:%02x:%02x\n",
				rtl, packet[0], packet[1], packet[2],
				packet[3], packet[4], packet[5] );
			DBGC2 ( rtl, "REALTEK %p RX packet src:  %02x:%02x:%02x:%02x:%02x:%02x\n",
				rtl, packet[6], packet[7], packet[8],
				packet[9], packet[10], packet[11] );
			if ( len >= 14 ) {
				uint16_t ethertype = ( packet[12] << 8 ) | packet[13];
				DBGC2 ( rtl, "REALTEK %p RX packet ethertype: 0x%04x\n", rtl, ethertype );
			}
			netdev_rx ( netdev, iobuf );
		}
		rtl->rx.cons++;
	}
}

/**
 * Poll for completed and received packets
 *
 * @v netdev		Network device
 */
static void realtek_poll ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	uint32_t isr32;
	uint16_t isr;

	/* MAC address checking removed - no longer needed since we generate it correctly */

	if ( rtl->use_8125 ) {
		/* RTL8125 uses 32-bit ISR */
		isr32 = readl ( rtl->regs + RTL_8125_ISR );
		if ( ! isr32 )
			return;
		writel ( isr32, rtl->regs + RTL_8125_ISR );
		
		DBGC2 ( rtl, "REALTEK %p RTL8125 ISR: 0x%08x\n", rtl, isr32 );
		
		/* Extract relevant interrupt bits (low 16 bits match old layout) */
		isr = (uint16_t)( isr32 & 0xffff );
	} else {
		/* Traditional chips use 16-bit ISR */
		isr = readw ( rtl->regs + RTL_ISR );
		if ( ! isr )
			return;
		writew ( isr, rtl->regs + RTL_ISR );
		
		DBGC2 ( rtl, "REALTEK %p legacy ISR: 0x%04x\n", rtl, isr );
	}

	/* Poll for TX completions, if applicable */
	if ( isr & ( RTL_IRQ_TER | RTL_IRQ_TOK ) ) {
		DBGC2 ( rtl, "REALTEK %p TX interrupt (TER=%d TOK=%d)\n", 
		        rtl, !!(isr & RTL_IRQ_TER), !!(isr & RTL_IRQ_TOK) );
		realtek_poll_tx ( netdev );
	}

	/* Poll for RX completionsm, if applicable */
	if ( isr & ( RTL_IRQ_RER | RTL_IRQ_ROK ) ) {
		DBGC2 ( rtl, "REALTEK %p RX interrupt (RER=%d ROK=%d)\n", 
		        rtl, !!(isr & RTL_IRQ_RER), !!(isr & RTL_IRQ_ROK) );
		realtek_poll_rx ( netdev );
	}

	/* Check link state, if applicable */
	if ( isr & RTL_IRQ_PUN_LINKCHG ) {
		DBGC2 ( rtl, "REALTEK %p link change interrupt\n", rtl );
		realtek_check_link ( netdev );
	}

	/* Refill RX ring */
	realtek_refill_rx ( rtl );
}

/**
 * Enable or disable interrupts
 *
 * @v netdev		Network device
 * @v enable		Interrupts should be enabled
 */
static void realtek_irq ( struct net_device *netdev, int enable ) {
	struct realtek_nic *rtl = netdev->priv;
	uint32_t mask;

	/* Set interrupt mask */
	mask = ( enable ? ( RTL_IRQ_PUN_LINKCHG | RTL_IRQ_TER | RTL_IRQ_TOK |
			    RTL_IRQ_RER | RTL_IRQ_ROK ) : 0 );

	if ( rtl->use_8125 ) {
		writel ( mask, rtl->regs + RTL_8125_IMR );
	} else {
		writew ( (uint16_t)mask, rtl->regs + RTL_IMR );
	}
}

/** Realtek network device operations */
static struct net_device_operations realtek_operations = {
	.open		= realtek_open,
	.close		= realtek_close,
	.transmit	= realtek_transmit,
	.poll		= realtek_poll,
	.irq		= realtek_irq,
};

/******************************************************************************
 *
 * PCI interface
 *
 ******************************************************************************
 */

/**
 * Detect device type
 *
 * @v rtl		Realtek device
 * @v pci		PCI device
 */
static void realtek_detect ( struct realtek_nic *rtl, struct pci_device *pci ) {
	uint16_t rms;
	uint16_t check_rms;
	uint16_t cpcr;
	uint16_t check_cpcr;

	/* Check for RTL8125 family by PCI device ID */
	DBGC ( rtl, "REALTEK %p PCI device detection: vendor=0x%04x device=0x%04x\n", 
	       rtl, pci->vendor, pci->device );
	if ( RTL_IS_8125_FAMILY ( pci->vendor, pci->device ) ) {
		DBGC ( rtl, "REALTEK %p appears to be an RTL8125 family device (ID=0x%04x)\n", rtl, pci->device );
		DBGC ( rtl, "REALTEK %p RTL8125 configuration: use_8125=1, have_phy_regs=1, tppoll=0x%02x\n", 
		       rtl, RTL_8125_TPPOLL );
		rtl->use_8125 = 1;
		rtl->have_phy_regs = 1;
		rtl->tppoll = RTL_8125_TPPOLL;
		rtl->eeprom.bus = &rtl->spibit.bus;
		DBGC ( rtl, "REALTEK %p RTL8125 family device detection completed\n", rtl );
		return;
	}

	/* Default to non-8125 mode */
	DBGC ( rtl, "REALTEK %p not RTL8125 family, proceeding with legacy detection\n", rtl );
	rtl->use_8125 = 0;

	/* The RX Packet Maximum Size register is present only on
	 * 8169.  Try to set to our intended MTU.
	 */
	DBGC ( rtl, "REALTEK %p testing RMS register for 8169 detection\n", rtl );
	rms = RTL_RX_MAX_LEN;
	writew ( rms, rtl->regs + RTL_RMS );
	check_rms = readw ( rtl->regs + RTL_RMS );
	DBGC ( rtl, "REALTEK %p RMS test: wrote=0x%04x read=0x%04x\n", rtl, rms, check_rms );

	/* The C+ Command register is present only on 8169 and 8139C+.
	 * Try to enable C+ mode and PCI Dual Address Cycle (for
	 * 64-bit systems), if supported.
	 *
	 * Note that enabling DAC seems to cause bizarre behaviour
	 * (lockups, garbage data on the wire) on some systems, even
	 * if only 32-bit addresses are used.
	 *
	 * Disable VLAN offload, since some cards seem to have it
	 * enabled by default.
	 */
	DBGC ( rtl, "REALTEK %p testing C+ Command register for 8169/8139C+ detection\n", rtl );
	cpcr = readw ( rtl->regs + RTL_CPCR );
	DBGC ( rtl, "REALTEK %p C+ Command original value: 0x%04x\n", rtl, cpcr );
	cpcr |= ( RTL_CPCR_MULRW | RTL_CPCR_CPRX | RTL_CPCR_CPTX );
	if ( sizeof ( physaddr_t ) > sizeof ( uint32_t ) )
		cpcr |= RTL_CPCR_DAC;
	cpcr &= ~RTL_CPCR_VLAN;
	DBGC ( rtl, "REALTEK %p C+ Command writing value: 0x%04x\n", rtl, cpcr );
	writew ( cpcr, rtl->regs + RTL_CPCR );
	check_cpcr = readw ( rtl->regs + RTL_CPCR );
	DBGC ( rtl, "REALTEK %p C+ Command readback value: 0x%04x\n", rtl, check_cpcr );

	/* Detect device type */
	DBGC ( rtl, "REALTEK %p device type detection starting\n", rtl );
	if ( check_rms == rms ) {
		DBGC ( rtl, "REALTEK %p appears to be an RTL8169\n", rtl );
		DBGC ( rtl, "REALTEK %p RTL8169 configuration: have_phy_regs=1, tppoll=0x%02x\n", 
		       rtl, RTL_TPPOLL );
		rtl->have_phy_regs = 1;
		rtl->tppoll = RTL_TPPOLL;
	} else {
		if ( ( check_cpcr == cpcr ) && ( cpcr != 0xffff ) ) {
			DBGC ( rtl, "REALTEK %p appears to be an RTL8139C+\n",
			       rtl );
			DBGC ( rtl, "REALTEK %p RTL8139C+ configuration: tppoll=0x%02x\n", 
			       rtl, RTL_TPPOLL_8139CP );
			rtl->tppoll = RTL_TPPOLL_8139CP;
		} else {
			DBGC ( rtl, "REALTEK %p appears to be an RTL8139\n",
			       rtl );
			DBGC ( rtl, "REALTEK %p RTL8139 configuration: legacy=1\n", rtl );
			rtl->legacy = 1;
		}
		rtl->eeprom.bus = &rtl->spibit.bus;
	}
}

/**
 * Probe PCI device
 *
 * @v pci		PCI device
 * @ret rc		Return status code
 */
static int realtek_probe ( struct pci_device *pci ) {
	struct net_device *netdev;
	struct realtek_nic *rtl;
	int rc;

	DBGC ( NULL, "REALTEK probing PCI device %04x:%04x\n", pci->vendor, pci->device );

	/* Allocate and initialise net device */
	netdev = alloc_etherdev ( sizeof ( *rtl ) );
	if ( ! netdev ) {
		rc = -ENOMEM;
		DBGC ( NULL, "REALTEK failed to allocate network device\n" );
		goto err_alloc;
	}
	netdev_init ( netdev, &realtek_operations );
	rtl = netdev->priv;
	pci_set_drvdata ( pci, netdev );
	netdev->dev = &pci->dev;
	memset ( rtl, 0, sizeof ( *rtl ) );
	realtek_init_ring ( &rtl->tx, RTL_NUM_TX_DESC, RTL_TNPDS );
	realtek_init_ring ( &rtl->rx, RTL_NUM_RX_DESC, RTL_RDSAR );

	DBGC ( rtl, "REALTEK %p allocated and initialized\n", rtl );

	/* Fix up PCI device */
	adjust_pci_device ( pci );
	DBGC ( rtl, "REALTEK %p PCI device adjusted\n", rtl );

	/* Map registers */
	size_t bar_size = RTL_BAR_SIZE;
	if ( RTL_IS_8125_FAMILY ( pci->vendor, pci->device ) ) {
		/* RTL8125 family - use larger BAR size (4KB to match Linux driver) */
		bar_size = RTL_BAR_SIZE_RTL8125;
	}
	DBGC ( rtl, "REALTEK %p mapping BAR of size 0x%zx at address 0x%08lx\n", 
	       rtl, bar_size, pci->membase );
	rtl->regs = pci_ioremap ( pci, pci->membase, bar_size );
	if ( ! rtl->regs ) {
		DBGC ( rtl, "REALTEK %p failed to map PCI BAR\n", rtl );
		rc = -ENODEV;
		goto err_ioremap;
	}
	DBGC ( rtl, "REALTEK %p registers mapped to %p\n", rtl, rtl->regs );

	/* Configure DMA */
	rtl->dma = &pci->dma;
	DBGC ( rtl, "REALTEK %p DMA device configured\n", rtl );

	/* Reset the NIC */
	if ( ( rc = realtek_reset ( rtl, pci ) ) != 0 )
		goto err_reset;

	/* Detect device type */
	realtek_detect ( rtl, pci );

	/* Try to read MAC address from hardware first */
	DBGC ( rtl, "REALTEK %p attempting to read MAC from hardware\n", rtl );
	if ( realtek_read_mac_from_hardware ( netdev ) == 0 ) {
		DBGC ( rtl, "REALTEK %p using valid hardware MAC: %s\n", 
		       rtl, eth_ntoa ( netdev->hw_addr ) );
	} else {
		/* No valid MAC in hardware, generate a locally administered one */
		DBGC ( rtl, "REALTEK %p no valid hardware MAC found, generating temporary locally administered MAC address\n", rtl );
		
		/* Use a locally administered MAC with QEMU-style prefix for compatibility */
		netdev->hw_addr[0] = 0x52;  /* Locally administered, unicast (QEMU style) */
		netdev->hw_addr[1] = 0x54;  /* QEMU standard prefix */
		netdev->hw_addr[2] = 0x00;
		netdev->hw_addr[3] = 0x12;
		netdev->hw_addr[4] = ( pci->busdevfn >> 8 ) & 0xff;  /* Bus number for uniqueness */
		netdev->hw_addr[5] = pci->busdevfn & 0xff;           /* Device/function for uniqueness */
		
		DBGC ( rtl, "REALTEK %p generated temporary MAC address: %s (bus=%02x dev/fn=%02x)\n", 
		       rtl, eth_ntoa ( netdev->hw_addr ), 
		       (pci->busdevfn >> 8) & 0xff, pci->busdevfn & 0xff );
	}

	/* Disable new TX descriptor format for RTL8125 */
	if ( rtl->use_8125 ) {
		DBGC ( rtl, "REALTEK %p RTL8125 detected, testing OCP and disabling new descriptors\n", rtl );
		if ( realtek_ocp_ready ( rtl ) )
			realtek_disable_new_desc ( rtl ); /* best effort; non-fatal on timeout */
	}

	/* Initialise and reset MII interface */
	DBGC ( rtl, "REALTEK %p initializing MII interface\n", rtl );
	mdio_init ( &rtl->mdio, &realtek_mii_operations );
	mii_init ( &rtl->mii, &rtl->mdio, 0 );
	if ( ( rc = realtek_phy_reset ( rtl ) ) != 0 )
		goto err_phy_reset;

	/* Enable C+ mode for RTL8125 after reset */
	if ( rtl->use_8125 )
		realtek_enable_cplus ( rtl );
	
	/* Minimal PHY bring-up so DHCP works */
	realtek_phy_up ( rtl );

	/* Register network device */
	DBGC ( rtl, "REALTEK %p registering network device with MAC %s\n", 
	       rtl, eth_ntoa ( netdev->hw_addr ) );
	if ( ( rc = register_netdev ( netdev ) ) != 0 )
		goto err_register_netdev;

	/* Check MAC address after registration - it might have changed */
	DBGC ( rtl, "REALTEK %p MAC address after registration: %s\n", 
	       rtl, eth_ntoa ( netdev->hw_addr ) );

	/* Set initial link state */
	realtek_check_link ( netdev );

	/* Register non-volatile options, if applicable */
	if ( rtl->nvo.nvs ) {
		DBGC ( rtl, "REALTEK %p registering non-volatile options\n", rtl );
		if ( ( rc = register_nvo ( &rtl->nvo,
					   netdev_settings ( netdev ) ) ) != 0)
			goto err_register_nvo;
	}

	DBGC ( rtl, "REALTEK %p probe completed successfully\n", rtl );

	return 0;

 err_register_nvo:
	unregister_netdev ( netdev );
 err_register_netdev:
 err_phy_reset:
	realtek_reset ( rtl, pci );
 err_reset:
	iounmap ( rtl->regs );
 err_ioremap:
	netdev_nullify ( netdev );
	netdev_put ( netdev );
 err_alloc:
	return rc;
}

/**
 * Remove PCI device
 *
 * @v pci		PCI device
 */
static void realtek_remove ( struct pci_device *pci ) {
	struct net_device *netdev = pci_get_drvdata ( pci );
	struct realtek_nic *rtl = netdev->priv;

	/* Unregister non-volatile options, if applicable */
	if ( rtl->nvo.nvs )
		unregister_nvo ( &rtl->nvo );

	/* Unregister network device */
	unregister_netdev ( netdev );

	/* Reset card */
	realtek_reset ( rtl, pci );

	/* Free network device */
	iounmap ( rtl->regs );
	netdev_nullify ( netdev );
	netdev_put ( netdev );
}

/** Realtek PCI device IDs */
static struct pci_device_id realtek_nics[] = {
	PCI_ROM ( 0x0001, 0x8168, "clone8169",	"Cloned 8169", 0 ),
	PCI_ROM ( 0x018a, 0x0106, "fpc0106tx",	"LevelOne FPC-0106TX", 0 ),
	PCI_ROM ( 0x021b, 0x8139, "hne300",	"Compaq HNE-300", 0 ),
	PCI_ROM ( 0x02ac, 0x1012, "s1012",	"SpeedStream 1012", 0 ),
	PCI_ROM ( 0x0357, 0x000a, "ttpmon",	"TTTech TTP-Monitoring", 0 ),
	PCI_ROM ( 0x10ec, 0x8125, "rtl8125",	"RTL-8125", 0 ),
	PCI_ROM ( 0x10ec, 0x8129, "rtl8129",	"RTL-8129", 0 ),
	PCI_ROM ( 0x10ec, 0x8136, "rtl8136",	"RTL8101E/RTL8102E", 0 ),
	PCI_ROM ( 0x10ec, 0x8138, "rtl8138",	"RT8139 (B/C)", 0 ),
	PCI_ROM ( 0x10ec, 0x8139, "rtl8139",	"RTL-8139/8139C/8139C+", 0 ),
	PCI_ROM ( 0x10ec, 0x8167, "rtl8167",	"RTL-8110SC/8169SC", 0 ),
	PCI_ROM ( 0x10ec, 0x8168, "rtl8168",	"RTL8111/8168B", 0 ),
	PCI_ROM ( 0x10ec, 0x8169, "rtl8169",	"RTL-8169", 0 ),
	PCI_ROM ( 0x1113, 0x1211, "smc1211",	"SMC2-1211TX", 0 ),
	PCI_ROM ( 0x1186, 0x1300, "dfe538",	"DFE530TX+/DFE538TX", 0 ),
	PCI_ROM ( 0x1186, 0x1340, "dfe690",	"DFE-690TXD", 0 ),
	PCI_ROM ( 0x1186, 0x4300, "dge528t",	"DGE-528T", 0 ),
	PCI_ROM ( 0x11db, 0x1234, "sega8139",	"Sega Enterprises 8139", 0 ),
	PCI_ROM ( 0x1259, 0xa117, "allied8139",	"Allied Telesyn 8139", 0 ),
	PCI_ROM ( 0x1259, 0xa11e, "allied81xx",	"Allied Telesyn 81xx", 0 ),
	PCI_ROM ( 0x1259, 0xc107, "allied8169",	"Allied Telesyn 8169", 0 ),
	PCI_ROM ( 0x126c, 0x1211, "northen8139","Northern Telecom 8139", 0 ),
	PCI_ROM ( 0x13d1, 0xab06, "fe2000vx",	"Abocom FE2000VX", 0 ),
	PCI_ROM ( 0x1432, 0x9130, "edi8139",	"Edimax 8139", 0 ),
	PCI_ROM ( 0x14ea, 0xab06, "fnw3603tx",	"Planex FNW-3603-TX", 0 ),
	PCI_ROM ( 0x14ea, 0xab07, "fnw3800tx",	"Planex FNW-3800-TX", 0 ),
	PCI_ROM ( 0x1500, 0x1360, "delta8139",	"Delta Electronics 8139", 0 ),
	PCI_ROM ( 0x16ec, 0x0116, "usr997902",	"USR997902", 0 ),
	PCI_ROM ( 0x1737, 0x1032, "linksys8169","Linksys 8169", 0 ),
	PCI_ROM ( 0x1743, 0x8139, "rolf100",	"Peppercorn ROL/F-100", 0 ),
	PCI_ROM ( 0x4033, 0x1360, "addron8139",	"Addtron 8139", 0 ),
	PCI_ROM ( 0xffff, 0x8139, "clonse8139",	"Cloned 8139", 0 ),
};

/** Realtek PCI driver */
struct pci_driver realtek_driver __pci_driver = {
	.ids = realtek_nics,
	.id_count = ( sizeof ( realtek_nics ) / sizeof ( realtek_nics[0] ) ),
	.probe = realtek_probe,
	.remove = realtek_remove,
};
