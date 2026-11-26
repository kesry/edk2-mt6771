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

  # Simple Framebuffer
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferWidth|1080
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferHeight|2280
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferAddress|0x7d170000

[LibraryClasses]
  PlatformMemoryMapLib|A3Pkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.inf

!include Silicon/MediaTek/MT8183Pkg/MT8183.dsc
