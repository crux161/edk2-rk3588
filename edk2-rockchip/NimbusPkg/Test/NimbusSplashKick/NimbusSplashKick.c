/** @file
  QEMU test for NimbusSplashDxe (see NimbusSplashKick.inf).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/NimbusSplashState.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC EFI_GUID  mAfterConsoleGuid = {
  0xf1272c11, 0xb418, 0x40ca, { 0x88, 0x26, 0x11, 0x94, 0xd7, 0xb7, 0x30, 0x4a }
};

STATIC
VOID
EFIAPI
Noop (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
}

STATIC
VOID
Hold (
  IN UINT64  Microseconds
  )
{
  EFI_EVENT  Timer;
  UINTN      Index;

  gBS->CreateEvent (EVT_TIMER, 0, NULL, NULL, &Timer);
  gBS->SetTimer (Timer, TimerRelative, Microseconds * 10);
  gBS->WaitForEvent (1, &Timer, &Index);
  gBS->CloseEvent (Timer);
}

EFI_STATUS
EFIAPI
NimbusSplashKickMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT   Event;
  UINT8       *State;
  EFI_STATUS  Status;

  gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Noop, NULL, &mAfterConsoleGuid, &Event);
  gBS->SignalEvent (Event);
  gBS->CloseEvent (Event);

  Hold (5000000);

  Status = EfiGetSystemConfigurationTable (&gNimbusSplashStateGuid, (VOID **)&State);
  if (EFI_ERROR (Status)) {
    Print (L"NimbusSplashKick: no NIMBUS_SPLASH_STATE (%r)\n", Status);
  } else {
    Print (
      L"NimbusSplashKick: NIMBUS_SPLASH_STATE %c%c%c%c%c%c%c%c published; this line ends the splash\n",
      State[0], State[1], State[2], State[3], State[4], State[5], State[6], State[7]
      );
  }

  Hold (1500000);
  return EFI_SUCCESS;
}
