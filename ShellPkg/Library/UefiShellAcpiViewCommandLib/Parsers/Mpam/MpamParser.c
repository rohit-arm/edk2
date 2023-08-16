/** @file
  MPAM table parser

  Copyright (c) 2023, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Specification Reference:
   - [1] ACPI for Memory System Resource Partitioning and Monitoring 2.0
     (https://developer.arm.com/documentation/den0065/latest)

  @par Glossary:
    - MPAM - Memory System Resource Partitioning And Monitoring
    - MSC  - Memory System Component
    - PCC  - Platform Communication Channel
    - RIS  - Resource Instance Selection
    - SMMU - Arm System Memory Management Unit
 **/

#include <IndustryStandard/Mpam.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include "AcpiParser.h"
#include "AcpiView.h"
#include "AcpiViewConfig.h"

// Local variables
STATIC CONST UINT8                   *LocatorType;
STATIC CONST UINT16                  *MpamMscNodeLength;
STATIC UINT32                        MpamMscNodeLengthCumulative;
STATIC UINT32                        HeaderSize;
STATIC CONST UINT32                  *InterfaceType;
STATIC CONST UINT32                  *NumberOfMscResources;
STATIC CONST UINT32                  *NumberOfFunctionalDependencies;
STATIC CONST UINT32                  *NumberOfInterconnectDescriptors;
STATIC CONST UINT64                  *InterconnectTableOffset;
STATIC ACPI_DESCRIPTION_HEADER_INFO  AcpiHdrInfo;

// Array of locator type names. New types should be added keeping the order
// preserved as locator type is used to index into the array while parsing.
STATIC CONST CHAR16  *MpamMscLocatorTitles[] = {
  L"Processor cache",
  L"Memory",
  L"SMMU",
  L"Memory cache",
  L"ACPI device",
  L"Interconnect"
};

/**
  When the length of the table is insufficient to be parsed, this function could
  be used to display an appropriate error message.

  @param [in] ErrorMsg      Error message string that has to be appended to the
                            main error log. This string could explain the reason
                            why a insufficient length error was encountered in
                            the first place.
**/
STATIC
VOID
EFIAPI
MpamLengthError (
  IN CONST CHAR16  *ErrorMsg
  )
{
  IncrementErrorCount ();
  Print (L"\nERROR : ");
  Print (ErrorMsg);
  Print (
    L"\nError : Insufficient MPAM MSC Node length. Table length : %u.\n",
    *(AcpiHdrInfo.Length)
    );
}

/**
  This function validates reserved fields. Any reserved field within the MPAM
  specification must be 0.

  @param [in] Ptr       Pointer to the start of the field data.
  @param [in] Context   Pointer to context specific information. For this
                        particular function, context holds the size of the
                        reserved field that needs to be validated.
**/
STATIC
VOID
EFIAPI
ValidateReserved (
  IN UINT8  *Ptr,
  IN VOID   *Context
  )
{
  UINT64  Reserved;

  Reserved = 0;
  // If Context is not passed on, don't validate. Without the length, it is not
  // possible to decode the reserved field.
  if (Context == NULL) {
    return;
  }

  // Only those cases which are handled currently have been implemented.
  switch ((UINT64)Context) {
    case 8:
      Reserved = (((UINT64)*(Ptr + 7)) << 56);
    // fall through
    case 7:
      Reserved |= (((UINT64)*(Ptr + 6)) << 48);
    // fall through
    case 6:
      Reserved |= (((UINT64)*(Ptr + 5)) << 40);
    // fall through
    case 5:
      Reserved |= (((UINT64)*(Ptr + 4)) << 32);
    // fall through
    case 4:
      Reserved |= (*(Ptr + 3) << 24);
    // fall through
    case 3:
      Reserved |= (*(Ptr + 2) << 16);
    // fall through
    case 2:
      Reserved |= (*(Ptr + 1) << 8);
    // fall through
    case 1:
      Reserved |= (*Ptr);
      break;
    default:
      return;
  }

  if (Reserved != 0) {
    IncrementErrorCount ();
    Print (L"\nERROR : Reserved field should be 0\n");
  }
}

/**
  This function validates the MMIO size within the MSC node body for MPAM ACPI
  table. MPAM ACPI specification states that the MMIO size for an MSC having PCC
  type interface should be zero.

  @param [in] Ptr        Pointer to the start of the field data.
  @param [in] Context    Pointer to context specific information. For this
                         function, context holds the parent/double pointer to a
                         variable holding the interface type. Make sure to call
                         the function accordingly.
**/
STATIC
VOID
EFIAPI
ValidateMmioSize (
  IN UINT8  *Ptr,
  IN VOID   *Context
  )
{
  UINT8   InterfaceType;
  UINT8   **InterfaceTypeParentPtr;
  UINT32  MmioSize;

  InterfaceTypeParentPtr = (UINT8 **)Context;

  if (*InterfaceTypeParentPtr == NULL) {
    MpamLengthError (L"Interface type not set!");
    return;
  }

  InterfaceType = **InterfaceTypeParentPtr;

  if (InterfaceType == EFI_ACPI_MPAM_INTERFACE_PCC) {
    MmioSize = *((UINT32 *)Ptr);

    if (MmioSize != 0) {
      IncrementErrorCount ();
      Print (
        L"\nERROR: MMIO size should be 0 for PCC interface type. Size - %u\n",
        MmioSize
        );
    }
  }
}

/**
  This function decodes and validates the link type for MPAM's interconnect
  descriptor. Valid links are of NUMA and PROC type.

  @param [in] Ptr         Pointer to the start of the field data.
  @param [in] Context     Pointer to context specific information. For this
                          function, context is ignored.
**/
STATIC
VOID
EFIAPI
DecodeLinkType (
  IN UINT8  *Ptr,
  IN VOID   *Context
  )
{
  UINT8  LinkType;

  LinkType = *Ptr;

  if (LinkType == EFI_ACPI_MPAM_LINK_TYPE_NUMA) {
    Print (
      L" (NUMA)"
      );
  } else if (LinkType == EFI_ACPI_MPAM_LINK_TYPE_PROC) {
    Print (
      L" (PROC)"
      );
  } else {
    IncrementErrorCount ();
    Print (
      L"\nERROR: Invalid link type - %u\n",
      (UINT32)LinkType
      );
  }
}

/**
  This function decodes the hardware ID field present within MPAM ACPI table.
  The specification states that the hardware ID has to be set to zero if not
  being used.

  @param [in] Ptr         Pointer to the start of the field data.
  @param [in] Context     Pointer to context specific information. For this
                          function, context is ignored.
**/
STATIC
VOID
EFIAPI
DecodeHardwareId (
  IN UINT8  *Ptr,
  IN VOID   *Context
  )
{
  UINT64  HardwareId;

  HardwareId = *((UINT64 *)Ptr);

  if (HardwareId != 0) {
    Print (L" (");
    Dump8Chars (NULL, Ptr);
    Print (L")");
  }
}

/**
  This function decodes and validates the interface type for MPAM. Valid
  interfaces are of MMIO and PCC type.

  @param [in] Ptr         Pointer to the start of the field data.
  @param [in] Context     Pointer to context specific information. For this
                          function, context is ignored.
**/
STATIC
VOID
EFIAPI
DecodeInterfaceType (
  IN UINT8  *Ptr,
  IN VOID   *Context
  )
{
  UINT8  InterfaceType;

  InterfaceType = *Ptr;

  if (InterfaceType == EFI_ACPI_MPAM_INTERFACE_MMIO) {
    Print (L" (MMIO)");
  } else if (InterfaceType == EFI_ACPI_MPAM_INTERFACE_PCC) {
    Print (L" (PCC)");
  } else {
    IncrementErrorCount ();
    Print (
      L"\nERROR: Invalid interface type - %u\n",
      (UINT32)InterfaceType
      );
  }
}

/**
  This function decodes the interrupt mode flag for MPAM. Interrupt mode could
  either be "edge triggered" or "level triggered".

  @param [in] Ptr        Pointer to the start of the field data.
  @param [in] Context    Pointer to context specific information. For this
                         function, context holds the parent/double pointer to a
                         variable holding the interrupt gsiv. Make sure to call
                         the function accordingly.
**/
STATIC
VOID
EFIAPI
DecodeInterruptMode (
  IN UINT8  *Ptr,
  IN VOID   *Context
  )
{
  UINT8  InterruptMode;

  InterruptMode = *Ptr;

  if (InterruptMode == EFI_ACPI_MPAM_INTERRUPT_LEVEL_TRIGGERED) {
    Print (L" (Level triggered)");
  } else {
    Print (L" (Edge triggered)");
  }
}

/**
  This function decodes the interrupt type flag for MPAM. Interrupt type could
  be "wired interrupt". Other values are reserved at this point.

  @param [in] Ptr        Pointer to the start of the field data.
  @param [in] Context    Pointer to context specific information. For this
                         function, context holds the parent/double pointer to a
                         variable holding the interrupt gsiv. Make sure to call
                         the function accordingly.
**/
STATIC
VOID
EFIAPI
DecodeInterruptType (
  IN UINT8  *Ptr,
  IN VOID   *Context
  )
{
  UINT8  InterruptType;

  InterruptType = *Ptr;

  if (InterruptType == EFI_ACPI_MPAM_INTERRUPT_WIRED) {
    Print (L" (Wired interrupt)");
  } else {
    IncrementWarningCount ();
    Print (L" (Reserved value!)");
  }
}

/**
  This function decodes the interrupt affinity valid flag for MPAM. Interrupt
  affinity could be either be valid or not.

  @param [in] Ptr        Pointer to the start of the field data.
  @param [in] Context    Pointer to context specific information. For this
                         function, context holds the parent/double pointer to a
                         variable holding the interrupt gsiv. Make sure to call
                         the function accordingly.
**/
STATIC
VOID
EFIAPI
DecodeInterruptAffinityValid (
  IN UINT8  *Ptr,
  IN VOID   *Context
  )
{
  UINT8  InterruptAffinityValid;

  InterruptAffinityValid = *Ptr;

  if (InterruptAffinityValid != EFI_ACPI_MPAM_INTERRUPT_AFFINITY_VALID) {
    Print (L" (Affinity not valid)");
  } else {
    Print (L" (Affinity valid)");
  }
}

/**
  This function decodes the interrupt affinity type flag for MPAM. Interrupt
  affinity type could either be "Processor affinity" or "Processor container
  affinity"

  @param [in] Ptr        Pointer to the start of the field data.
  @param [in] Context    Pointer to context specific information. For this
                         function, context holds the parent/double pointer to a
                         variable holding the interrupt gsiv. Make sure to call
                         the function accordingly.
**/
STATIC
VOID
EFIAPI
DecodeInterruptAffinityType (
  IN UINT8  *Ptr,
  IN VOID   *Context
  )
{
  UINT8  InterruptAffinityType;

  InterruptAffinityType = *Ptr;

  if (InterruptAffinityType == EFI_ACPI_MPAM_INTERRUPT_PROCESSOR_AFFINITY) {
    Print (L" (Processor affinity)");
  } else {
    Print (L" (Processor container affinity)");
  }
}

/**
  This function decodes the locator type for a particular MPAM MSC resource.

  @param [in] Ptr        Pointer to the start of the field data.
  @param [in] Context    Pointer to context specific information. For this
                         function, context holds the parent/double pointer to a
                         variable holding the interrupt gsiv. Make sure to call
                         the function accordingly.
**/
STATIC
VOID
EFIAPI
DecodeLocatorType (
  IN UINT8  *Ptr,
  IN VOID   *Context
  )
{
  UINT8  LocatorType;

  LocatorType = *Ptr;

  if (LocatorType <= EFI_ACPI_MPAM_LOCATION_INTERCONNECT) {
    Print (L" (%s)", MpamMscLocatorTitles[LocatorType]);
  } else if (LocatorType == EFI_ACPI_MPAM_LOCATION_UNKNOWN) {
    Print (L" (Unknown)");
  } else {
    Print (L" (Reserved)");
  }
}

/**
  This function traces 7 byte reserved fields.

  @param [in] Format  Optional format string for tracing the data.
  @param [in] Ptr     Pointer to the start of the buffer.
**/
STATIC
VOID
EFIAPI
Dump7Reserved (
  IN CONST CHAR16  *Format OPTIONAL,
  IN UINT8         *Ptr
  )
{
  UINT64  Reserved;

  Reserved  = (((UINT64)*(Ptr + 6)) << 48);
  Reserved |= (((UINT64)*(Ptr + 5)) << 40);
  Reserved |= (((UINT64)*(Ptr + 4)) << 32);
  Reserved |= (*(Ptr + 3) << 24);
  Reserved |= (*(Ptr + 2) << 16);
  Reserved |= (*(Ptr + 1) << 8);
  Reserved |= (*Ptr);

  Print (L"%lu\n", Reserved);
}

/**
  This function traces 3 byte reserved fields.

  @param [in] Format  Optional format string for tracing the data.
  @param [in] Ptr     Pointer to the start of the buffer.
**/
STATIC
VOID
EFIAPI
Dump3Reserved (
  IN CONST CHAR16  *Format OPTIONAL,
  IN UINT8         *Ptr
  )
{
  UINT32  Reserved;

  Reserved  = (*(Ptr + 2) << 16);
  Reserved |= (*(Ptr + 1) << 8);
  Reserved |= (*Ptr);

  Print (L"%u\n", Reserved);
}

/**
  ACPI_PARSER array describing MPAM MSC interrupt flags.
**/
STATIC CONST ACPI_PARSER  MpamMscInterruptFlagParser[] = {
  { L"Interrupt Mode", 1,  0, L"%u", NULL, NULL,
    DecodeInterruptMode, NULL },
  { L"Interrupt Type", 2,  1, L"%u", NULL, NULL,
    DecodeInterruptType, NULL },
  { L"Affinity Type",  1,  3, L"%u", NULL, NULL,
    DecodeInterruptAffinityType, NULL },
  { L"Affinity Valid", 1,  4, L"%u", NULL, NULL,
    DecodeInterruptAffinityValid, NULL },
  { L"Reserved",       27, 5, L"%u", NULL, NULL,
    ValidateReserved, (VOID *)4 }
};

/**
  This function traces MPAM MSC Interrupt Flags.
  If no format string is specified the Format must be NULL.

  @param [in] Format  Optional format string for tracing the data.
  @param [in] Ptr     Pointer to the start of the buffer.
**/
STATIC
VOID
EFIAPI
DumpMpamMscInterruptFlags (
  IN CONST CHAR16  *Format OPTIONAL,
  IN UINT8         *Ptr
  )
{
  if (Format != NULL) {
    Print (Format, *(UINT32 *)Ptr);
    return;
  }

  Print (L"%u\n", *(UINT32 *)Ptr);
  // ParseAcpiBitFields would be called in a nested context (within ParseAcpi).
  // This would lead to an additional newline added after the parsing is
  // complete.  Though it would be ideal to have the MSC node parsed without
  // intermittent newline, this is not tackled at this point to keep the code
  // clean.
  ParseAcpiBitFields (
    TRUE,
    2,
    NULL,
    Ptr,
    4,
    PARSER_PARAMS (MpamMscInterruptFlagParser)
    );
}

/**
  ACPI_PARSER array describing the Generic ACPI MPAM table header.
**/
STATIC CONST ACPI_PARSER  MpamParser[] = {
  PARSE_ACPI_HEADER (&AcpiHdrInfo)
};

/**
  ACPI_PARSER array describing the MPAM MSC node object.
**/
STATIC CONST ACPI_PARSER  MpamMscNodeParser[] = {
  { L"Length",                       2, 0,  L"%u",    NULL,
    (VOID **)&MpamMscNodeLength, NULL, NULL },
  // Once Interface type is decoded, the address of interface type field is
  // captured into InterfaceType pointer so that it could be used to check if
  // MMIO Size field is set as per the specification.
  { L"Interface type",               1, 2,  L"0x%x",  NULL,
    (VOID **)&InterfaceType, DecodeInterfaceType, NULL },
  { L"Reserved",                     1, 3,  L"%u",    NULL,
    NULL, ValidateReserved, (VOID *)1 },
  { L"Identifier",                   4, 4,  L"%u",    NULL,
    NULL, NULL, NULL },
  { L"Base address",                 8, 8,  L"0x%lx", NULL,
    NULL, NULL, NULL },
  { L"MMIO Size",                    4, 16, L"0x%x",  NULL,
    NULL, ValidateMmioSize, (VOID **)&InterfaceType },
  { L"Overflow interrupt",           4, 20, L"%u",    NULL,
    NULL, NULL, NULL },
  { L"Overflow interrupt flags",     4, 24, NULL,     DumpMpamMscInterruptFlags,
    NULL, NULL, NULL },
  { L"Reserved1",                    4, 28, L"%u",    NULL,
    NULL, ValidateReserved, (VOID *)4 },
  { L"Overflow interrupt affinity",  4, 32, L"0x%x",  NULL,
    NULL, NULL, NULL },
  { L"Error interrupt",              4, 36, L"%u",    NULL,
    NULL, NULL, NULL },
  { L"Error interrupt flags",        4, 40, NULL,     DumpMpamMscInterruptFlags,
    NULL, NULL, NULL },
  { L"Reserved2",                    4, 44, L"%u",    NULL,
    NULL, ValidateReserved, (VOID *)4 },
  { L"Error interrupt affinity",     4, 48, L"0x%x",  NULL,
    NULL, NULL, NULL },
  { L"MAX_NRDY_USEC",                4, 52, L"0x%x",  NULL,
    NULL, NULL, NULL },
  { L"Hardware ID of linked device", 8, 56, L"0x%lx", NULL,
    NULL, DecodeHardwareId, NULL },
  { L"Instance ID of linked device", 4, 64, L"0x%x",  NULL,
    NULL, NULL, NULL },
  { L"Number of resource nodes",     4, 68, L"%u",    NULL,
    (VOID **)&NumberOfMscResources, NULL, NULL }
};

/**
  ACPI_PARSER array describing the MPAM MSC resource fields till locator type.
**/
STATIC CONST ACPI_PARSER  MpamMscResourceParser[] = {
  { L"Identifier",   4, 0, L"%u",   NULL, NULL, NULL, NULL },
  { L"RIS index",    1, 4, L"%u",   NULL, NULL, NULL, NULL },
  { L"Reserved1",    2, 5, L"%u",   NULL,
    NULL, ValidateReserved, (VOID *)2 },
  { L"Locator type", 1, 7, L"0x%x", NULL,
    (VOID **)&LocatorType,
    DecodeLocatorType, NULL }
};

/**
  ACPI_PARSER array describing the MPAM MSC processor cache locator field.
**/
STATIC CONST ACPI_PARSER  MpamMscProcessorCacheLocatorParser[] = {
  { L"Cache reference", 8, 0, L"%lu", NULL, NULL, NULL, NULL },
  { L"Reserved",        4, 8, L"%u",  NULL, NULL,
    ValidateReserved, (VOID *)4 }
};

/**
  ACPI_PARSER array describing the MPAM MSC memory locator field.
**/
STATIC CONST ACPI_PARSER  MpamMscMemoryLocatorParser[] = {
  { L"Proximity domain", 8, 0, L"%lu", NULL, NULL, NULL, NULL },
  { L"Reserved",         4, 8, L"%u",  NULL, NULL,
    ValidateReserved, (VOID *)4 }
};

/**
  ACPI_PARSER array describing the MPAM MSC SMMU locator field.
**/
STATIC CONST ACPI_PARSER  MpamMscSMMULocatorParser[] = {
  { L"SMMU interface", 8, 0, L"%lu", NULL, NULL, NULL, NULL },
  { L"Reserved",       4, 8, L"%u",  NULL, NULL,
    ValidateReserved, (VOID *)4 }
};

/**
  ACPI_PARSER array describing the MPAM MSC memory cache locator field.
**/
STATIC CONST ACPI_PARSER  MpamMscMemoryCacheLocatorParser[] = {
  { L"Reserved",  7, 0, L"%lu", Dump7Reserved, NULL,
    ValidateReserved, (VOID *)7 },
  { L"Level",     1, 7, L"%u",  NULL,          NULL,NULL,  NULL },
  { L"Reference", 4, 8, L"%u",  NULL,          NULL,NULL,  NULL }
};

/**
  ACPI_PARSER array describing the MPAM MSC ACPI device locator field.
**/
STATIC CONST ACPI_PARSER  MpamMscAcpiDeviceLocatorParser[] = {
  { L"ACPI hardware ID", 8, 0, L"0x%lx", NULL, NULL,
    DecodeHardwareId, NULL },
  { L"ACPI unique ID",   4, 8, L"%u",    NULL, NULL,NULL,NULL }
};

/**
  ACPI_PARSER array describing the MPAM MSC interconnect locator field.
**/
STATIC CONST ACPI_PARSER  MpamMscInterconnectLocatorParser[] = {
  { L"Interconnect desc tbl offset", 8, 0, L"%lu", NULL,
    (VOID **)&InterconnectTableOffset, NULL, NULL },
  { L"Reserved",                     4, 8, L"%u",  NULL,
    NULL, ValidateReserved, (VOID *)4 }
};

/**
  ACPI_PARSER array describing the MPAM MSC resource locator field.
**/
STATIC CONST ACPI_PARSER  MpamMscGenericLocatorParser[] = {
  { L"Descriptor1", 8, 0, L"%lu", NULL, NULL, NULL, NULL },
  { L"Descriptor2", 4, 8, L"%u",  NULL, NULL, NULL, NULL }
};

/**
  ACPI_PARSER array describing the MPAM MSC resource's functional dependencies
  count.
**/
STATIC CONST ACPI_PARSER  MpamMscFunctionalDependencyCountParser[] = {
  { L"Number of func dependencies", 4, 0, L"%u", NULL,
    (VOID **)&NumberOfFunctionalDependencies, NULL, NULL }
};

/**
  ACPI_PARSER array describing the MPAM MSC resource's functional dependencies.
**/
STATIC CONST ACPI_PARSER  MpamMscFunctionalDependencyParser[] = {
  { L"Producer", 4, 0, L"0x%x", NULL, NULL, NULL, NULL },
  { L"Reserved", 4, 4, L"0x%x", NULL,
    NULL, ValidateReserved, (VOID *)4 },
};

/**
  ACPI_PARSER array describing the interconnect descriptor table associated with
  the interconnect locator type.
**/
STATIC CONST ACPI_PARSER  MpamInterconnectDescriptorTableParser[] = {
  { L"Signature",                   16, 0,
    L"%x%x%x%x-%x%x-%x%x-%x%x-%x%x%x%x%x%x", Dump16Chars, NULL, NULL, NULL },
  { L"Number of Interconnect desc", 4,  16,L"0x%x", NULL,
    (VOID **)&NumberOfInterconnectDescriptors, NULL, NULL }
};

/**
  ACPI_PARSER array describing the interconnect descriptor associated with the
  interconnect locator type.
**/
STATIC CONST ACPI_PARSER  MpamInterconnectDescriptorParser[] = {
  { L"Source ID",      4, 0, L"%u",   NULL,          NULL, NULL, NULL },
  { L"Destination ID", 4, 4, L"%u",   NULL,          NULL, NULL, NULL },
  { L"Link type",      1, 8, L"0x%x", NULL,
    NULL, DecodeLinkType, NULL },
  { L"Reserved",       3, 9, L"%lu",  Dump3Reserved, NULL,
    ValidateReserved, (VOID *)3 }
};

/**
  This function parses the locator field within the resource node for ACPI MPAM
  table. The parsing is based on the locator type field.

  @param [in] Ptr                Pointer to the start of the buffer.
  @param [in] AcpiTableLength    Length of the ACPI table.
  @param [in] Offset             Pointer to current offset within Ptr.
**/
STATIC
VOID
EFIAPI
ParseLocator (
  IN UINT8   *Ptr,
  IN UINT32  AcpiTableLength,
  IN UINT32  *Offset
  )
{
  switch (*LocatorType) {
    case EFI_ACPI_MPAM_LOCATION_PROCESSOR_CACHE:
      *Offset += ParseAcpi (
                   TRUE,
                   4,
                   NULL,
                   Ptr + *Offset,
                   AcpiTableLength - *Offset,
                   PARSER_PARAMS (MpamMscProcessorCacheLocatorParser)
                   );
      break;
    case EFI_ACPI_MPAM_LOCATION_MEMORY:
      *Offset += ParseAcpi (
                   TRUE,
                   4,
                   NULL,
                   Ptr + *Offset,
                   AcpiTableLength - *Offset,
                   PARSER_PARAMS (MpamMscMemoryLocatorParser)
                   );
      break;
    case EFI_ACPI_MPAM_LOCATION_SMMU:
      *Offset += ParseAcpi (
                   TRUE,
                   4,
                   NULL,
                   Ptr + *Offset,
                   AcpiTableLength - *Offset,
                   PARSER_PARAMS (MpamMscSMMULocatorParser)
                   );
      break;
    case EFI_ACPI_MPAM_LOCATION_MEMORY_CACHE:
      *Offset += ParseAcpi (
                   TRUE,
                   4,
                   NULL,
                   Ptr + *Offset,
                   AcpiTableLength - *Offset,
                   PARSER_PARAMS (MpamMscMemoryCacheLocatorParser)
                   );
      break;
    case EFI_ACPI_MPAM_LOCATION_ACPI_DEVICE:
      *Offset += ParseAcpi (
                   TRUE,
                   4,
                   NULL,
                   Ptr + *Offset,
                   AcpiTableLength - *Offset,
                   PARSER_PARAMS (MpamMscAcpiDeviceLocatorParser)
                   );
      break;
    case EFI_ACPI_MPAM_LOCATION_INTERCONNECT:
      *Offset += ParseAcpi (
                   TRUE,
                   4,
                   NULL,
                   Ptr + *Offset,
                   AcpiTableLength - *Offset,
                   PARSER_PARAMS (MpamMscInterconnectLocatorParser)
                   );
      break;
    // For both UNKNOWN and RESERVED locator types, the locator is parsed using
    // the generic locator parser as the spec does not define any format.
    case EFI_ACPI_MPAM_LOCATION_UNKNOWN:
      *Offset += ParseAcpi (
                   TRUE,
                   4,
                   NULL,
                   Ptr + *Offset,
                   AcpiTableLength - *Offset,
                   PARSER_PARAMS (MpamMscGenericLocatorParser)
                   );
      break;
    default:
      Print (L"\nWARNING : Reserved locator type\n");
      *Offset += ParseAcpi (
                   TRUE,
                   4,
                   NULL,
                   Ptr + *Offset,
                   AcpiTableLength - *Offset,
                   PARSER_PARAMS (MpamMscGenericLocatorParser)
                   );
      IncrementWarningCount ();
      break;
  } // switch
}

/**
  PrintBlockTitle could be used to print the title of blocks that
  appear more than once in the MPAM ACPI table.

  @param [in] Indent              Number of spaces to add to the global table
                                  indent. The global table indent is 0 by
                                  default; however this value is updated on
                                  entry to the ParseAcpi() by adding the indent
                                  value provided to ParseAcpi() and restored
                                  back on exit.  Therefore the total indent in
                                  the output is dependent on from where this
                                  function is called.
  @param [in] Title               Title string to be used for the block.
  @param [in] Index               Index of the block.
**/
STATIC
VOID
EFIAPI
PrintBlockTitle (
  IN UINT32        Indent,
  IN CONST CHAR16  *Title,
  IN CONST UINT32  Index
  )
{
  Print (L"\n");
  PrintFieldName (Indent, Title);
  Print (L"%u\n\n", Index);
}

/**
  This function parses the interconnect descriptor(s) associated with
  an interconnect type locator object.

  @param [in] Ptr                Pointer to the start of the buffer.
  @param [in] AcpiTableLength    Length of the ACPI table.
  @param [in] Offset             Pointer to current offset within Ptr.

Returns:

  Status

  EFI_SUCCESS                    MPAM MSC nodes were parsed properly.
  EFI_BAD_BUFFER_SIZE            The buffer pointer provided as input is not
                                 long enough to be parsed correctly.
**/
STATIC
EFI_STATUS
EFIAPI
ParseInterconnectDescriptors (
  IN UINT8   *Ptr,
  IN UINT32  AcpiTableLength,
  IN UINT32  *Offset
  )
{
  UINT32  InterconnectDescriptorIndex;

  InterconnectDescriptorIndex = 0;

  // Parse MPAM MSC resources within the MSC body
  if (NumberOfInterconnectDescriptors == NULL) {
    MpamLengthError (L"Number of interconnect descriptors not set!");
    return EFI_BAD_BUFFER_SIZE;
  }

  while (InterconnectDescriptorIndex < *NumberOfInterconnectDescriptors) {
    PrintBlockTitle (
      6,
      L"* Interconnect descriptor *",
      InterconnectDescriptorIndex
      );

    // Parse interconnect descriptor
    *Offset += ParseAcpi (
                 TRUE,
                 4,
                 NULL,
                 Ptr + *Offset,
                 AcpiTableLength - *Offset,
                 PARSER_PARAMS (MpamInterconnectDescriptorParser)
                 );

    InterconnectDescriptorIndex++;
  }

  return EFI_SUCCESS;
}

/**
  This function parses the interconnect descriptor table associated with an
  interconnect type locator object. It also performs necessary validation to
  make sure the interconnect descriptor is at a valid location.

  @param [in] Ptr                Pointer to the start of the buffer.
  @param [in] AcpiTableLength    Length of the ACPI table.
  @param [in] Offset             Pointer to current offset within Ptr.
  @param [in] InterconnectOffset Offset to the interconnect descriptor table.

Returns:

  Status

  EFI_SUCCESS                    MPAM MSC nodes were parsed properly.
  EFI_BAD_BUFFER_SIZE            The buffer pointer provided as input is not
                                 long enough to be parsed correctly.
  EFI_INVALID_PARAMETER          The Offset parameter encoded within the Ptr
                                 buffer is not valid.
**/
STATIC
EFI_STATUS
EFIAPI
ParseInterconnectDescriptorTable (
  IN UINT8   *Ptr,
  IN UINT32  AcpiTableLength,
  IN UINT32  *Offset,
  IN UINT64  InterconnectOffset
  )
{
  EFI_STATUS  Status;

  // The specification doesn't strictly state that the interconnect table be
  // placed exactly after say, functional dependency table or the resource node.
  // Instead, the interconnect locator's descriptor field 1 gives the offset
  // from the start of the MSC node where the table could be found.
  if (*Offset > (MpamMscNodeLengthCumulative +
                 InterconnectOffset + HeaderSize))
  {
    IncrementErrorCount ();
    Print (L"\nERROR : Parsing Interconnect descriptor table failed!\n");
    Print (
      L"ERROR : Offset overlaps with other objects within the MSC. Offset %u.\n",
      InterconnectOffset
      );

    return EFI_INVALID_PARAMETER;
  }

  if (InterconnectOffset > (*MpamMscNodeLength)) {
    IncrementErrorCount ();
    Print (L"\nERROR : Parsing Interconnect descriptor table failed!\n");
    Print (
      L"ERROR : Offset falls outside MSC's space. Offset %u.\n",
      InterconnectOffset
      );

    return EFI_INVALID_PARAMETER;
  }

  *Offset = HeaderSize + MpamMscNodeLengthCumulative + InterconnectOffset;

  Print (L"\n");
  PrintFieldName (6, L"* Interconnect desc table *");
  Print (L"\n\n");

  // Parse interconnect descriptor table
  *Offset += ParseAcpi (
               TRUE,
               4,
               NULL,
               Ptr + *Offset,
               AcpiTableLength - *Offset,
               PARSER_PARAMS (MpamInterconnectDescriptorTableParser)
               );

  Status = ParseInterconnectDescriptors (Ptr, AcpiTableLength, Offset);
  return Status;
}

/**
  This function parses all the MPAM functional dependency nodes within a
  single resource node.

  @param [in] Ptr                Pointer to the start of the buffer.
  @param [in] AcpiTableLength    Length of the ACPI table.
  @param [in] Offset             Pointer to current offset within Ptr.

Returns:

  Status

  EFI_SUCCESS                    MPAM MSC nodes were parsed properly.
  EFI_BAD_BUFFER_SIZE            The buffer pointer provided as input is not
                                 long enough to be parsed correctly.
**/
STATIC
EFI_STATUS
EFIAPI
ParseMpamMscFunctionalDependencies (
  IN UINT8   *Ptr,
  IN UINT32  AcpiTableLength,
  IN UINT32  *Offset
  )
{
  UINT32  FunctionalDependencyIndex;

  FunctionalDependencyIndex = 0;

  // Parse MPAM MSC resources within the MSC body
  if (NumberOfFunctionalDependencies == NULL) {
    MpamLengthError (L"Number of functional dependencies not set!");
    return EFI_BAD_BUFFER_SIZE;
  }

  while (FunctionalDependencyIndex < *NumberOfFunctionalDependencies) {
    PrintBlockTitle (
      6,
      L"* Functional dependency *",
      FunctionalDependencyIndex
      );

    // Parse functional dependency
    *Offset += ParseAcpi (
                 TRUE,
                 4,
                 NULL,
                 Ptr + *Offset,
                 AcpiTableLength - *Offset,
                 PARSER_PARAMS (MpamMscFunctionalDependencyParser)
                 );

    FunctionalDependencyIndex++;
  }

  return EFI_SUCCESS;
}

/**
  This function parses all the MPAM resource nodes within a single MSC
  node within the MPAM ACPI table.  It also invokes helper functions to
  validate and parse locators and functional dependency descriptors.

  @param [in] Ptr                Pointer to the start of the buffer.
  @param [in] AcpiTableLength    Length of the ACPI table.
  @param [in] Offset             Pointer to current offset within Ptr.

Returns:

  Status

  EFI_SUCCESS                    MPAM MSC nodes were parsed properly.
  EFI_BAD_BUFFER_SIZE            The buffer pointer provided as input is not
                                 long enough to be parsed correctly.
**/
STATIC
EFI_STATUS
EFIAPI
ParseMpamMscResources (
  IN UINT8   *Ptr,
  IN UINT32  AcpiTableLength,
  IN UINT32  *Offset
  )
{
  EFI_STATUS  Status;
  UINT32      ResourceIndex;

  ResourceIndex = 0;

  if (NumberOfMscResources == NULL) {
    MpamLengthError (L"Number of MSC resource not set!");
    return EFI_BAD_BUFFER_SIZE;
  }

  while (ResourceIndex < *NumberOfMscResources) {
    PrintBlockTitle (
      4,
      L"* Resource *",
      ResourceIndex
      );

    // Parse MPAM MSC resources within the MSC body. This could be traced.
    *Offset += ParseAcpi (
                 TRUE,
                 2,
                 NULL,
                 Ptr + *Offset,
                 AcpiTableLength - *Offset,
                 PARSER_PARAMS (MpamMscResourceParser)
                 );

    // Proceed with parsing only if a valid locator has been set.
    if ((LocatorType == NULL)) {
      MpamLengthError (L"Locator type or Locator not set");
      return EFI_BAD_BUFFER_SIZE;
    }

    ParseLocator (Ptr, AcpiTableLength, Offset);

    // Parse the number of functional dependency descriptors.
    *Offset += ParseAcpi (
                 TRUE,
                 2,
                 NULL,
                 Ptr + *Offset,
                 AcpiTableLength - *Offset,
                 PARSER_PARAMS (MpamMscFunctionalDependencyCountParser)
                 );

    Status = ParseMpamMscFunctionalDependencies (Ptr, AcpiTableLength, Offset);
    if (Status != EFI_SUCCESS) {
      return Status;
    }

    // If offset field has been set, parse the interconnect description table.
    if (  (*LocatorType == EFI_ACPI_MPAM_LOCATION_INTERCONNECT)
       && (InterconnectTableOffset != NULL))
    {
      Status = ParseInterconnectDescriptorTable (
                 Ptr,
                 AcpiTableLength,
                 Offset,
                 *InterconnectTableOffset
                 );
      if (Status != EFI_SUCCESS) {
        return Status;
      }
    }

    ResourceIndex++;
  }

  return EFI_SUCCESS;
}

/**
  This function parses all the MPAM MSC nodes within the MPAM ACPI table.  It
  also invokes a helper function to detect and parse resource nodes that maybe
  present.

  @param [in] Ptr                Pointer to the start of the buffer.
  @param [in] AcpiTableLength    Length of the ACPI table.
  @param [in] Offset             Current offset within Ptr.

Returns:

  Status

  EFI_SUCCESS                    MPAM MSC nodes were parsed properly.
  EFI_BAD_BUFFER_SIZE            The buffer pointer provided as input is not
                                 long enough to be parsed correctly.
**/
STATIC
EFI_STATUS
EFIAPI
ParseMpamMscNodes (
  IN UINT8   *Ptr,
  IN UINT32  AcpiTableLength,
  IN UINT32  Offset
  )
{
  EFI_STATUS  Status;
  UINT32      MscIndex;

  MscIndex = 0;

  while (Offset < AcpiTableLength) {
    PrintBlockTitle (2, L"* MSC *", MscIndex);
    // Parse MPAM MSC node
    Offset += ParseAcpi (
                TRUE,
                0,
                NULL,
                Ptr + Offset,
                AcpiTableLength - Offset,
                PARSER_PARAMS (MpamMscNodeParser)
                );

    if (MpamMscNodeLength == NULL) {
      MpamLengthError (L"MPAM MSC node length not set!");
      return EFI_BAD_BUFFER_SIZE;
    }

    if (*MpamMscNodeLength < sizeof (EFI_ACPI_MPAM_MSC_NODE)) {
      Print (L"\nERROR: MSC length should be at least the size of node body! ");
      Print (L"MSC Length %u\n", *MpamMscNodeLength);
      return EFI_BAD_BUFFER_SIZE;
    }

    // Parse MPAM MSC resources within the MSC body
    Status = ParseMpamMscResources (Ptr, AcpiTableLength, &Offset);
    if (Status != EFI_SUCCESS) {
      return Status;
    }

    MpamMscNodeLengthCumulative += (*MpamMscNodeLength);
    MscIndex++;
  }

  return EFI_SUCCESS;
}

/**
  This function parses the MPAM ACPI table's generic header. It also invokes a
  sub routine that would help with parsing rest of the table.

  @param [in] Trace              If TRUE, trace the ACPI fields.
  @param [in] Ptr                Pointer to the start of the buffer.
  @param [in] AcpiTableLength    Length of the ACPI table.
  @param [in] AcpiTableRevision  Revision of the ACPI table.
**/
VOID
EFIAPI
ParseAcpiMpam (
  IN BOOLEAN  Trace,
  IN UINT8    *Ptr,
  IN UINT32   AcpiTableLength,
  IN UINT8    AcpiTableRevision
  )
{
  EFI_STATUS  Status;

  if (!Trace) {
    return;
  }

  // Parse generic table header
  HeaderSize = ParseAcpi (
                 TRUE,
                 0,
                 "MPAM",
                 Ptr,
                 AcpiTableLength,
                 PARSER_PARAMS (MpamParser)
                 );

  Status = ParseMpamMscNodes (Ptr, AcpiTableLength, HeaderSize);
  if (Status == EFI_SUCCESS) {
    // Check if the length of all MPAM MSCs with the header, matches with the
    // ACPI table's length field.
    if (*(AcpiHdrInfo.Length) != (MpamMscNodeLengthCumulative + HeaderSize)) {
      IncrementErrorCount ();
      Print (L"\nERROR: Length mismatch! : ");
      Print (L"MSC Length total != MPAM table length.");
      Print (
        L"Table length : %u MSC total : %u\n",
        *(AcpiHdrInfo.Length),
        MpamMscNodeLengthCumulative
        );
    }
  }
}
