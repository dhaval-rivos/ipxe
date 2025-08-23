# RTL8125 Debug Guide for iPXE

This document explains the comprehensive debug output added to the RTL8125 driver for troubleshooting first-time enablement.

## Debug Output Categories

### 1. Device Detection and Initialization

**PCI Device Probing:**
```
REALTEK probing PCI device 04x:04x
REALTEK %p allocated and initialized  
REALTEK %p PCI device adjusted
REALTEK %p mapping BAR of size 0x%zx at address 0x%08lx
REALTEK %p registers mapped to %p
REALTEK %p DMA device configured
```

**Device Type Detection:**
```
REALTEK %p PCI device detection: vendor=0x%04x device=0x%04x
REALTEK %p appears to be an RTL8125 family device (ID=0x%04x)
REALTEK %p RTL8125 configuration: use_8125=1, have_phy_regs=1, tppoll=0x%02x
REALTEK %p RTL8125 family device detection completed
```

**Legacy Device Fallback:**
```
REALTEK %p not RTL8125 family, proceeding with legacy detection
REALTEK %p testing RMS register for 8169 detection
REALTEK %p RMS test: wrote=0x%04x read=0x%04x
REALTEK %p testing C+ Command register for 8169/8139C+ detection
REALTEK %p device type detection starting
```

### 2. Device Reset and Initialization

**Reset Process:**
```
REALTEK %p starting device reset
REALTEK %p RTL8125 family using extended reset timeout (%dms)
REALTEK %p issuing reset command
REALTEK %p reset completed after %dms
REALTEK %p RTL8125 family post-reset settling delay
REALTEK %p reset successful
```

**RTL8125 Specific Configuration:**
```
REALTEK %p RTL8125 family detected, disabling new descriptor format
REALTEK %p disabling RTL8125 new TX descriptor format
REALTEK %p OCP 0xeb58 original value: 0x%04x
REALTEK %p OCP 0xeb58 modified value: 0x%04x
REALTEK %p OCP 0xeb58 verification read: 0x%04x
REALTEK %p RTL8125 new descriptor format disabled
```

### 3. OCP Register Access (RTL8125)

**Read Operations (Linux r8169 compatible):**
```
REALTEK %p OCP read from address 0x%04x
REALTEK %p OCP writing ERIAR=0x%08x for read
REALTEK %p OCP read completed: addr=0x%04x value=0x%04x (took %d iterations)
REALTEK %p OCP read timeout at address 0x%04x (ERIAR=0x%08x)  [if timeout]
```

**Write Operations (Linux r8169 compatible):**
```
REALTEK %p OCP write to address 0x%04x value 0x%04x
REALTEK %p OCP writing ERIAR=0x%08x for write
REALTEK %p OCP write completed: addr=0x%04x value=0x%04x (took %d iterations)
REALTEK %p OCP write timeout at address 0x%04x (ERIAR=0x%08x)  [if timeout]
```

**ERIAR Register Format (Corrected to match Linux r8169):**
- Read operation: ERIAR = 0x00020000 | address (type=0x02, bit 31=0)
- Write operation: ERIAR = 0x80020000 | address (type=0x02, bit 31=1)
- Expected values: 0x0002eb58 for read, 0x8002eb58 for write at address 0xeb58

### 4. Network Interface Setup

**MAC Address Configuration:**
```
REALTEK %p EEPROM not present, reading MAC from ID registers
REALTEK %p MAC address from registers: %s
```

**Network Device Registration:**
```
REALTEK %p initializing MII interface
REALTEK %p registering network device with MAC %s
REALTEK %p registering non-volatile options
REALTEK %p probe completed successfully
```

### 5. Link Status Monitoring

**Link State Detection:**
```
REALTEK %p checking link state
REALTEK %p PHY status is %02x (%s, Link%s, %sDuplex)
REALTEK %p media status is %02x (Link%s, %dMbps%s)
```

### 6. Packet Transmission

**Transmit Path:**
```
REALTEK %p transmit packet %d length %zd
REALTEK %p legacy transmit: idx=%d addr=0x%08lx len=%zd         [RTL8139]
REALTEK %p descriptor transmit: idx=%d addr=0x%016llx len=%d flags=0x%04x
REALTEK %p RTL8125 doorbell: writing 0x0001 to 0x%02x          [RTL8125]
REALTEK %p legacy doorbell: writing 0x%02x to 0x%02x           [other chips]
```

**Transmit Completion:**
```
REALTEK %p legacy TX %d completed (TSD=0x%08x)                 [RTL8139]
REALTEK %p descriptor TX %d completed (flags=0x%04x)           [others]
```

### 7. Interrupt Handling

**Interrupt Status:**
```
REALTEK %p RTL8125 ISR: 0x%08x                                 [RTL8125]
REALTEK %p legacy ISR: 0x%04x                                  [others]
REALTEK %p TX interrupt (TER=%d TOK=%d)
REALTEK %p RX interrupt (RER=%d ROK=%d)
REALTEK %p link change interrupt
```

## Debug Levels

The driver uses different debug levels:

- **DBGC()**: Standard debug messages (device detection, major operations)
- **DBGC2()**: Detailed debug messages (packet-level operations, register access)

## Enabling Debug Output

To enable debug output when building iPXE:

1. **Enable debug for specific subsystems:**
   ```bash
   # Enable network device debug
   make bin/realtek.rom DEBUG=netdevice

   # Enable all debug output
   make bin/realtek.rom DEBUG=all
   ```

2. **For console output during boot:**
   ```bash
   # Enable console debug
   make bin/realtek.rom DEBUG=all CONSOLE=serial
   ```

## Troubleshooting Common Issues

### RTL8125 Family Not Detected
Look for:
```
REALTEK probing PCI device 10ec:8125  (or 8126, 8127, 3000, 5000)
REALTEK %p appears to be an RTL8125 family device (ID=0x8125)
```

If you see a different device ID, check if it should be added to `RTL_IS_8125_FAMILY()` macro.

### OCP Register Access Issues
Look for timeout messages and verify ERIAR values:
```
REALTEK %p OCP read timeout at address 0xeb58
REALTEK %p OCP write timeout at address 0xeb58
```

**Expected ERIAR values for address 0xeb58:**
- Read: `REALTEK %p OCP writing ERIAR=0x0002eb58 for read`
- Write: `REALTEK %p OCP writing ERIAR=0x8002eb58 for write`

If you see different patterns like 0x8003xxxx or other unexpected formats, the ERIAR bit assignments may be incorrect.

### Descriptor Format Issues
Verify new descriptor format is disabled:
```
REALTEK %p OCP 0xeb58 original value: 0x0001
REALTEK %p OCP 0xeb58 modified value: 0x0000
```

### Link Detection Problems
Check PHY status output:
```
REALTEK %p PHY status is 02 (GMII, LinkUp, FullDuplex)
```

### Transmit Issues
Monitor doorbell operations:
```
REALTEK %p RTL8125 doorbell: writing 0x0001 to 0x90
```

This comprehensive debug output will help identify exactly where the RTL8125 initialization or operation is failing during first-time enablement.
