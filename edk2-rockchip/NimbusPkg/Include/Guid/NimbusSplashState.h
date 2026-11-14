/** @file
  NIMBUS_SPLASH_STATE: the record the firmware splash publishes as a configuration table.

  80 bytes, little-endian, magic "KUMOSPLS". The layout is owned by kumo-splash
  (lib/kumo-splash/src/record.rs in the KUMO tree); never parse it here, only publish it.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef NIMBUS_SPLASH_STATE_H_
#define NIMBUS_SPLASH_STATE_H_

#define NIMBUS_SPLASH_STATE_GUID \
  { 0x4F6D983C, 0xC4BB, 0x4218, { 0x9D, 0x31, 0xBA, 0x44, 0x7B, 0x4B, 0xC8, 0x1F } }

#define NIMBUS_SPLASH_STATE_SIZE  80

extern EFI_GUID  gNimbusSplashStateGuid;

#endif
