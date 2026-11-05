/* Production usbsleep.h transaction with fault-injected HCD/class/NT boundaries.
 * Models ownership and ordering, not controller registers or ACPI firmware. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char UCHAR;
typedef unsigned long ULONG;
typedef int BOOLEAN, KIRQL, OS_IRQL;
#define UsbLog(...) ((void)0)
#define TRUE 1
#define FALSE 0
#define USB_OK 0
#define USB_ERR_IO -1
#define USB_ERR_NODEV -2
#define USB_ERR_NOMEM -3
#define USB_ERR_BUSY -9
#define USB_ERR_PARAM -10
#define USB_MAX_PORTS 4
#define UD_PIPES 2
#define USB_RT_OUT 0
#define USB_RT_STD 0
#define USB_RT_DEVICE 0
#define USB_REQ_SET_CONFIGURATION 9
#define USB_DT_CONFIG 2
#define PS_CONNECT 1
#define USBD_STATUS_CANCELED -11
#define CONTAINING_RECORD(p,t,f) ((t*)((char*)(p)-offsetof(t,f)))
typedef struct List {struct List *Flink,*Blink;} LIST_ENTRY;
static void InitializeListHead(LIST_ENTRY*p){p->Flink=p->Blink=p;}
static int IsListEmpty(LIST_ENTRY*p){return p->Flink==p;}
static LIST_ENTRY*RemoveHeadList(LIST_ENTRY*p){LIST_ENTRY*n=p->Flink;p->Flink=n->Flink;n->Flink->Blink=p;return n;}
typedef struct USB_DEV USB_DEV;
typedef struct USB_HC USB_HC;
typedef struct {USB_DEV*Dev;int Opened,Toggle,Halted,IsoNext,Resource,Active;} USB_PIPE;
typedef struct {int(*PipeOpen)(USB_PIPE*);void(*PipeClose)(USB_PIPE*);void(*DevFree)(USB_DEV*);void(*Stop)(USB_HC*);int(*Release)(USB_HC*);int(*Start)(USB_HC*);ULONG(*PortStatus)(USB_HC*,int);} HCD_OPS;
struct USB_DEV {USB_HC*Hc;USB_DEV*Parent;USB_PIPE Ep0;int Sleeping,SleepConfig,ConfigValue,Index,NativeIf,Gone,Ready,IsHub;unsigned short ConfigLen;UCHAR*Config;int gates[5],resources[5],cli,cli_busy,cli_failed,context,connected,identity,changed_config;};
struct USB_HC {HCD_OPS*Ops;USB_HC*Ehci;USB_DEV*RootDev[USB_MAX_PORTS+1];int PortRetry[USB_MAX_PORTS+1];int Running,Removed,Sleeping,Bus,Dev,Fn,NumPorts;void*Priv;};
typedef struct {USB_PIPE P;int Used;} UD_PIPE;
typedef struct {void*F;UCHAR Alt,Num;} IFACE;
typedef struct Func {struct Func*Next;USB_DEV*Dev;int Dead,Resetting,ResetFailed,Suspended,Config;UD_PIPE Pipe[UD_PIPES];IFACE Iface[32];} UD_FUNC;
typedef struct {LIST_ENTRY Link;} UD_REQ;
static UD_FUNC*Funcs;
static struct {LIST_ENTRY Wait;int Reset,Retry;} Ep0[3];
static int UdLock,Kick,locked;
static USB_HC *UsbHcs[2];static int UsbHcCount;
static USB_DEV devices[3];static UD_FUNC funcs[6];static USB_HC hcs[2];
static UCHAR cfg[9]={9,2,9,0,2,1,0,128,50};
static int step,fail_step,release_fail,start_fail,port_change,stop_calls,start_calls,removed,allocs,commits,checks;
static int fault(void){return ++step==fail_step?USB_ERR_IO:USB_OK;}
static void*OsAlloc(size_t n){void*p;if(fault())return NULL;p=malloc(n);assert(p);allocs++;return p;}
static void OsFree(void*p){assert(p&&allocs>0);free(p);allocs--;}
static void OsWakeWorker(void){}
static OS_IRQL OsLock(void){assert(!locked);locked=1;return 0;}
static void OsUnlock(OS_IRQL k){assert(locked&&!k);locked=0;}
static void KeAcquireSpinLock(int*l,KIRQL*k){assert(l==&UdLock&&!locked);locked=1;*k=0;}
static void KeReleaseSpinLock(int*l,KIRQL k){assert(l==&UdLock&&locked&&!k);locked=0;}
static int CliBeginDeviceReset(USB_DEV*d){if(d->cli||d->cli_busy)return 0;d->cli=1;return 1;}
static void CliAbortDeviceReset(USB_DEV*d){assert(d->cli);d->cli=0;}
static void CliEndDeviceReset(USB_DEV*d,int r){assert(d->cli);d->cli=0;d->cli_failed=r!=0;}
static void AbortPipe(UD_PIPE*p){assert(p->Used&&p->P.Dev->Sleeping);p->P.Active=0;}
static void closepipe(USB_PIPE*p){p->Opened=p->Resource=p->Active=0;}
static void UsbClosePipe(USB_PIPE*p){closepipe(p);}
static int openpipe(USB_PIPE*p){assert(p->Dev->Hc->Running&&p->Dev->cli);p->Resource=1;return fault();}
static void Finish(UD_REQ*r,int status,LIST_ENTRY*done){(void)r;(void)done;assert(locked&&status==USBD_STATUS_CANCELED);}
static void CompleteList(LIST_ENTRY*d){(void)d;assert(!locked);}
static void begin(USB_DEV*d,int k){assert(d->cli&&d->Sleeping);d->gates[k]=1;d->resources[k]=0;}
static int restore(USB_DEV*d,int k){assert(d->gates[k]&&d->Hc->Running);d->resources[k]=1;return fault();}
static void end(USB_DEV*d,int k,int r){assert(d->gates[k]);if(r)d->resources[k]=0;else{assert(d->resources[k]);d->gates[k]=0;commits++;}}
#define NATIVE(name,k) \
static int name##ResetInterfaces(USB_DEV*d){return d->NativeIf&(1<<k);} \
static void name##BeginDeviceReset(USB_DEV*d){begin(d,k);} \
static int name##RestoreDevice(USB_DEV*d){return restore(d,k);} \
static void name##EndDeviceReset(USB_DEV*d,int r){end(d,k,r);}
NATIVE(Hid,0) NATIVE(Msc,1) NATIVE(RtsxUsb,2) NATIVE(Uvc,3) NATIVE(Audio,4)
static void devfree(USB_DEV*d){d->context=0;}
static void stop(USB_HC*h){assert(h->Priv);h->Running=0;stop_calls++;}
static int release(USB_HC*h){int i,k,j;assert(!h->Running);if(release_fail)return USB_ERR_BUSY;
 for(i=0;i<3;i++){USB_DEV*d=&devices[i];if(d->Hc!=h||d->Gone)continue;assert(!d->context&&!d->Ep0.Resource);for(k=0;k<5;k++)assert(!d->resources[k]);
 for(j=0;j<6;j++)if(funcs[j].Dev==d)for(k=0;k<UD_PIPES;k++)assert(!funcs[j].Pipe[k].P.Resource);}
 if(h->Priv){OsFree(h->Priv);h->Priv=NULL;}return 0;}
static int start(USB_HC*h){assert(!h->Priv&&!h->Running);start_calls++;h->Priv=OsAlloc(8);if(!h->Priv)return USB_ERR_NOMEM;if(start_fail)return USB_ERR_IO;h->Running=1;if(port_change)h->NumPorts--;return 0;}
static ULONG status(USB_HC*h,int port){USB_DEV*d=h->RootDev[port];assert(h->Running);return d&&d->connected?PS_CONNECT:0;}
static int UsbResetDevice(USB_DEV*d){assert(d->cli&&d->Sleeping&&!d->context);assert(d->Hc->Running);d->context=1;d->Ep0.Opened=d->Ep0.Resource=1;return fault();}
static int UsbGetDescriptor(USB_DEV*d,int type,int idx,int lang,UCHAR*buf,int len,ULONG*got){assert(type==2&&!idx&&!lang&&len==9);memcpy(buf,d->Config,len);if(d->changed_config)buf[6]++;*got=len;return fault();}
static int UsbCheckDeviceStrings(USB_DEV*d){int r=fault();return r?r:(d->identity?0:USB_ERR_NODEV);}
static int UsbControl(USB_DEV*d,int type,int req,int value,int idx,int len,void*buf,int timeout,void*got){assert(d->Sleeping&&!type&&req==9&&value==d->SleepConfig&&!idx&&!len&&!buf&&timeout==1000&&!got);return fault();}
static int UsbSetInterface(USB_DEV*d,int num,int alt){assert(d->Sleeping&&num==1&&alt==2);return fault();}
static void UsbDevDetach(USB_DEV*d){int i,k;assert(d->Sleeping);d->Gone=1;d->cli=0;d->context=0;closepipe(&d->Ep0);for(k=0;k<5;k++)d->resources[k]=d->gates[k]=0;for(i=0;i<6;i++)if(funcs[i].Dev==d){funcs[i].Dead=1;for(k=0;k<UD_PIPES;k++)closepipe(&funcs[i].Pipe[k].P);}removed++;}
static int UsbReleaseController(USB_HC*h){OS_IRQL k=OsLock();int running=h->Running;OsUnlock(k);if(running)return USB_ERR_BUSY;return h->Ops->Release(h);}
#include "usbsleep.h"
static HCD_OPS ops={openpipe,closepipe,devfree,stop,release,start,status};
static void setup(void){int i,j,k;assert(!allocs);memset(devices,0,sizeof(devices));memset(funcs,0,sizeof(funcs));memset(hcs,0,sizeof(hcs));
 step=fail_step=release_fail=start_fail=port_change=stop_calls=start_calls=removed=commits=0;checks++;
 hcs[0].Ops=&ops;hcs[0].Running=1;hcs[0].NumPorts=3;hcs[0].Bus=2;hcs[0].Dev=3;hcs[0].Fn=1;hcs[0].Priv=OsAlloc(8);hcs[1].Ops=&ops;hcs[1].NumPorts=1;UsbHcs[0]=&hcs[0];UsbHcs[1]=&hcs[1];UsbHcCount=2;Funcs=funcs;
 for(i=0;i<3;i++){USB_DEV*d=&devices[i];d->Hc=&hcs[0];d->Index=i;d->Ready=1;d->NativeIf=31;d->Config=cfg;d->ConfigLen=9;d->ConfigValue=1;d->Ep0.Dev=d;d->Ep0.Resource=d->Ep0.Opened=1;d->context=d->connected=d->identity=1;hcs[0].RootDev[i+1]=d;InitializeListHead(&Ep0[i].Wait);for(k=0;k<5;k++)d->resources[k]=1;
 for(j=0;j<2;j++){UD_FUNC*f=&funcs[i*2+j];f->Next=i*2+j<5?f+1:NULL;f->Dev=d;f->Config=1;f->Suspended=j;f->Iface[1].F=f;f->Iface[1].Num=1;f->Iface[1].Alt=2;for(k=0;k<2;k++){UD_PIPE*p=&f->Pipe[k];p->Used=1;p->P.Dev=d;p->P.Opened=p->P.Resource=p->P.Active=1;}}}step=0;}
static int power(int op,BOOLEAN*s){return UsbdiControllerPower(2,3,1,op,s);}
static void retained(int sleeping){int i,j,k;assert(hcs[0].Sleeping==sleeping);for(i=0;i<3;i++){USB_DEV*d=&devices[i];if(d->Gone)continue;assert(hcs[0].RootDev[i+1]==d&&d->Sleeping==sleeping&&d->cli==sleeping);for(k=0;k<5;k++)assert(d->gates[k]==sleeping);for(j=0;j<2;j++){UD_FUNC*f=&funcs[i*2+j];assert(f->Dev==d&&!f->Dead&&f->Resetting==sleeping&&f->Suspended==j);}}}
static void cleanup(void){int i,k;for(i=0;i<3;i++){devices[i].context=0;closepipe(&devices[i].Ep0);for(k=0;k<5;k++)devices[i].resources[k]=0;}for(i=0;i<6;i++)for(k=0;k<2;k++)closepipe(&funcs[i].Pipe[k].P);release_fail=0;hcs[0].Running=0;assert(!release(&hcs[0]));assert(!allocs);}
int main(void){int i,n;BOOLEAN sleeping;
 setup();assert(!power(0,&sleeping)&&!sleeping&&!stop_calls);retained(0);assert(!power(1,&sleeping)&&sleeping&&!hcs[0].Running&&!hcs[0].Priv);retained(1);assert(!power(2,&sleeping)&&!sleeping&&hcs[0].Running&&!removed);retained(0);n=step;cleanup();
 /* Every allocation/control/class/alternate/endpoint restore failure must
    park all siblings, preserve handles and allow an unchanged D0 retry. */
 for(i=1;i<=n;i++){setup();assert(!power(1,&sleeping));fail_step=i;step=0;int r=power(2,&sleeping);if(r){assert(sleeping&&!hcs[0].Running&&!commits&&!removed);retained(1);fail_step=0;assert(!power(2,&sleeping));}retained(0);cleanup();}
 for(i=0;i<6;i++){setup();devices[0].cli_failed=1;switch(i){case 0:devices[2].IsHub=1;break;case 1:devices[2].NativeIf=128;break;case 2:devices[2].cli_busy=1;break;case 3:devices[2].Ready=0;break;case 4:devices[2].Parent=&devices[0];break;case 5:hcs[1].Ehci=&hcs[0];hcs[1].RootDev[1]=&devices[2];break;}assert(power(0,&sleeping)==USB_ERR_BUSY&&!stop_calls);assert(devices[0].cli_failed);retained(0);assert(power(1,&sleeping)==USB_ERR_BUSY&&!stop_calls);retained(0);cleanup();}
 setup();devices[0].cli_failed=1;assert(!power(0,&sleeping)&&devices[0].cli_failed);retained(0);cleanup();
 setup();release_fail=1;assert(power(1,&sleeping)==USB_ERR_BUSY&&sleeping&&hcs[0].Priv);assert(power(2,&sleeping)==USB_ERR_BUSY&&!start_calls);retained(1);release_fail=0;assert(!power(2,&sleeping));retained(0);cleanup();
 setup();assert(!power(1,&sleeping));start_fail=1;assert(power(2,&sleeping)&&sleeping&&!hcs[0].Priv);retained(1);start_fail=0;assert(!power(2,&sleeping));retained(0);cleanup();
 setup();assert(!power(1,&sleeping));port_change=1;assert(power(2,&sleeping)&&sleeping&&hcs[0].NumPorts==3);retained(1);port_change=0;assert(!power(2,&sleeping));retained(0);cleanup();
 for(i=0;i<3;i++){setup();assert(!power(1,&sleeping));if(i==0)devices[1].connected=0;else if(i==1)devices[1].identity=0;else devices[1].changed_config=1;assert(!power(2,&sleeping)&&removed==1&&!hcs[0].RootDev[2]);retained(0);cleanup();}
 printf("PASS: %d production retained-controller transactions, %d restore fault points, preflight rollback, failed start/release retry, changed identity/removal, six Windows functions and all five native owner gates\n",checks,n);
 return 0;}
