/** @file
  Main file for support of shell consist mapping.

  Copyright (c) 2005 - 2014, Intel Corporation. All rights reserved.<BR>
  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution. The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include "UefiShellCommandLib.h"
#include <Library/DevicePathLib.h>
#include <Library/SortLib.h>
#include <Library/UefiLib.h>
#include <Protocol/UsbIo.h>

typedef enum {
  MTDTypeUnknown,
  MTDTypeFloppy,
  MTDTypeHardDisk,
  MTDTypeCDRom,
  MTDTypeEnd
} MTD_TYPE;

typedef struct {
  CHAR16  *Str;
  UINTN   Len;
} POOL_PRINT;

typedef struct {
  UINTN       Hi;
  MTD_TYPE    Mtd;
  POOL_PRINT  Csd;
  BOOLEAN     Digital;
} DEVICE_CONSIST_MAPPING_INFO;

typedef struct {
  MTD_TYPE  MTDType;
  CHAR16    *Name;
} MTD_NAME;

/**
  Serial Decode function.

  @param  DevPath          The Device path info.
  @param  MapInfo          The map info.
  @param  OrigDevPath      The original device path protocol.

**/
typedef 
VOID 
(EFIAPI *SERIAL_DECODE_FUNCTION) (
  EFI_DEVICE_PATH_PROTOCOL    *DevPath, 
  DEVICE_CONSIST_MAPPING_INFO *MapInfo,
  EFI_DEVICE_PATH_PROTOCOL    *OrigDevPath
  );

typedef struct {
  UINT8 Type;
  UINT8 SubType;
  SERIAL_DECODE_FUNCTION SerialFun;
  INTN (EFIAPI *CompareFun) (EFI_DEVICE_PATH_PROTOCOL *DevPath, EFI_DEVICE_PATH_PROTOCOL *DevPath2);
} DEV_PATH_CONSIST_MAPPING_TABLE;


/**
  Concatenates a formatted unicode string to allocated pool.
  The caller must free the resulting buffer.

  @param  Str      Tracks the allocated pool, size in use, and amount of pool allocated.
  @param  Fmt      The format string
  @param  ...      The data will be printed.

  @return Allocated buffer with the formatted string printed in it.
          The caller must free the allocated buffer.
          The buffer allocation is not packed.

**/
CHAR16 *
EFIAPI
CatPrint (
  IN OUT POOL_PRINT   *Str,
  IN CHAR16           *Fmt,
  ...
  )
{
  UINT16  *AppendStr;
  VA_LIST Args;
  UINTN   StringSize;

  AppendStr = AllocateZeroPool (0x1000);
  if (AppendStr == NULL) {
    ASSERT(FALSE);
    return Str->Str;
  }

  VA_START (Args, Fmt);
  UnicodeVSPrint (AppendStr, 0x1000, Fmt, Args);
  VA_END (Args);
  if (NULL == Str->Str) {
    StringSize   = StrSize (AppendStr);
    Str->Str  = AllocateZeroPool (StringSize);
    ASSERT (Str->Str != NULL);
  } else {
    StringSize = StrSize (AppendStr);
    StringSize += (StrSize (Str->Str) - sizeof (UINT16));

    Str->Str = ReallocatePool (
                StrSize (Str->Str),
                StringSize,
                Str->Str
               );
    ASSERT (Str->Str != NULL);
  }

  StrnCat (Str->Str, AppendStr, StringSize/sizeof(CHAR16) - 1 - StrLen(Str->Str));
  Str->Len = StringSize;

  FreePool (AppendStr);
  return Str->Str;
}

MTD_NAME  mMTDName[] = {
  {
    MTDTypeUnknown,
    L"F"
  },
  {
    MTDTypeFloppy,
    L"FP"
  },
  {
    MTDTypeHardDisk,
    L"HD"
  },
  {
    MTDTypeCDRom,
    L"CD"
  },
  {
    MTDTypeEnd,
    NULL
  }
};

/**
  Function to append a 64 bit number / 25 onto the string.

  @param[in, out] Str          The string so append onto.
  @param[in]      Num          The number to divide and append.

  @retval EFI_INVALID_PARAMETER   A parameter was NULL.
  @retval EFI_SUCCESS             The appending was successful.
**/
EFI_STATUS
EFIAPI
AppendCSDNum2 (
  IN OUT POOL_PRINT       *Str,
  IN UINT64               Num
  )
{
  UINT64  Result;
  UINT32   Rem;

  if (Str == NULL) {
    return (EFI_INVALID_PARAMETER);
  }

  Result = DivU64x32Remainder (Num, 25, &Rem);
  if (Result > 0) {
    AppendCSDNum2 (Str, Result);
  }

  CatPrint (Str, L"%c", Rem + 'a');
  return (EFI_SUCCESS);
}

/**
  Function to append a 64 bit number onto the mapping info.

  @param[in, out] MappingItem  The mapping info object to append onto.
  @param[in]      Num          The info to append.

  @retval EFI_INVALID_PARAMETER   A parameter was NULL.
  @retval EFI_SUCCESS             The appending was successful.
**/
EFI_STATUS
EFIAPI
AppendCSDNum (
  IN OUT DEVICE_CONSIST_MAPPING_INFO            *MappingItem,
  IN     UINT64                                 Num
  )
{
  if (MappingItem == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MappingItem->Digital) {
    CatPrint (&MappingItem->Csd, L"%ld", Num);
  } else {
    AppendCSDNum2 (&MappingItem->Csd, Num);
  }

  MappingItem->Digital = (BOOLEAN)!(MappingItem->Digital);

  return (EFI_SUCCESS);
}

/**
  Function to append string into the mapping info.

  @param[in, out] MappingItem  The mapping info object to append onto.
  @param[in]      Str          The info to append.

  @retval EFI_INVALID_PARAMETER   A parameter was NULL.
  @retval EFI_SUCCESS             The appending was successful.
**/
EFI_STATUS
EFIAPI
AppendCSDStr (
  IN OUT DEVICE_CONSIST_MAPPING_INFO            *MappingItem,
  IN     CHAR16                                 *Str
  )
{
  CHAR16  *Index;

  if (Str == NULL || MappingItem == NULL) {
    return (EFI_INVALID_PARAMETER);
  }

  if (MappingItem->Digital) {
    //
    // To aVOID mult-meaning, the mapping is:
    //  0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
    //  0  16 2  3  4  5  6  7  8  9  10 11 12 13 14 15
    //
    for (Index = Str; *Index != 0; Index++) {
      switch (*Index) {
      case '0':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        CatPrint (&MappingItem->Csd, L"%c", *Index);
        break;

      case '1':
        CatPrint (&MappingItem->Csd, L"16");
        break;

      case 'a':
      case 'b':
      case 'c':
      case 'd':
      case 'e':
      case 'f':
        CatPrint (&MappingItem->Csd, L"1%c", *Index - 'a' + '0');
        break;

      case 'A':
      case 'B':
      case 'C':
      case 'D':
      case 'E':
      case 'F':
        CatPrint (&MappingItem->Csd, L"1%c", *Index - 'A' + '0');
        break;
      }
    }
  } else {
    for (Index = Str; *Index != 0; Index++) {
      //
      //  The mapping is:
      //  0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
      //  a  b  c  d  e  f  g  h  i  j  k  l  m  n  o  p
      //
      if (*Index >= '0' && *Index <= '9') {
        CatPrint (&MappingItem->Csd, L"%c", *Index - '0' + 'a');
      } else if (*Index >= 'a' && *Index <= 'f') {
        CatPrint (&MappingItem->Csd, L"%c", *Index - 'a' + 'k');
      } else if (*Index >= 'A' && *Index <= 'F') {
        CatPrint (&MappingItem->Csd, L"%c", *Index - 'A' + 'k');
      }
    }
  }

  MappingItem->Digital = (BOOLEAN)!(MappingItem->Digital);

  return (EFI_SUCCESS);
}

/**
  Function to append a Guid to the mapping item.

  @param[in, out] MappingItem  The item to append onto.
  @param[in]      Guid         The guid to append.

  @retval EFI_SUCCESS           The appending operation was successful.
  @retval EFI_INVALID_PARAMETER A parameter was NULL.
**/
EFI_STATUS
EFIAPI
AppendCSDGuid (
  DEVICE_CONSIST_MAPPING_INFO            *MappingItem,
  EFI_GUID                               *Guid
  )
{
  CHAR16  Buffer[64];

  if (Guid == NULL || MappingItem == NULL) {
    return (EFI_INVALID_PARAMETER);
  }

  UnicodeSPrint (
    Buffer,
    0,
    L"%g",
    Guid
   );

  AppendCSDStr (MappingItem, Buffer);

  return (EFI_SUCCESS);
}

/**
  Function to compare 2 APCI device paths.

  @param[in] DevicePath1        The first device path to compare.
  @param[in] DevicePath2        The second device path to compare.

  @retval 0 The device paths represent the same device.
  @return   Non zero if the devices are different, zero otherwise.
**/
INTN
EFIAPI
DevPathCompareAcpi (
  IN EFI_DEVICE_PATH_PROTOCOL       *DevicePath1,
  IN EFI_DEVICE_PATH_PROTOCOL       *DevicePath2
  )
{
  ACPI_HID_DEVICE_PATH  *Acpi1;
  ACPI_HID_DEVICE_PATH  *Acpi2;

  if (DevicePath1 == NULL || DevicePath2 == NULL) {
    return (-2);
  }

  Acpi1 = (ACPI_HID_DEVICE_PATH *) DevicePath1;
  Acpi2 = (ACPI_HID_DEVICE_PATH *) DevicePath2;
  if (Acpi1->HID > Acpi2->HID || (Acpi1->HID == Acpi2->HID && Acpi1->UID > Acpi2->UID)) {
    return 1;
  }

  if (Acpi1->HID == Acpi2->HID && Acpi1->UID == Acpi2->UID) {
    return 0;
  }

  return -1;
}

/**
  Function to compare 2 PCI device paths.

  @param[in] DevicePath1        The first device path to compare.
  @param[in] DevicePath2        The second device path to compare.

  @retval 0 The device paths represent the same device.
  @return   Non zero if the devices are different, zero otherwise.
**/
INTN
EFIAPI
DevPathComparePci (
  IN EFI_DEVICE_PATH_PROTOCOL       *DevicePath1,
  IN EFI_DEVICE_PATH_PROTOCOL       *DevicePath2
  )
{
  PCI_DEVICE_PATH *Pci1;
  PCI_DEVICE_PATH *Pci2;

  ASSERT(DevicePath1 != NULL);
  ASSERT(DevicePath2 != NULL);

  Pci1  = (PCI_DEVICE_PATH *) DevicePath1;
  Pci2  = (PCI_DEVICE_PATH *) DevicePath2;
  if (Pci1->Device > Pci2->Device || (Pci1->Device == Pci2->Device && Pci1->Function > Pci2->Function)) {
    return 1;
  }

  if (Pci1->Device == Pci2->Device && Pci1->Function == Pci2->Function) {
    return 0;
  }

  return -1;
}

/**
  Do a comparison on 2 device paths.

  @param[in] DevicePath1   The first device path.
  @param[in] DevicePath2   The second device path.

  @retval 0 The 2 device paths are the same.
  @retval <0  DevicePath2 is greater than DevicePath1.
  @retval >0  DevicePath1 is greater than DevicePath2.
**/
INTN
EFIAPI
DevPathCompareDefault (
  IN EFI_DEVICE_PATH_PROTOCOL       *DevicePath1,
  IN EFI_DEVICE_PATH_PROTOCOL       *DevicePath2
  )
{
  UINTN DevPathSize1;
  UINTN DevPathSize2;

  ASSERT(DevicePath1 != NULL);
  ASSERT(DevicePath2 != NULL);

  DevPathSize1  = DevicePathNodeLength (DevicePath1);
  DevPathSize2  = DevicePathNodeLength (DevicePath2);
  if (DevPathSize1 > DevPathSize2) {
    return 1;
  } else if (DevPathSize1 < DevPathSize2) {
    return -1;
  } else {
    return CompareMem (DevicePath1, DevicePath2, DevPathSize1);
  }
}

/**
  DevicePathNode must be SerialHDD Channel type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.
**/
VOID
EFIAPI
DevPathSerialHardDrive (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  HARDDRIVE_DEVICE_PATH *Hd;

  ASSERT(DevicePathNode != NULL);
  ASSERT(MappingItem != NULL);

  Hd = (HARDDRIVE_DEVICE_PATH *) DevicePathNode;
  if (MappingItem->Mtd == MTDTypeUnknown) {
    MappingItem->Mtd = MTDTypeHardDisk;
  }

  AppendCSDNum (MappingItem, Hd->PartitionNumber);
}

/**
  DevicePathNode must be SerialAtapi Channel type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.
**/
VOID
EFIAPI
DevPathSerialAtapi (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  ATAPI_DEVICE_PATH *Atapi;

  ASSERT(DevicePathNode != NULL);
  ASSERT(MappingItem != NULL);

  Atapi = (ATAPI_DEVICE_PATH *) DevicePathNode;
  AppendCSDNum (MappingItem, (Atapi->PrimarySecondary * 2 + Atapi->SlaveMaster));
}

/**
  DevicePathNode must be SerialCDROM Channel type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.
**/
VOID
EFIAPI
DevPathSerialCdRom (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  CDROM_DEVICE_PATH *Cd;

  ASSERT(DevicePathNode != NULL);
  ASSERT(MappingItem != NULL);

  Cd                = (CDROM_DEVICE_PATH *) DevicePathNode;
  MappingItem->Mtd  = MTDTypeCDRom;
  AppendCSDNum (MappingItem, Cd->BootEntry);
}

/**
  DevicePathNode must be SerialFibre Channel type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.
**/
VOID
EFIAPI
DevPathSerialFibre (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  FIBRECHANNEL_DEVICE_PATH  *Fibre;

  ASSERT(DevicePathNode != NULL);
  ASSERT(MappingItem != NULL);

  Fibre = (FIBRECHANNEL_DEVICE_PATH *) DevicePathNode;
  AppendCSDNum (MappingItem, Fibre->WWN);
  AppendCSDNum (MappingItem, Fibre->Lun);
}

/**
  DevicePathNode must be SerialUart type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.
**/
VOID
EFIAPI
DevPathSerialUart (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  UART_DEVICE_PATH  *Uart;

  ASSERT(DevicePathNode != NULL);
  ASSERT(MappingItem != NULL);

  Uart = (UART_DEVICE_PATH *) DevicePathNode;
  AppendCSDNum (MappingItem, Uart->BaudRate);
  AppendCSDNum (MappingItem, Uart->DataBits);
  AppendCSDNum (MappingItem, Uart->Parity);
  AppendCSDNum (MappingItem, Uart->StopBits);
}

/**
  DevicePathNode must be SerialUSB type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.
**/
VOID
EFIAPI
DevPathSerialUsb (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  USB_DEVICE_PATH           *Usb;
  EFI_USB_IO_PROTOCOL       *UsbIo;
  EFI_HANDLE                TempHandle;
  EFI_STATUS                Status;
  USB_INTERFACE_DESCRIPTOR  InterfaceDesc;


//  ASSERT(DevicePathNode != NULL);
//  ASSERT(MappingItem != NULL);
  if (!DevicePathNode || !MappingItem) return;

  Usb = (USB_DEVICE_PATH *) DevicePathNode;
  AppendCSDNum (MappingItem, Usb->ParentPortNumber);
  AppendCSDNum (MappingItem, Usb->InterfaceNumber);

  if (PcdGetBool(PcdUsbExtendedDecode)) {
    Status = gBS->LocateDevicePath( &gEfiUsbIoProtocolGuid, &DevicePath, &TempHandle );
    UsbIo = NULL;
    if (!EFI_ERROR(Status)) {
      Status = gBS->OpenProtocol(TempHandle, &gEfiUsbIoProtocolGuid, (VOID**)&UsbIo, gImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    } 

    if (!EFI_ERROR(Status)) {
      ASSERT(UsbIo != NULL);
      Status = UsbIo->UsbGetInterfaceDescriptor(UsbIo, &InterfaceDesc);
      if (!EFI_ERROR(Status)) {
        if (InterfaceDesc.InterfaceClass == USB_MASS_STORE_CLASS && MappingItem->Mtd == MTDTypeUnknown) {
          switch (InterfaceDesc.InterfaceSubClass){
            case USB_MASS_STORE_SCSI:
              MappingItem->Mtd = MTDTypeHardDisk;
              break;
            case USB_MASS_STORE_8070I:
            case USB_MASS_STORE_UFI:
              MappingItem->Mtd = MTDTypeFloppy;
              break;
            case USB_MASS_STORE_8020I:
              MappingItem->Mtd  = MTDTypeCDRom;
              break;
          }
        }
      }
    } 
  }
}

/**
  DevicePathNode must be SerialVendor type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.

**/
VOID
EFIAPI
DevPathSerialVendor (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  VENDOR_DEVICE_PATH  *Vendor;
  SAS_DEVICE_PATH     *Sas;
  UINTN               TargetNameLength;
  UINTN               Index;
  CHAR16              *Buffer;

  if (DevicePathNode == NULL || MappingItem == NULL) {
    return;
  }

  Vendor = (VENDOR_DEVICE_PATH *) DevicePathNode;
  AppendCSDGuid (MappingItem, &Vendor->Guid);

  if (CompareGuid (&gEfiSasDevicePathGuid, &Vendor->Guid)) {
    Sas = (SAS_DEVICE_PATH *) Vendor;
    AppendCSDNum (MappingItem, Sas->SasAddress);
    AppendCSDNum (MappingItem, Sas->Lun);
    AppendCSDNum (MappingItem, Sas->DeviceTopology);
    AppendCSDNum (MappingItem, Sas->RelativeTargetPort);
  } else {
    TargetNameLength = MIN(DevicePathNodeLength (DevicePathNode) - sizeof (VENDOR_DEVICE_PATH), PcdGet32(PcdShellVendorExtendedDecode));
    if (TargetNameLength != 0) {
      //
      // String is 2 chars per data byte, plus NULL terminator
      //
      Buffer = AllocateZeroPool (((TargetNameLength * 2) + 1) * sizeof(CHAR16));
      ASSERT(Buffer != NULL);
      if (Buffer == NULL) {
        return;
  }

      //
      // Build the string data
      //
      for (Index = 0; Index < TargetNameLength; Index++) {
        Buffer = CatSPrint (Buffer, L"%02x", *((UINT8*)Vendor + sizeof (VENDOR_DEVICE_PATH) + Index));
}

      //
      // Append the new data block
      //
      AppendCSDStr (MappingItem, Buffer);

      FreePool(Buffer);
    }
  }
}

/**
  DevicePathNode must be SerialLun type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.
**/
VOID
EFIAPI
DevPathSerialLun (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  DEVICE_LOGICAL_UNIT_DEVICE_PATH *Lun;

  ASSERT(DevicePathNode != NULL);
  ASSERT(MappingItem != NULL);

  Lun = (DEVICE_LOGICAL_UNIT_DEVICE_PATH *) DevicePathNode;
  AppendCSDNum (MappingItem, Lun->Lun);
}

/**
  DevicePathNode must be SerialSata type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.
**/
VOID
EFIAPI
DevPathSerialSata (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  SATA_DEVICE_PATH  *Sata;

  ASSERT(DevicePathNode != NULL);
  ASSERT(MappingItem != NULL);

  Sata = (SATA_DEVICE_PATH  *) DevicePathNode;
  AppendCSDNum (MappingItem, Sata->HBAPortNumber);
  AppendCSDNum (MappingItem, Sata->PortMultiplierPortNumber);
  AppendCSDNum (MappingItem, Sata->Lun);
}

/**
  DevicePathNode must be SerialSCSI type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.
**/
VOID
EFIAPI
DevPathSerialIScsi (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  ISCSI_DEVICE_PATH  *IScsi;
  UINT8              *IScsiTargetName;
  CHAR16             *TargetName;
  UINTN              TargetNameLength;
  UINTN              Index;

  ASSERT(DevicePathNode != NULL);
  ASSERT(MappingItem != NULL);

  if (PcdGetBool(PcdShellDecodeIScsiMapNames)) {
    IScsi = (ISCSI_DEVICE_PATH  *) DevicePathNode;
    AppendCSDNum (MappingItem, IScsi->NetworkProtocol);
    AppendCSDNum (MappingItem, IScsi->LoginOption);
    AppendCSDNum (MappingItem, IScsi->Lun);
    AppendCSDNum (MappingItem, IScsi->TargetPortalGroupTag);
    TargetNameLength = DevicePathNodeLength (DevicePathNode) - sizeof (ISCSI_DEVICE_PATH);
    if (TargetNameLength > 0) {
      TargetName = AllocateZeroPool ((TargetNameLength + 1) * sizeof (CHAR16));
      if (TargetName != NULL) {
        IScsiTargetName = (UINT8 *) (IScsi + 1);
        for (Index = 0; Index < TargetNameLength; Index++) {
          TargetName[Index] = (CHAR16) IScsiTargetName[Index];
        }
        AppendCSDStr (MappingItem, TargetName);
        FreePool (TargetName);
      }
    }
  }
}

/**
  DevicePathNode must be SerialI20 type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.
**/
VOID
EFIAPI
DevPathSerialI2O (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  I2O_DEVICE_PATH *DevicePath_I20;

  ASSERT(DevicePathNode != NULL);
  ASSERT(MappingItem != NULL);

  DevicePath_I20 = (I2O_DEVICE_PATH *) DevicePathNode;
  AppendCSDNum (MappingItem, DevicePath_I20->Tid);
}

/**
  DevicePathNode must be Mac Address type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.
**/
VOID
EFIAPI
DevPathSerialMacAddr (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  MAC_ADDR_DEVICE_PATH  *Mac;
  UINTN                 HwAddressSize;
  UINTN                 Index;
  CHAR16                Buffer[64];
  CHAR16                *PBuffer;

  ASSERT(DevicePathNode != NULL);
  ASSERT(MappingItem != NULL);

  Mac           = (MAC_ADDR_DEVICE_PATH *) DevicePathNode;

  HwAddressSize = sizeof (EFI_MAC_ADDRESS);
  if (Mac->IfType == 0x01 || Mac->IfType == 0x00) {
    HwAddressSize = 6;
  }

  for (Index = 0, PBuffer = Buffer; Index < HwAddressSize; Index++, PBuffer += 2) {
    UnicodeSPrint (PBuffer, 0, L"%02x", (UINTN) Mac->MacAddress.Addr[Index]);
  }

  AppendCSDStr (MappingItem, Buffer);
}

/**
  DevicePathNode must be InfiniBand type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.
**/
VOID
EFIAPI
DevPathSerialInfiniBand (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  INFINIBAND_DEVICE_PATH  *InfiniBand;
  UINTN                   Index;
  CHAR16                  Buffer[64];
  CHAR16                  *PBuffer;

  ASSERT(DevicePathNode != NULL);
  ASSERT(MappingItem != NULL);

  InfiniBand = (INFINIBAND_DEVICE_PATH *) DevicePathNode;
  for (Index = 0, PBuffer = Buffer; Index < 16; Index++, PBuffer += 2) {
    UnicodeSPrint (PBuffer, 0, L"%02x", (UINTN) InfiniBand->PortGid[Index]);
  }

  AppendCSDStr (MappingItem, Buffer);
  AppendCSDNum (MappingItem, InfiniBand->ServiceId);
  AppendCSDNum (MappingItem, InfiniBand->TargetPortId);
  AppendCSDNum (MappingItem, InfiniBand->DeviceId);
}

/**
  DevicePathNode must be IPv4 type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.
**/
VOID
EFIAPI
DevPathSerialIPv4 (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  IPv4_DEVICE_PATH  *Ip;
  CHAR16            Buffer[10];

//  ASSERT(DevicePathNode != NULL);
//  ASSERT(MappingItem != NULL);
  if (!DevicePathNode || !MappingItem) return;

  Ip = (IPv4_DEVICE_PATH *) DevicePathNode;
  UnicodeSPrint (
    Buffer,
    0,
    L"%02x%02x%02x%02x",
    (UINTN) Ip->LocalIpAddress.Addr[0],
    (UINTN) Ip->LocalIpAddress.Addr[1],
    (UINTN) Ip->LocalIpAddress.Addr[2],
    (UINTN) Ip->LocalIpAddress.Addr[3]
   );
  AppendCSDStr (MappingItem, Buffer);
  AppendCSDNum (MappingItem, Ip->LocalPort);
  UnicodeSPrint (
    Buffer,
    0,
    L"%02x%02x%02x%02x",
    (UINTN) Ip->RemoteIpAddress.Addr[0],
    (UINTN) Ip->RemoteIpAddress.Addr[1],
    (UINTN) Ip->RemoteIpAddress.Addr[2],
    (UINTN) Ip->RemoteIpAddress.Addr[3]
   );
  AppendCSDStr (MappingItem, Buffer);
  AppendCSDNum (MappingItem, Ip->RemotePort);
}

/**
  DevicePathNode must be IPv6 type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.

**/
VOID
EFIAPI
DevPathSerialIPv6 (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  IPv6_DEVICE_PATH  *Ip;
  UINTN             Index;
  CHAR16            Buffer[64];
  CHAR16            *PBuffer;

//  ASSERT(DevicePathNode != NULL);
//  ASSERT(MappingItem != NULL);
  if (!DevicePathNode || !MappingItem) return;

  Ip = (IPv6_DEVICE_PATH *) DevicePathNode;
  for (Index = 0, PBuffer = Buffer; Index < 16; Index++, PBuffer += 2) {
    UnicodeSPrint (PBuffer, 0, L"%02x", (UINTN) Ip->LocalIpAddress.Addr[Index]);
  }

  AppendCSDStr (MappingItem, Buffer);
  AppendCSDNum (MappingItem, Ip->LocalPort);
  for (Index = 0, PBuffer = Buffer; Index < 16; Index++, PBuffer += 2) {
    UnicodeSPrint (PBuffer, 0, L"%02x", (UINTN) Ip->RemoteIpAddress.Addr[Index]);
  }

  AppendCSDStr (MappingItem, Buffer);
  AppendCSDNum (MappingItem, Ip->RemotePort);
}

/**
  DevicePathNode must be SCSI type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.

**/
VOID
EFIAPI
DevPathSerialScsi (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  SCSI_DEVICE_PATH  *Scsi;

//  ASSERT(DevicePathNode != NULL);
//  ASSERT(MappingItem != NULL);
  if (!DevicePathNode || !MappingItem) return;

  Scsi = (SCSI_DEVICE_PATH *) DevicePathNode;
  AppendCSDNum (MappingItem, Scsi->Pun);
  AppendCSDNum (MappingItem, Scsi->Lun);
}

/**
  DevicePathNode must be 1394 type and this will populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.
**/
VOID
EFIAPI
DevPathSerial1394 (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  F1394_DEVICE_PATH *DevicePath_F1394;
  CHAR16            Buffer[20];

//  ASSERT(DevicePathNode != NULL);
//  ASSERT(MappingItem != NULL);
  if (!DevicePathNode || !MappingItem) return;

  DevicePath_F1394 = (F1394_DEVICE_PATH *) DevicePathNode;
  UnicodeSPrint (Buffer, 0, L"%lx", DevicePath_F1394->Guid);
  AppendCSDStr (MappingItem, Buffer);
}

/**
  If the node is floppy type then populate the MappingItem.

  @param[in] DevicePathNode   The node to get info on.
  @param[in] MappingItem      The info item to populate.
  @param[in] DevicePath       Ignored.
**/
VOID
EFIAPI
DevPathSerialAcpi (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  ACPI_HID_DEVICE_PATH  *Acpi;

//  ASSERT(DevicePathNode != NULL);
//  ASSERT(MappingItem != NULL);
  if (!DevicePathNode || !MappingItem) return;

  Acpi = (ACPI_HID_DEVICE_PATH *) DevicePathNode;
  if ((Acpi->HID & PNP_EISA_ID_MASK) == PNP_EISA_ID_CONST) {
    if (EISA_ID_TO_NUM (Acpi->HID) == 0x0604) {
      MappingItem->Mtd = MTDTypeFloppy;
      AppendCSDNum (MappingItem, Acpi->UID);
    }
  }
}

/**
  Empty function used for unknown devices.

  @param[in] DevicePathNode       Ignored.
  @param[in] MappingItem          Ignored.
  @param[in] DevicePath           Ignored.

  Does nothing.
**/
VOID
EFIAPI
DevPathSerialDefault (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePathNode,
  IN DEVICE_CONSIST_MAPPING_INFO  *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  return;
}

DEV_PATH_CONSIST_MAPPING_TABLE  DevPathConsistMappingTable[] = {
  {
    HARDWARE_DEVICE_PATH,
    HW_PCI_DP,
    DevPathSerialDefault,
    DevPathComparePci
  },
  {
    ACPI_DEVICE_PATH,
    ACPI_DP,
    DevPathSerialAcpi,
    DevPathCompareAcpi
  },
  {
    MESSAGING_DEVICE_PATH,
    MSG_ATAPI_DP,
    DevPathSerialAtapi,
    DevPathCompareDefault
  },
  {
    MESSAGING_DEVICE_PATH,
    MSG_SCSI_DP,
    DevPathSerialScsi,
    DevPathCompareDefault
  },
  {
    MESSAGING_DEVICE_PATH,
    MSG_FIBRECHANNEL_DP,
    DevPathSerialFibre,
    DevPathCompareDefault
  },
  {
    MESSAGING_DEVICE_PATH,
    MSG_1394_DP,
    DevPathSerial1394,
    DevPathCompareDefault
  },
  {
    MESSAGING_DEVICE_PATH,
    MSG_USB_DP,
    DevPathSerialUsb,
    DevPathCompareDefault
  },
  {
    MESSAGING_DEVICE_PATH,
    MSG_I2O_DP,
    DevPathSerialI2O,
    DevPathCompareDefault
  },
  {
    MESSAGING_DEVICE_PATH,
    MSG_MAC_ADDR_DP,
    DevPathSerialMacAddr,
    DevPathCompareDefault
  },
  {
    MESSAGING_DEVICE_PATH,
    MSG_IPv4_DP,
    DevPathSerialIPv4,
    DevPathCompareDefault
  },
  {
    MESSAGING_DEVICE_PATH,
    MSG_IPv6_DP,
    DevPathSerialIPv6,
    DevPathCompareDefault
  },
  {
    MESSAGING_DEVICE_PATH,
    MSG_INFINIBAND_DP,
    DevPathSerialInfiniBand,
    DevPathCompareDefault
  },
  {
    MESSAGING_DEVICE_PATH,
    MSG_UART_DP,
    DevPathSerialUart,
    DevPathCompareDefault
  },
  {
    MESSAGING_DEVICE_PATH,
    MSG_VENDOR_DP,
    DevPathSerialVendor,
    DevPathCompareDefault
  },
  {
    MESSAGING_DEVICE_PATH,
    MSG_DEVICE_LOGICAL_UNIT_DP,
    DevPathSerialLun,
    DevPathCompareDefault
  },
  {
    MESSAGING_DEVICE_PATH,
    MSG_SATA_DP,
    DevPathSerialSata,
    DevPathCompareDefault
  },
  {
    MESSAGING_DEVICE_PATH,
    MSG_ISCSI_DP,
    DevPathSerialIScsi,
    DevPathCompareDefault
  },
  {
    MEDIA_DEVICE_PATH,
    MEDIA_HARDDRIVE_DP,
    DevPathSerialHardDrive,
    DevPathCompareDefault
  },
  {
    MEDIA_DEVICE_PATH,
    MEDIA_CDROM_DP,
    DevPathSerialCdRom,
    DevPathCompareDefault
  },
  {
    MEDIA_DEVICE_PATH,
    MEDIA_VENDOR_DP,
    DevPathSerialVendor,
    DevPathCompareDefault
  },
  {
    0,
    0,
    NULL,
    NULL
  }
};

/**
  Function to determine if a device path node is Hi or not.

  @param[in] DevicePathNode   The node to check.

  @retval TRUE    The node is Hi.
  @retval FALSE   The node is not Hi.
**/
BOOLEAN
EFIAPI
IsHIDevicePathNode (
  IN EFI_DEVICE_PATH_PROTOCOL *DevicePathNode
  )
{
  ACPI_HID_DEVICE_PATH  *Acpi;

  ASSERT(DevicePathNode != NULL);

  if (DevicePathNode->Type == HARDWARE_DEVICE_PATH) {
    return TRUE;
  }

  if (DevicePathNode->Type == ACPI_DEVICE_PATH) {
    Acpi = (ACPI_HID_DEVICE_PATH *) DevicePathNode;
    switch (EISA_ID_TO_NUM (Acpi->HID)) {
    case 0x0301:
    case 0x0401:
    case 0x0501:
    case 0x0604:
      return FALSE;
    }

    return TRUE;
  }

  return FALSE;
}

/**
  Function to convert a standard device path structure into a Hi version.

  @param[in] DevicePath   The device path to convert.

  @return   the device path portion that is Hi.
**/
EFI_DEVICE_PATH_PROTOCOL *
EFIAPI
GetHIDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL        *DevicePath
  )
{
  UINTN                     NonHIDevicePathNodeCount;
  UINTN                     Index;
  EFI_DEV_PATH              Node;
  EFI_DEVICE_PATH_PROTOCOL  *HIDevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *TempDevicePath;

//  ASSERT(DevicePath != NULL);
  if (!DevicePath) {
    return NULL;
  }

  NonHIDevicePathNodeCount  = 0;

  HIDevicePath              = AllocateZeroPool (sizeof (EFI_DEVICE_PATH_PROTOCOL));
  SetDevicePathEndNode (HIDevicePath);

  Node.DevPath.Type       = END_DEVICE_PATH_TYPE;
  Node.DevPath.SubType    = END_INSTANCE_DEVICE_PATH_SUBTYPE;
  Node.DevPath.Length[0]  = (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL);
  Node.DevPath.Length[1]  = 0;

  while (!IsDevicePathEnd (DevicePath)) {
    if (IsHIDevicePathNode (DevicePath)) {
      for (Index = 0; Index < NonHIDevicePathNodeCount; Index++) {
        TempDevicePath = AppendDevicePathNode (HIDevicePath, &Node.DevPath);
        FreePool (HIDevicePath);
        HIDevicePath = TempDevicePath;
      }

      TempDevicePath = AppendDevicePathNode (HIDevicePath, DevicePath);
      FreePool (HIDevicePath);
      HIDevicePath = TempDevicePath;
    } else {
      NonHIDevicePathNodeCount++;
    }
    //
    // Next device path node
    //
    DevicePath = (EFI_DEVICE_PATH_PROTOCOL *) NextDevicePathNode (DevicePath);
  }

  return HIDevicePath;
}

/**
  Function to walk the device path looking for a dumpable node.

  @param[in] MappingItem      The Item to fill with data.
  @param[in] DevicePath       The path of the item to get data on.

  @return EFI_SUCCESS         Always returns success.
**/
EFI_STATUS
EFIAPI
GetDeviceConsistMappingInfo (
  IN DEVICE_CONSIST_MAPPING_INFO    *MappingItem,
  IN EFI_DEVICE_PATH_PROTOCOL       *DevicePath
  )
{
  SERIAL_DECODE_FUNCTION    SerialFun;
  UINTN                     Index;
  EFI_DEVICE_PATH_PROTOCOL  *OriginalDevicePath;

//  ASSERT(DevicePathNode != NULL);
//  ASSERT(MappingItem != NULL);
  if (!DevicePath || !MappingItem) return EFI_SUCCESS;

  SetMem (&MappingItem->Csd, sizeof (POOL_PRINT), 0);
  OriginalDevicePath = DevicePath;

  while (!IsDevicePathEnd (DevicePath)) {
    //
    // Find the handler to dump this device path node and
    // initialize with generic function in case nothing is found
    //
    for (SerialFun = DevPathSerialDefault, Index = 0; DevPathConsistMappingTable[Index].SerialFun != NULL; Index += 1) {

      if (DevicePathType (DevicePath) == DevPathConsistMappingTable[Index].Type &&
          DevicePathSubType (DevicePath) == DevPathConsistMappingTable[Index].SubType
         ) {
        SerialFun = DevPathConsistMappingTable[Index].SerialFun;
        break;
      }
    }

    SerialFun (DevicePath, MappingItem, OriginalDevicePath);

    //
    // Next device path node
    //
    DevicePath = (EFI_DEVICE_PATH_PROTOCOL *) NextDevicePathNode (DevicePath);
  }

  return EFI_SUCCESS;
}

/**
  Function to initialize the table for creating consistent map names.

  @param[out] Table             The pointer to pointer to pointer to DevicePathProtocol object.

  @retval EFI_SUCCESS           The table was created successfully.
**/
EFI_STATUS
EFIAPI
ShellCommandConsistMappingInitialize (
  OUT EFI_DEVICE_PATH_PROTOCOL           ***Table
  )
{
  EFI_HANDLE                *HandleBuffer;
  UINTN                     HandleNum;
  UINTN                     HandleLoop;
  EFI_DEVICE_PATH_PROTOCOL  **TempTable;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *HIDevicePath;
  UINTN                     Index;
  EFI_STATUS                Status;

  HandleBuffer              = NULL;

  Status = gBS->LocateHandleBuffer (
              AllHandles,
              NULL,
              NULL,
              &HandleNum,
              &HandleBuffer
             );
  ASSERT_EFI_ERROR(Status);

  TempTable     = AllocateZeroPool ((HandleNum + 1) * sizeof (EFI_DEVICE_PATH_PROTOCOL *));
  if (TempTable == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  for (HandleLoop = 0 ; HandleLoop < HandleNum ; HandleLoop++) {
    DevicePath = DevicePathFromHandle (HandleBuffer[HandleLoop]);
    if (DevicePath == NULL) {
      continue;
    }

    HIDevicePath = GetHIDevicePath (DevicePath);
    if (HIDevicePath == NULL) {
      continue;
    }

    for (Index = 0; TempTable[Index] != NULL; Index++) {
      if (DevicePathCompare (&TempTable[Index], &HIDevicePath) == 0) {
        FreePool (HIDevicePath);
        break;
      }
    }

    if (TempTable[Index] == NULL) {
      TempTable[Index] = HIDevicePath;
    }
  }

  for (Index = 0; TempTable[Index] != NULL; Index++);
  PerformQuickSort(TempTable, Index, sizeof(EFI_DEVICE_PATH_PROTOCOL*), DevicePathCompare);
  *Table = TempTable;

  if (HandleBuffer != NULL) {
    FreePool (HandleBuffer);
  }

  return EFI_SUCCESS;
}

/**
  Function to uninitialize the table for creating consistent map names.

  The parameter must have been received from ShellCommandConsistMappingInitialize.

  @param[out] Table             The pointer to pointer to DevicePathProtocol object.

  @retval EFI_SUCCESS           The table was deleted successfully.
**/
EFI_STATUS
EFIAPI
ShellCommandConsistMappingUnInitialize (
  EFI_DEVICE_PATH_PROTOCOL **Table
  )
{
  UINTN Index;

  ASSERT(Table  != NULL);

  for (Index = 0; Table[Index] != NULL; Index++) {
    FreePool (Table[Index]);
  }

  FreePool (Table);
  return EFI_SUCCESS;
}

/**
  Create a consistent mapped name for the device specified by DevicePath
  based on the Table.

  This must be called after ShellCommandConsistMappingInitialize() and
  before ShellCommandConsistMappingUnInitialize() is called.

  @param[in] DevicePath   The pointer to the dev path for the device.
  @param[in] Table        The Table of mapping information.

  @retval NULL            A consistent mapped name could not be created.
  @return                 A pointer to a string allocated from pool with the device name.
**/
CHAR16 *
EFIAPI
ShellCommandConsistMappingGenMappingName (
  IN EFI_DEVICE_PATH_PROTOCOL    *DevicePath,
  IN EFI_DEVICE_PATH_PROTOCOL    **Table
  )
{
  POOL_PRINT                  Str;
  DEVICE_CONSIST_MAPPING_INFO MappingInfo;
  EFI_DEVICE_PATH_PROTOCOL    *HIDevicePath;
  UINTN                       Index;
  UINTN                       NewSize;

  ASSERT(DevicePath         != NULL);
  ASSERT(Table  != NULL);

  HIDevicePath = GetHIDevicePath (DevicePath);
  if (HIDevicePath == NULL) {
    return NULL;
  }

  for (Index = 0; Table[Index] != NULL; Index++) {
    if (DevicePathCompare (&Table[Index], &HIDevicePath) == 0) {
      break;
    }
  }

  FreePool (HIDevicePath);
  if (Table[Index] == NULL) {
    return NULL;
  }

  MappingInfo.Hi      = Index;
  MappingInfo.Mtd     = MTDTypeUnknown;
  MappingInfo.Digital = FALSE;

  GetDeviceConsistMappingInfo (&MappingInfo, DevicePath);

  SetMem (&Str, sizeof (Str), 0);
  for (Index = 0; mMTDName[Index].MTDType != MTDTypeEnd; Index++) {
    if (MappingInfo.Mtd == mMTDName[Index].MTDType) {
      break;
    }
  }

  if (mMTDName[Index].MTDType != MTDTypeEnd) {
    CatPrint (&Str, L"%s", mMTDName[Index].Name);
  }

  CatPrint (&Str, L"%d", (UINTN) MappingInfo.Hi);
  if (MappingInfo.Csd.Str != NULL) {
    CatPrint (&Str, L"%s", MappingInfo.Csd.Str);
    FreePool (MappingInfo.Csd.Str);
  }

  if (Str.Str != NULL) {
    CatPrint (&Str, L":");
  }

  NewSize           = (Str.Len + 1) * sizeof (CHAR16);
  Str.Str           = ReallocatePool (Str.Len, NewSize, Str.Str);
  if (Str.Str == NULL) {
    return (NULL);
  }
  Str.Str[Str.Len]  = CHAR_NULL;
  return Str.Str;
}

/**
  Function to search the list of mappings for the node on the list based on the key.

  @param[in] MapKey       String Key to search for on the map

  @return the node on the list.
**/
SHELL_MAP_LIST *
EFIAPI
ShellCommandFindMapItem (
  IN CONST CHAR16 *MapKey
  )
{
  SHELL_MAP_LIST *MapListItem;

  for ( MapListItem = (SHELL_MAP_LIST *)GetFirstNode(&gShellMapList.Link)
      ; !IsNull(&gShellMapList.Link, &MapListItem->Link)
      ; MapListItem = (SHELL_MAP_LIST *)GetNextNode(&gShellMapList.Link, &MapListItem->Link)
     ){
    if (gUnicodeCollation->StriColl(gUnicodeCollation,MapListItem->MapName,(CHAR16*)MapKey) == 0) {
      return (MapListItem);
    }
  }
  return (NULL);
}


