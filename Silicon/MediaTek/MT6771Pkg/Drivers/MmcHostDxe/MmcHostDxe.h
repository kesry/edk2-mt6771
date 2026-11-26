/** @file

  MT6771 MSDC/eMMC host controller DXE.

  This driver installs EFI_MMC_HOST_PROTOCOL so that EmbeddedPkg MmcDxe can
  enumerate the OPPO A3 eMMC and publish BlockIo.

  Register definitions and low level sequencing are derived from:
    - linux-mediatek/edk2-mtk next-msdc:
        Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.c
        Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.h
        (GPL-2.0-only, Arseniy Velikanov)
    - Linux v6.12 drivers/mmc/host/mtk-sd.c, mt6779_compat

  Hardware values for OPPO A3 / MT6771:
    - MSDC0        : 0x11230000, eMMC, 8-bit, non-removable
    - MSDC0_TOP    : 0x11f50000
    - IRQ          : GIC SPI 77 (unused: PIO polling only)
    - clocks/pins  : already configured by LK before UEFI is loaded

  Copyright (c) 2026, kesry. All rights reserved.
  SPDX-License-Identifier: GPL-2.0-only

**/

#ifndef __MT6771_MMC_HOST_DXE_H__
#define __MT6771_MMC_HOST_DXE_H__

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/BaseMemoryLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/MmcHost.h>

//
// Default CPU-visible MMIO bases (verified from OPPO A3 DTB/mainline patches).
//
#define MT6771_MSDC0_BASE         0x11230000u
#define MT6771_MSDC0_TOP_BASE     0x11f50000u

//
// Conservative card clock for first bring-up. LK leaves MSDC clocked from
// msdcpll (~400 MHz) with a divider; we do not reconfigure PLLs here.
//
#define MT6771_MSDC_SCLK_HZ       26000000u
#define MT6771_MSDC_TIMEOUT_NS    100000000u
#define MT6771_MSDC_FIFO_SIZE     128u

//
// MSDC registers
//
#define MSDC_CFG                  0x000u
#define MSDC_IOCON                0x004u
#define MSDC_PS                   0x008u
#define MSDC_INT                  0x00cu
#define MSDC_INTEN                0x010u
#define MSDC_FIFOCS               0x014u
#define MSDC_TXDATA               0x018u
#define MSDC_RXDATA               0x01cu
#define SDC_CFG                   0x030u
#define SDC_CMD                   0x034u
#define SDC_ARG                   0x038u
#define SDC_STS                   0x03cu
#define SDC_RESP0                 0x040u
#define SDC_RESP1                 0x044u
#define SDC_RESP2                 0x048u
#define SDC_RESP3                 0x04cu
#define SDC_BLK_NUM               0x050u
#define SDC_CSTS                  0x058u
#define SDC_CSTS_EN               0x05cu
#define SDC_DATCRC_STS            0x060u
#define SDC_ADV_CFG0              0x064u
#define EMMC_CFG0                 0x070u
#define EMMC_CFG1                 0x074u
#define EMMC_STS                  0x078u
#define EMMC_IOCON                0x07cu
#define DMA_SA                    0x090u
#define DMA_CTRL                  0x098u
#define DMA_LENGTH                0x0a8u
#define MSDC_PATCH_BIT0           0x0b0u
#define MSDC_PATCH_BIT1           0x0b4u
#define MSDC_PATCH_BIT2           0x0b8u
#define EMMC51_CFG0               0x204u
#define EMMC50_CFG0               0x208u
#define EMMC50_CFG1               0x20cu
#define SDC_FIFO_CFG              0x228u

//
// MSDC_TOP registers
//
#define TOP_CTRL                  0x00u
#define TOP_CMD                   0x04u

//
// MSDC_CFG bits
//
#define MSDC_CFG_MODE             BIT0
#define MSDC_CFG_CKPDN            BIT1
#define MSDC_CFG_RST              BIT2
#define MSDC_CFG_PIO              BIT3
#define MSDC_CFG_CKSTB            BIT7
#define MSDC_CFG_CKDIV            (0xfffu << 8)
#define MSDC_CFG_CKMOD            (0x3u << 20)
#define MSDC_CFG_HS400_CK_MODE_EXTRA BIT22

//
// MSDC_INT bits
//
#define MSDC_INT_CMDRDY           BIT8
#define MSDC_INT_CMDTMO           BIT9
#define MSDC_INT_CMDCRCERR        BIT10
#define MSDC_INT_XFER_COMPL       BIT12
#define MSDC_INT_DATTMO           BIT14
#define MSDC_INT_DATCRCERR        BIT15

//
// MSDC_FIFOCS bits
//
#define MSDC_FIFOCS_CLR           BIT31
#define MSDC_FIFOCS_RXCNT         0x000000ffu
#define MSDC_FIFOCS_TXCNT         (0x000000ffu << 16)

//
// SDC_CFG bits
//
#define SDC_CFG_BUS_WIDTH_SHIFT   16
#define SDC_CFG_BUS_WIDTH_1BIT    0u
#define SDC_CFG_BUS_WIDTH_4BIT    1u
#define SDC_CFG_BUS_WIDTH_8BIT    2u
#define SDC_CFG_SDIO              BIT19
#define SDC_CFG_SDIOIDE           BIT20

//
// SDC_CMD bits
//
#define SDC_CMD_RSP_TYPE_SHIFT    7
#define SDC_CMD_SINGLE_BLK        BIT11
#define SDC_CMD_MULTIPLE_BLK      BIT12
#define SDC_CMD_RW                BIT13
#define SDC_CMD_STOP_CMD          BIT14
#define SDC_CMD_BLK_SIZE_SHIFT    16

//
// SDC_STS bits
//
#define SDC_STS_CMDBUSY           BIT1

//
// Patch bits (MT6779-compatible, cross-checked with Linux mtk-sd.c)
//
#define MSDC_PB1_BUSYCHECKSEL     BIT7
#define MSDC_PB2_CFGRESP          BIT15
#define MSDC_PB2_CFGCRCSTS        BIT28
#define SDC_FIFO_CFG_WRVALIDSEL   BIT24
#define SDC_FIFO_CFG_RDVALIDSEL   BIT25
#define TOP_CTRL_SDC_RX_ENH_EN    BIT15

//
// Response types encoded in SDC_CMD (matches next-msdc).
//
#define MMC_RSP_NONE              0u
#define MMC_RSP_R1                1u
#define MMC_RSP_R2                2u
#define MMC_RSP_R3                3u
#define MMC_RSP_R4                4u
#define MMC_RSP_R1B               7u

typedef struct {
  UINTN   Base;
  UINTN   TopBase;
  UINT32  Sclk;
  BOOLEAN InitializationDone;
} MT6771_MMC_HOST;

extern EFI_MMC_HOST_PROTOCOL gMt6771MmcHost;

#endif /* __MT6771_MMC_HOST_DXE_H__ */
