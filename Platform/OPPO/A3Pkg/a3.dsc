[Defines]
  PLATFORM_NAME                  = a3
  PLATFORM_GUID                  = 6F5C4E7B-3E9A-4A2B-8A3E-4B8B05D1A3F7
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = A3Pkg/a3.fdf
  DEVICE_DXE_FV_COMPONENTS       = A3Pkg/a3.fdf.inc

[PcdsFixedAtBuild.common]
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x100000000 # 4GB
  gArmTokenSpaceGuid.PcdCpuVectorBaseAddress|0x40C40000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackBase|0x40C00000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackSize|0x00040000
  gMediaTekTokenSpaceGuid.PcdUefiMemPoolBase|0x40C50000
  gMediaTekTokenSpaceGuid.PcdUefiMemPoolSize|0x075b0000

  # LK-injected simple-framebuffer (decoded from vendor/a3-v186-tgpio.dtb):
  #   base 0x77440000, 16 MiB, 1080x2280, stride 0x1100 (4352 B = 1088 px),
  #   format a8r8g8b8.
  # SimpleFbDxe computes pitch as Width * 4, so Width must be 1088 to match
  # LK's stride; 1080 would skew every row by 32 bytes.
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferWidth|1088
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferHeight|2280
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferAddress|0x77440000

[LibraryClasses]
  PlatformMemoryMapLib|A3Pkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.inf
  NonDiscoverableDeviceRegistrationLib|MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.inf

[Components.common]
  MediaTekPkg/Drivers/Mt6771UsbAcmDxe/Mt6771UsbAcmDxe.inf
  MediaTekPkg/Drivers/Mt6771VbusDxe/Mt6771VbusDxe.inf
  MediaTekPkg/Drivers/Mt6771UsbHostDxe/Mt6771UsbHostDxe.inf
  MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf
  MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf
  MediaTekPkg/Drivers/Mt6771MmcHostDxe/Mt6771MmcHostDxe.inf
  MediaTekPkg/Drivers/Mt6771MmcDiagDxe/Mt6771MmcDiagDxe.inf
  MediaTekPkg/Drivers/A3UsbProbeDxe/A3UsbProbeDxe.inf
  MediaTekPkg/Drivers/A3UsbKbDxe/A3UsbKbDxe.inf
  MediaTekPkg/Applications/A3EmmcInfo/A3EmmcInfo.inf
  EmbeddedPkg/Universal/MmcDxe/MmcDxe.inf

!include Silicon/MediaTek/MT8183Pkg/MT8183.dsc
