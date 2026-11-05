"""Compile production SELECT_CONFIGURATION against a side-effect recorder."""
from pathlib import Path
import os, re, subprocess, tempfile
source=Path(os.environ.get('USBDI_SOURCE',Path(__file__).resolve().parents[2]/'src/usbdi.c')).read_text()
def extract(name):
    m=re.search(r'static (?:void|BOOLEAN) '+name+r'\([^;{]*\)\s*\{',source);assert m
    end=m.end();depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[m.start():end]
prefix=r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char UCHAR,BOOLEAN;
typedef unsigned long ULONG;
typedef int NTSTATUS;
#define FIELD_OFFSET(t,f) offsetof(t,f)
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_UNSUCCESSFUL -2
#define USB_OK 0
#define USB_RT_OUT 0
#define USB_RT_STD 0
#define USB_RT_DEVICE 0
#define USB_REQ_SET_CONFIGURATION 9
#define USBD_STATUS_SUCCESS 0
#define USBD_STATUS_SET_CONFIG_FAILED 1
#define USBD_STATUS_INAVLID_CONFIGURATION_DESCRIPTOR 2
#define LOG_ERR 0
typedef void *USBD_CONFIGURATION_HANDLE;
typedef struct {unsigned char data[24];} USBD_PIPE_INFORMATION;
typedef struct {unsigned short Length;UCHAR InterfaceNumber,AlternateSetting;ULONG NumberOfPipes;USBD_PIPE_INFORMATION Pipes[1];} USBD_INTERFACE_INFORMATION,*PUSBD_INTERFACE_INFORMATION;
typedef struct {unsigned short Length;int Status;} HEADER;
struct _URB_SELECT_CONFIGURATION {HEADER H;void*ConfigurationDescriptor;USBD_CONFIGURATION_HANDLE ConfigurationHandle;USBD_INTERFACE_INFORMATION Interface;};
typedef union {HEADER UrbHeader;struct _URB_SELECT_CONFIGURATION UrbSelectConfiguration;} URB,*PURB;
typedef struct {int ConfigValue;} USB_DEV;
typedef struct {int Whole,First,Count,Config,DevIndex;USB_DEV*Dev;} UD_FUNC;
typedef struct {UD_FUNC*F;PURB Urb;NTSTATUS St;} UD_JOB;
static unsigned closes,configs,alts,opens;
static const UCHAR desc[]={9,4,0,0,1,0,0,0,0};
static const UCHAR*FindIface(USB_DEV*d,UCHAR i,UCHAR a){(void)d;return i<4&&a<3?desc:NULL;}
static void ClosePipes(UD_FUNC*f,UCHAR i){(void)f;(void)i;closes++;}
static int UsbControl(USB_DEV*d,int t,int r,int v,int i,int n,void*b,int timeout,void*out){(void)d;(void)t;(void)r;(void)v;(void)i;(void)n;(void)b;(void)timeout;(void)out;configs++;return 0;}
static int UsbSetInterface(USB_DEV*d,UCHAR i,UCHAR a){(void)d;(void)i;(void)a;alts++;return 0;}
static NTSTATUS OpenIface(UD_FUNC*f,PUSBD_INTERFACE_INFORMATION i){(void)f;(void)i;opens++;return 0;}
static void UsbLog(int l,const char*f,...){(void)l;(void)f;}
'''
main=r'''
int main(void){union {URB align;UCHAR bytes[1024];}storage;USB_DEV dev={1};UD_FUNC f;UD_JOB j;PURB u;PUSBD_INTERFACE_INFORMATION first,second;unsigned mode,whole;
 for(whole=0;whole<2;whole++)for(mode=0;mode<13;mode++){
  memset(&storage,0,sizeof(storage));memset(&f,0,sizeof(f));f.Dev=&dev;f.First=0;f.Count=2;f.Whole=whole;f.Config=9;j.F=&f;j.St=99;u=j.Urb=(PURB)&storage;
  first=&u->UrbSelectConfiguration.Interface;second=(void*)((UCHAR*)first+sizeof(*first));first->Length=second->Length=sizeof(*first);first->InterfaceNumber=0;second->InterfaceNumber=1;first->AlternateSetting=second->AlternateSetting=1;
  u->UrbSelectConfiguration.ConfigurationDescriptor=&dev;u->UrbHeader.Length=(unsigned short)((UCHAR*)(second+1)-(UCHAR*)u);closes=configs=alts=opens=0;
  switch(mode){case 1:second->InterfaceNumber=3;f.Whole=0;break;
   case 2:second->Length=1;break;case 3:second->Length=1000;break;
   case 4:u->UrbHeader.Length=(unsigned short)((UCHAR*)second-(UCHAR*)u+1);break;
   case 5:second->AlternateSetting=99;break;case 6:second->Length=offsetof(USBD_INTERFACE_INFORMATION,Pipes);break;
   case 7:second->InterfaceNumber=32;break;
   case 8:u->UrbHeader.Length=offsetof(struct _URB_SELECT_CONFIGURATION,Interface)-1;break;
   case 9:u->UrbSelectConfiguration.ConfigurationDescriptor=NULL;break;
   case 10:second->Length=0;break;
   case 11:u->UrbHeader.Length=(unsigned short)((UCHAR*)second-(UCHAR*)u);break;
   case 12:second->InterfaceNumber=3;f.Whole=1;break;
  }
  SelectConfig(&j);
  if(mode>=1&&mode<=8){assert(j.St==STATUS_INVALID_PARAMETER&&u->UrbHeader.Status==USBD_STATUS_INAVLID_CONFIGURATION_DESCRIPTOR);assert(!closes&&!configs&&!alts&&!opens&&f.Config==9);}
  else{assert(j.St==0&&u->UrbHeader.Status==0&&closes==1);assert(configs==(unsigned)f.Whole);assert(opens==(mode==9?0:mode==10||mode==11?1:2));assert(alts==opens);}
 }
 puts("PASS: 26 production configuration cases; foreign/malformed interfaces rejected before any pipe or hardware mutation");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='usbnt-select-config-') as tmp:
    c=Path(tmp)/'test.c';exe=Path(tmp)/'test'
    c.write_text(prefix+extract('Owns')+extract('SelectConfig')+main)
    subprocess.run(['gcc','-std=c11','-g','-O1','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
