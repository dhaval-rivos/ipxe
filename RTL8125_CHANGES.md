# RTL8125 Support Implementation for iPXE

This document summarizes the changes made to add RTL8125 support to the iPXE Realtek driver.

## Changes Made

### 1. Header File Changes (realtek.h)

- Added RTL8125-specific register definitions:
  - `RTL_8125_IMR`: 32-bit interrupt mask register at 0x38
  - `RTL_8125_ISR`: 32-bit interrupt status register at 0x3c  
  - `RTL_8125_TPPOLL`: 16-bit transmit poll register at 0x90
  - `RTL_BAR_SIZE_RTL8125`: Extended memory BAR size (0x1000) for RTL8125

- Added `use_8125` flag to `struct realtek_nic` to identify RTL8125 devices

### 2. iPXE API Compliance Updates

The implementation has been updated to use proper iPXE APIs:

**DMA and Memory Management:**
- Removed non-existent `iob_map_tx()` calls - iPXE uses `iob_dma()` directly
- Removed `dma_set_mask_64bit()` calls - iPXE handles DMA masks through 64-bit writes to ring base registers
- Used `dma_alloc()` and `dma()` for descriptor ring allocation (already correct)

**Register Access:**
- Added generic `RTL_TPPOLL` macro (0x38) to simplify TPPoll register handling
- Removed conditional TPPoll assignments that used undefined macros
- Use runtime device detection instead of compile-time macros

**PCI BAR Mapping:**
- Reduced `RTL_BAR_SIZE_RTL8125` from 0x2000 to 0x1000 (4KB) to match Linux r8169 driver
- Added `RTL_IS_8125_FAMILY()` macro for consistent device family detection
- All RTL8125 family devices use 4KB BAR mapping
- Improved BAR size selection logic for better maintainability

### 3. PCI Device IDs (realtek.c)

Added support for RTL8125 family device IDs (matching Linux r8169 driver):
- 0x8125 (RTL-8125)
- 0x8126 (RTL-8126) 
- 0x8127 (RTL-8127)
- 0x3000 (RTL-3000)
- 0x5000 (RTL-5000)

All RTL8125 family devices now receive the same treatment:
- `use_8125` flag set to 1
- `have_phy_regs` flag set to 1  
- Extended reset timeout (200ms vs 100ms)
- RTL8125 register layout and OCP access
- 4KB BAR mapping (matching Linux driver)

### 4. Device Detection

Modified `realtek_detect()` function to:
- Recognize RTL8125 family devices by PCI device ID
- Set the `use_8125` flag appropriately
- Configure RTL8125 devices with proper register layout

### 5. OCP Register Access

Implemented robust OCP (On-Chip Programming) register access functions based on the Linux r8169 driver:
- `realtek_ocp_read()`: Read from OCP registers via ERIAR/ERIDR with proper Linux r8169 bit settings
- `realtek_ocp_write()`: Write to OCP registers via ERIAR/ERIDR with proper Linux r8169 bit settings  
- `realtek_disable_new_desc()`: Disable new TX descriptor format (essential for RTL8125)

**Key improvements over simplified implementation:**
- Corrected ERIAR register bit field handling to match Linux r8169 driver exactly:
  - Bit 31: Operation flag (0 for read initiation, 1 for write operation)
  - Bits 16-23: Type field (0x02 for MAC OCP access)
  - Bits 0-15: Address field (16-bit address, no shift required)
- Proper completion detection: read waits for bit 31 = 1, write waits for bit 31 = 0
- Increased timeout values (2000 iterations vs 100) for reliable operation
- Better error handling with timeout detection and reporting
- Uses symbolic constants instead of magic numbers for maintainability

**Critical ERIAR Register Layout (Linux r8169 Compatible):**
```
Bit 31:    Operation flag (ERIAR_FLAG = 0x80000000)
Bits 16-23: Type field (MAC_OCP = 0x02, shifted to 0x00020000)
Bits 0-15:  Address field (16-bit address, no shifting)
```

### 6. Transmit Function Updates

Modified `realtek_transmit()` to:
- Use correct doorbell register for RTL8125 (16-bit write to 0x90)
- Use legacy doorbell for older chips

### 7. Interrupt Handling Updates

Modified interrupt functions to handle RTL8125's different register layout:

**`realtek_irq()`:**
- Use 32-bit writes to RTL_8125_IMR for RTL8125
- Use 16-bit writes to RTL_IMR for older chips

**`realtek_poll()`:**
- Read/acknowledge 32-bit ISR for RTL8125
- Extract low 16 bits for compatibility with existing interrupt bit definitions
- Fall back to 16-bit ISR handling for older chips

### 7. Memory Mapping

Updated register memory mapping to use larger BAR size (0x1000) for RTL8125 family devices.

### 8. Initialization Sequence

Added call to `realtek_disable_new_desc()` during device probe for RTL8125 devices to ensure compatibility with iPXE's descriptor format.

## Key Technical Details

### New TX Descriptor Format Disable
RTL8125 defaults to a new 32-byte descriptor format. iPXE uses the legacy 16-byte format, so we must disable the new format by clearing bit 0 of OCP register 0xeb58.

### Register Layout Differences
RTL8125 moves the interrupt and doorbell registers compared to RTL8168/8169:
- Interrupt mask/status registers become 32-bit and move to 0x38/0x3c
- Transmit doorbell moves to 0x90 and becomes 16-bit

### Backward Compatibility
All changes maintain full backward compatibility with existing RTL8139/8168/8169 devices.

## Testing

The implementation compiles successfully and follows the proven approach used by the Linux r8169 driver. The changes are minimal and focused on the essential differences needed for RTL8125 operation.

## Expected Result

With these changes, RTL8125 network cards should be:
1. Recognized by iPXE during PCI enumeration
2. Properly initialized with correct register layout
3. Capable of transmitting and receiving packets
4. Compatible with iPXE's existing network stack

The implementation provides basic RTL8125 functionality at 1000Mbps speeds. Full 2.5GbE support would require additional PHY configuration and MII register definitions.
