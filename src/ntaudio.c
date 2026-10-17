/*
 * ntaudio.c - \Device\UsbNtAudio: hands the USB audio class interface
 * (usbntaud.h) to other kernel drivers such as the AUDIONT sound driver.
 */

#include "ntdrv.h"
#include "usbaudio.h"

static PDEVICE_OBJECT AudioDevice;

static const USBNT_AUDIO_IF AudioIf = {
    USBNT_AUDIO_IF_VERSION,
    sizeof(USBNT_AUDIO_IF),
    UaRegister,
    UaUnregister,
    UaGetInfo,
    UaOpen,
    UaClose,
    UaStart,
    UaStop,
    UaSetVolume,
    UaSelectInput,
    UaGetStats,
    UaMidiOut
};

NTSTATUS NtAudioCreate(PDRIVER_OBJECT Drv)
{
    UNICODE_STRING uname;
    NT_EXT *ext;
    NTSTATUS st;

    RtlInitUnicodeString(&uname, USBNT_AUDIO_DEVICE_NAME);
    st = IoCreateDevice(Drv, sizeof(USBNT_AUDIO_EXT), &uname, FILE_DEVICE_UNKNOWN, 0, FALSE, &AudioDevice);
    if (!NT_SUCCESS(st)) {
        UsbLog(LOG_ERR, "cannot create the audio interface device (%x)\n", st);
        return st;
    }
    ext = (NT_EXT *)AudioDevice->DeviceExtension;
    ext->Kind = EXT_AUDIO;
    ext->Self = AudioDevice;
    ((USBNT_AUDIO_EXT *)ext)->Magic = USBNT_AUDIO_EXT_MAGIC;
    ((USBNT_AUDIO_EXT *)ext)->If = &AudioIf;
    AudioDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

NTSTATUS NtAudioInternal(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;

    (void)Dev;
    Irp->IoStatus.Information = 0;
    if (sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_USBNT_AUDIO_INTERFACE) {
        USBNT_AUDIO_IF *out = (USBNT_AUDIO_IF *)Irp->UserBuffer;
        ULONG len = sp->Parameters.DeviceIoControl.OutputBufferLength;
        if (out == NULL || len < 2 * sizeof(ULONG)) {
            st = STATUS_BUFFER_TOO_SMALL;
        } else {
            /* an older client gets the part of the table it knows */
            if (len > sizeof(AudioIf)) {
                len = sizeof(AudioIf);
            }
            UsbMemCpy(out, &AudioIf, len);
            out->Size = len;
            Irp->IoStatus.Information = len;
            st = STATUS_SUCCESS;
        }
    }
    Irp->IoStatus.Status = st;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}
