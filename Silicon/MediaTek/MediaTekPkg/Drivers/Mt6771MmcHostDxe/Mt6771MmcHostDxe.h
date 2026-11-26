/** @file

  Internal header for the OPPO A3 (MT6771) eMMC DXE host driver.

  Register names/bit definitions and the low-level PIO model are derived from
  the GPL-2.0-only MediaTek MSDC WIP driver in edk2-mtk, branch origin/next-msdc:

    Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.h
    Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.c
    Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcUtils.c

  This file adds only eMMC/MMC-host-protocol plumbing needed for
  EmbeddedPkg/Universal/MmcDxe. It intentionally contains no SD card
  identification logic.

  Copyright (c) 2025, Arseniy Velikanov <me@adomerle.pw> (register definitions
    and low-level MSDC access model; GPL-2.0-only)
  SPDX-License-Identifier: GPL-2.0-only

**/

#ifndef MT6771_MMC_HOST_DXE_H_
#define MT6771_MMC_HOST_DXE_H_

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/MmcHost.h>

//
// Base addresses are build-time PCDs. OPPO A3 values come from the extracted
// DTB facts / mainline DTS patch:
//   mainline-linux-mt6771/patch/0005-a3-OPPO-A3-MT6771-bring-up.patch:513
//     msdc@11230000, reg <0x0 0x11230000 0x0 0x10000>, bus-width=8,
//     non-removable, bootable, max-frequency=0xbebc200
//   same patch:662
//     msdc0_top@11f50000, reg <0x0 0x11f50000 0x0 0x1000>
//

#define MT6771_MSDC_FIFO_SIZE 128u
#define MT6771_MSDC_BLOCK_SIZE 512u

//
// Register offsets, copied from origin/next-msdc:MsdcDxe.h.
//
#define MSDC_CFG         0x000u
#define MSDC_IOCON       0x004u
#define MSDC_PS          0x008u
#define MSDC_INT         0x00cu
#define MSDC_INTEN       0x010u
#define MSDC_FIFOCS      0x014u
#define MSDC_TXDATA      0x018u
#define MSDC_RXDATA      0x01cu
#define SDC_CFG          0x030u
#define SDC_CMD          0x034u
#define SDC_ARG          0x038u
#define SDC_STS          0x03cu
#define SDC_RESP0        0x040u
#define SDC_RESP1        0x044u
#define SDC_RESP2        0x048u
#define SDC_RESP3        0x04cu
#define SDC_BLK_NUM      0x050u
#define SDC_ADV_CFG0     0x064u
#define MSDC_PATCH_BIT0  0x0b0u
#define MSDC_PATCH_BIT1  0x0b4u
#define MSDC_PATCH_BIT2  0x0b8u
#define EMMC50_CFG0      0x208u
#define SDC_FIFO_CFG     0x228u

#define TOP_CTRL         0x000u
#define TOP_CMD          0x004u

#define MSDC_CFG_MODE          BIT0
#define MSDC_CFG_CCKPD         BIT1
#define MSDC_CFG_RST           BIT2
#define MSDC_CFG_PIO           BIT3
#define MSDC_CFG_CCKSB         BIT7
#define MSDC_CFG_HS400CKMD     BIT22
#define MSDC_CFG_CKMD_SHIFT    20u
#define MSDC_CFG_CKDIV_SHIFT   8u

#define MSDC_INT_CMDRDY        BIT8
#define MSDC_INT_CMDTMO        BIT9
#define MSDC_INT_CMDCRCERR     BIT10
#define MSDC_INT_DATTMO        BIT14
#define MSDC_INT_DATCRCERR     BIT15
#define MSDC_INT_XFER_COMPL    BIT12
#define MSDC_INT_ACMDRDY       BIT3
#define MSDC_INT_ACMDTMO       BIT4
#define MSDC_INT_ACMDCRCERR    BIT5

#define MSDC_INT_CMDSTS (MSDC_INT_CMDRDY | MSDC_INT_CMDTMO | \
                         MSDC_INT_CMDCRCERR | MSDC_INT_ACMDRDY | \
                         MSDC_INT_ACMDTMO | MSDC_INT_ACMDCRCERR)
#define MSDC_INT_DATSTS (MSDC_INT_XFER_COMPL | MSDC_INT_DATTMO | MSDC_INT_DATCRCERR)

#define MSDC_FIFOCS_CLR        BIT31
#define MSDC_FIFO_RXBYTES_MASK 0xffu
#define MSDC_FIFO_TXBYTES_SHIFT 16u
#define MSDC_FIFO_TXBYTES_MASK (0xffu << MSDC_FIFO_TXBYTES_SHIFT)

#define SDC_CFG_BUS_WIDTH_SHIFT 16u
#define SDC_CFG_SDIO            BIT19
#define SDC_CFG_SDIOIDE         BIT20

#define SDC_CMD_RSP_TYPE_SHIFT  7u
#define SDC_CMD_SINGLE_BLK      BIT11
#define SDC_CMD_MULTIPLE_BLK    BIT12
#define SDC_CMD_RW              BIT13
#define SDC_CMD_STOP_CMD        BIT14
#define SDC_CMD_BLK_SIZE_SHIFT  16u

#define SDC_STS_BUSY            BIT0
#define SDC_STS_CMDBUSY         BIT1

#define SDC_RX_ENHANCE_EN       BIT20
#define EMMC50_CFG0_CRCSTSSEL   BIT4
#define MSDC_PB1_BUSYCHECKSEL   BIT7
#define MSDC_PB2_CFGRESP        BIT15
#define MSDC_PB2_CFGCRCSTS      BIT28

#define SDC_FIFO_CFG_WRVALIDSEL BIT24
#define SDC_FIFO_CFG_RDVALIDSEL BIT25

#define PAD_DAT_RD_RXDLY_SEL    BIT13
#define DATA_K_VALUE_SEL        BIT14
#define SDC_RX_ENH_EN           BIT15
#define PAD_CMD_RD_RXDLY_SEL    BIT11

#define MSDC_BUS_WIDTH_1 0u
#define MSDC_BUS_WIDTH_4 1u
#define MSDC_BUS_WIDTH_8 2u

#define MSDC_MCLK_DIV    0u
#define MSDC_MCLK_NO_DIV 1u

#define SCLK_CYCLES_SHIFT 20u

#define MT6771_CMD_WAIT_TIMEOUT_US 2000000u
#define MT6771_DAT_WAIT_TIMEOUT_US 5000000u
#define MT6771_BUSY_WAIT_TIMEOUT_US 2000000u

typedef struct {
  UINTN   MsdcBase;
  UINTN   TopBase;
  BOOLEAN UseTop;
  UINT32  SourceClockHz;
  UINT32  SclkHz;
  UINT32  BusWidth;
  UINT32  PendingBlocks;
  UINT32  LastCommand;
  UINT32  Response[4];
} MT6771_MMC_HOST_DATA;

extern EFI_GUID gEmbeddedMmcHostProtocolGuid;

//
// BuildDevicePath() emits a vendor hardware device path. The GUID is local to
// this driver because no upstream eMMC hardware DP GUID exists at the pinned
// EDK2 commit.
//
#endif // MT6771_MMC_HOST_DXE_H_
