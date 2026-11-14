/** @file
  The Kumo splash (PROPOSAL/024 KS2, revised by §10: "less is more").

  From the moment the boot manager's consoles are up (the platform's AfterConsole event) until
  something else needs the glass, this driver shows CRUX's Kumo mark, still, with a thin progress
  bar under it. The bar is the only thing that moves. It shows the firmware's milestone and creeps
  toward Nijigumo's without ever reaching it.

  It starts then, not at the first GOP, because connecting the consoles makes GraphicsConsole
  clear the screen, so anything drawn earlier would be erased. It draws through the console
  splitter's GOP, so every connected display shows the same picture. Every pixel comes from
  kumo-splash, the same Rust code Nijigumo and drv-fb run, and the bar is a pure function of the
  ARM generic counter.

  The driver publishes NIMBUS_SPLASH_STATE (the splash epoch, counter frequency and mode) as a
  configuration table for the later stages.

  It never hides anything. It stops for good the moment anyone else uses the screen:
    - console text (ConOut OutputString or ClearScreen: setup, a boot error, a loader's output);
    - another GOP Blt or SetMode;
    - a direct framebuffer write (caught by reading back sample pixels before each frame);
    - ExitBootServices.
  The last frame stays on the glass.

  Drawing never takes more than half the CPU: the next frame is due no sooner than twice the time
  the last one took, and no sooner than PcdNimbusSplashFrameMs.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Guid/NimbusSplashState.h>
#include <Library/ArmGenericTimerCounterLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextOut.h>

// kumo_splash.h speaks C99 fixed-width types; EDK2 has no libc headers, so supply them.
typedef UINT8   uint8_t;
typedef INT32   int32_t;
typedef UINT32  uint32_t;
typedef UINT64  uint64_t;
typedef UINTN   size_t;
#define KUMO_SPLASH_NO_STDINT
#include "Lib/kumo_splash.h"

#define SAMPLE_GRID  8

//
// The platform signals this event group at the start of PlatformBootManagerAfterConsole
// (RockchipPkg.dec: gRockchipEventPlatformBmAfterConsoleGuid). Kept local so NimbusPkg does not
// depend on RockchipPkg; a platform without it simply never starts the splash.
//
STATIC EFI_GUID  mAfterConsoleGuid = {
  0xf1272c11, 0xb418, 0x40ca, { 0x88, 0x26, 0x11, 0x94, 0xd7, 0xb7, 0x30, 0x4a }
};

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mGop;
STATIC EFI_EVENT                     mAfterConsoleEvent;
// A physical display's linear framebuffer at the same mode, for the read-back check. The
// splitter's GOP may have none; a loader writing pixels directly would bypass every hook.
STATIC volatile UINT32               *mFb;
STATIC UINT32                        mFbStride;
STATIC BOOLEAN                       mFbIsRgb;
STATIC EFI_EVENT                     mTimer;
STATIC EFI_EVENT                     mExitBootServicesEvent;
STATIC BOOLEAN                       mStopped;

STATIC UINT32  mWidth;
STATIC UINT32  mHeight;
STATIC UINT32  mAreaX, mAreaY, mAreaW, mAreaH;   // the mark
STATIC UINT32  mBarX, mBarY, mBarW, mBarH;        // the bar: the only part redrawn per tick
STATIC UINT32  *mBack;   // what we last put on the glass, as EFI_GRAPHICS_OUTPUT_BLT_PIXEL (BGRX)
STATIC UINT64  mEpoch;
STATIC UINT64  mFreq;
STATIC UINT8   mRecord[NIMBUS_SPLASH_STATE_SIZE];
STATIC UINT32  mFrames;

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT       mOrigBlt;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE  mOrigSetMode;
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL        *mConOut;
STATIC EFI_TEXT_STRING                        mOrigOutputString;
STATIC EFI_TEXT_CLEAR_SCREEN                  mOrigClearScreen;

STATIC
VOID
FindReadBackFramebuffer (
  VOID
  );

STATIC
VOID
StopSplash (
  IN CONST CHAR8  *Why,
  IN BOOLEAN      CanCallBootServices
  )
{
  if (mStopped) {
    return;
  }

  mStopped = TRUE;
  if (CanCallBootServices && (mTimer != NULL)) {
    gBS->SetTimer (mTimer, TimerCancel, 0);
  }

  // Put the hooks back where they are still ours; the last frame stays on the glass.
  if ((mGop != NULL) && (mOrigBlt != NULL)) {
    mGop->Blt     = mOrigBlt;
    mGop->SetMode = mOrigSetMode;
  }

  if ((mConOut != NULL) && (mOrigOutputString != NULL)) {
    mConOut->OutputString = mOrigOutputString;
    mConOut->ClearScreen  = mOrigClearScreen;
  }

  DEBUG ((DEBUG_INFO, "NimbusSplash: stopped after %u frames (%a)\n", mFrames, Why));
}

STATIC
EFI_STATUS
EFIAPI
HookOutputString (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN CHAR16                           *String
  )
{
  EFI_TEXT_STRING  Original = mOrigOutputString;

  StopSplash ("console text", TRUE);
  return Original (This, String);
}

STATIC
EFI_STATUS
EFIAPI
HookClearScreen (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This
  )
{
  EFI_TEXT_CLEAR_SCREEN  Original = mOrigClearScreen;

  StopSplash ("console clear", TRUE);
  return Original (This);
}

STATIC
EFI_STATUS
EFIAPI
HookBlt (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer OPTIONAL,
  IN  EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN  UINTN                              SourceX,
  IN  UINTN                              SourceY,
  IN  UINTN                              DestinationX,
  IN  UINTN                              DestinationY,
  IN  UINTN                              Width,
  IN  UINTN                              Height,
  IN  UINTN                              Delta OPTIONAL
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT  Original = mOrigBlt;

  if (BltOperation != EfiBltVideoToBltBuffer) {
    StopSplash ("another Blt", TRUE);
  }

  return Original (This, BltBuffer, BltOperation, SourceX, SourceY, DestinationX, DestinationY, Width, Height, Delta);
}

STATIC
EFI_STATUS
EFIAPI
HookSetMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN  UINT32                        ModeNumber
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE  Original = mOrigSetMode;

  StopSplash ("mode change", TRUE);
  return Original (This, ModeNumber);
}

/**
  TRUE if the glass no longer shows what we last drew: someone wrote the framebuffer directly.
  Only possible with a linear 32-bit framebuffer; with Blt-only displays the Blt hook covers it.
**/
STATIC
BOOLEAN
GlassWasTouched (
  VOID
  )
{
  UINT32  Gx, Gy, X, Y, Want, Have;

  if (mFb == NULL) {
    return FALSE;
  }

  for (Gy = 0; Gy < SAMPLE_GRID; Gy++) {
    for (Gx = 0; Gx < SAMPLE_GRID; Gx++) {
      // Half the samples across the whole glass, half inside the cloud's area.
      if (((Gx + Gy) & 1) == 0) {
        X = (2 * Gx + 1) * mWidth / (2 * SAMPLE_GRID);
        Y = (2 * Gy + 1) * mHeight / (2 * SAMPLE_GRID);
      } else {
        X = mAreaX + (2 * Gx + 1) * mAreaW / (2 * SAMPLE_GRID);
        Y = mAreaY + (2 * Gy + 1) * mAreaH / (2 * SAMPLE_GRID);
      }

      Want = mBack[Y * mWidth + X] & 0x00FFFFFF;
      Have = mFb[Y * mFbStride + X] & 0x00FFFFFF;
      if (mFbIsRgb) {
        Have = ((Have & 0xFF) << 16) | (Have & 0xFF00) | ((Have >> 16) & 0xFF);
      }

      if (Want != Have) {
        return TRUE;
      }
    }
  }

  return FALSE;
}

STATIC
VOID
EFIAPI
SplashTick (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT64  Start;
  UINT64  Took;
  UINT64  Next;
  INT32   Rc;

  if (mStopped) {
    return;
  }

  if ((mFrames > 0) && GlassWasTouched ()) {
    StopSplash ("framebuffer written", TRUE);
    return;
  }

  Start = ArmGenericTimerGetSystemCount ();
  Rc    = kumo_splash_bar (
            mWidth,
            mHeight,
            mWidth,
            KUMO_SPLASH_FORMAT_BGR,
            Start,
            mEpoch,
            mFreq,
            0,
            mBack,
            (UINTN)mWidth * mHeight
            );
  if (Rc != KUMO_SPLASH_OK) {
    StopSplash ("render refused", TRUE);
    return;
  }

  if (mFrames == 0) {
    // First frame: own the whole glass: background, mark, bar.
    mOrigBlt (mGop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)mBack, EfiBltBufferToVideo, 0, 0, 0, 0, mWidth, mHeight, mWidth * 4);
  } else {
    mOrigBlt (mGop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)mBack, EfiBltBufferToVideo, mBarX, mBarY, mBarX, mBarY, mBarW, mBarH, mWidth * 4);
  }

  Took = ArmGenericTimerGetSystemCount () - Start;
  if ((mFrames == 0) || ((mFrames & 31) == 0)) {
    DEBUG ((
      DEBUG_INFO,
      "NimbusSplash: frame %u at %lu ticks since epoch, %lu us to draw\n",
      mFrames,
      Start - mEpoch,
      Took * 1000000 / mFreq
      ));
  }

  mFrames++;

  // Next frame no sooner than the PCD interval, and no sooner than twice the drawing time.
  Next = (UINT64)PcdGet32 (PcdNimbusSplashFrameMs) * 10000;   // 100 ns units
  if (Took * 2 * 10000000 / mFreq > Next) {
    Next = Took * 2 * 10000000 / mFreq;
  }

  gBS->SetTimer (mTimer, TimerRelative, Next);
}

STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  StopSplash ("ExitBootServices", FALSE);
}

STATIC
EFI_STATUS
StartSplash (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop
  )
{
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info = Gop->Mode->Info;
  EFI_STATUS                            Status;
  UINT64                                T0;
  UINT32                                Format;
  UINT32                                Stride;

  mWidth  = Info->HorizontalResolution;
  mHeight = Info->VerticalResolution;
  mFreq   = ArmGenericTimerGetTimerFreq ();
  if ((mWidth == 0) || (mHeight == 0) || (mFreq == 0)) {
    return EFI_UNSUPPORTED;
  }

  if ((kumo_splash_area (mWidth, mHeight, &mAreaX, &mAreaY, &mAreaW, &mAreaH) != KUMO_SPLASH_OK) ||
      (kumo_splash_bar_area (mWidth, mHeight, &mBarX, &mBarY, &mBarW, &mBarH) != KUMO_SPLASH_OK))
  {
    return EFI_UNSUPPORTED;
  }

  mBack = AllocatePool ((UINTN)mWidth * mHeight * sizeof (UINT32));
  if (mBack == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  T0 = ArmGenericTimerGetSystemCount ();
  if (kumo_splash_still (mWidth, mHeight, mWidth, KUMO_SPLASH_FORMAT_BGR, mBack, (UINTN)mWidth * mHeight) != KUMO_SPLASH_OK) {
    return EFI_UNSUPPORTED;
  }

  DEBUG ((
    DEBUG_INFO,
    "NimbusSplash: %ux%u, mark %ux%u at (%u,%u), bar %ux%u at (%u,%u), still drawn in %lu us\n",
    mWidth,
    mHeight,
    mAreaW,
    mAreaH,
    mAreaX,
    mAreaY,
    mBarW,
    mBarH,
    mBarX,
    mBarY,
    (ArmGenericTimerGetSystemCount () - T0) * 1000000 / mFreq
    ));

  // The splash epoch: the counter value of the first frame.
  mEpoch = ArmGenericTimerGetSystemCount ();

  Format = (Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) ? KUMO_SPLASH_FORMAT_RGB : KUMO_SPLASH_FORMAT_BGR;
  Stride = (Info->PixelsPerScanLine >= mWidth) ? Info->PixelsPerScanLine : mWidth;
  if (kumo_splash_record (mEpoch, mFreq, mWidth, mHeight, Stride, Format, mRecord, sizeof (mRecord)) == KUMO_SPLASH_OK) {
    Status = gBS->InstallConfigurationTable (&gNimbusSplashStateGuid, mRecord);
    DEBUG ((DEBUG_INFO, "NimbusSplash: epoch %lu at %lu Hz, state published (%r)\n", mEpoch, mFreq, Status));
  }

  // Take the hooks: anyone else drawing, printing or changing mode ends the splash.
  mGop          = Gop;
  mOrigBlt      = Gop->Blt;
  mOrigSetMode  = Gop->SetMode;
  Gop->Blt      = HookBlt;
  Gop->SetMode  = HookSetMode;
  if (gST->ConOut != NULL) {
    mConOut                = gST->ConOut;
    mOrigOutputString      = mConOut->OutputString;
    mOrigClearScreen       = mConOut->ClearScreen;
    mConOut->OutputString  = HookOutputString;
    mConOut->ClearScreen   = HookClearScreen;
  }

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, SplashTick, NULL, &mTimer);
  if (EFI_ERROR (Status)) {
    StopSplash ("no timer", TRUE);
    return Status;
  }

  gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, OnExitBootServices, NULL, &gEfiEventExitBootServicesGuid, &mExitBootServicesEvent);

  FindReadBackFramebuffer ();
  DEBUG ((DEBUG_INFO, "NimbusSplash: read-back check %a\n", (mFb != NULL) ? "on" : "off (no linear framebuffer)"));

  // First frame now, then the timer keeps it moving.
  SplashTick (mTimer, NULL);
  return EFI_SUCCESS;
}

/**
  Find a physical display at the splash's mode with a linear 32-bit framebuffer, for the
  read-back check.
**/
STATIC
VOID
FindReadBackFramebuffer (
  VOID
  )
{
  EFI_HANDLE                            *Handles;
  UINTN                                 Count, Index;
  EFI_GRAPHICS_OUTPUT_PROTOCOL          *Gop;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &Count, &Handles))) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop))) {
      continue;
    }

    Info = Gop->Mode->Info;
    if ((Gop->Mode->FrameBufferBase != 0) &&
        (Info->HorizontalResolution == mWidth) && (Info->VerticalResolution == mHeight) &&
        ((Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) ||
         (Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor)))
    {
      mFb       = (volatile UINT32 *)(UINTN)Gop->Mode->FrameBufferBase;
      mFbStride = Info->PixelsPerScanLine;
      mFbIsRgb  = (Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor);
      break;
    }
  }

  FreePool (Handles);
}

STATIC
VOID
EFIAPI
OnAfterConsole (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_STATUS                    Status;

  gBS->CloseEvent (Event);
  if ((mGop != NULL) || mStopped) {
    return;
  }

  Status = EFI_NOT_FOUND;
  if (gST->ConsoleOutHandle != NULL) {
    Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop);
  }

  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "NimbusSplash: no display; nothing to draw\n"));
    return;
  }

  Status = StartSplash (Gop);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "NimbusSplash: not started (%r); the firmware's usual screen applies\n", Status));
  }
}

EFI_STATUS
EFIAPI
NimbusSplashDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_CALLBACK,
                OnAfterConsole,
                NULL,
                &mAfterConsoleGuid,
                &mAfterConsoleEvent
                );
}
