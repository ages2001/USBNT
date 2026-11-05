/* Real MSC restore functions with failure-injected transport/context allocation. */
#define MSC_RESET_TEST 1
#define main msc_rundown_main
#include "tmscrundown.c"
#undef main
static unsigned step, fault, submitted, notifications;
static USB_HC hc;
static HCD_OPS ops;
static UCHAR config[128];
static int check(void){return ++step==fault?USB_ERR_IO:USB_OK;}
typedef struct Context {USB_PIPE*child;int count;} CTX;
int UsbControl(USB_DEV*d,UCHAR rt,UCHAR r,USHORT val,USHORT i,USHORT n,void*b,ULONG t,ULONG*a){
    (void)d;(void)rt;(void)n;(void)b;(void)t;(void)a;
    assert(r==USB_REQ_SET_INTERFACE&&i==2&&val==1);return check();
}
int UsbSetInterface(USB_DEV*d,UCHAR i,UCHAR a){return UsbControl(d,0,USB_REQ_SET_INTERFACE,a,i,0,NULL,1000,NULL);}
int UsbGetDescriptor(USB_DEV*d,UCHAR t,UCHAR i,USHORT l,void*b,USHORT n,ULONG*a){
    (void)d;(void)t;(void)i;(void)l;(void)b;(void)n;(void)a;assert(0);return USB_ERR_IO;
}
const UCHAR *UsbNextDesc(const UCHAR*p,const UCHAR*end){
    if(!p||p>=end||p[0]<2)return NULL;p+=p[0];
    return p+2<=end&&p[0]>=2&&p+p[0]<=end?p:NULL;
}
const UCHAR *UsbFindDesc(const UCHAR*p,const UCHAR*end,UCHAR type){
    while((p=UsbNextDesc(p,end))!=NULL){if(p[1]==type)return p;if(p[1]==USB_DT_INTERFACE&&type!=USB_DT_INTERFACE)return NULL;}return NULL;
}
int UsbOpenPipe(USB_DEV*d,USB_PIPE*p,const UCHAR*ep){
    int r=check();assert(!p->Opened&&!p->HcPriv);
    if(r!=USB_OK)return r;
    memset(p,0,sizeof(*p));p->Dev=d;p->Opened=1;p->HcPriv=OsAlloc(sizeof(CTX));
    p->Type=ep[3]&3;p->Endpoint=ep[2]&15;p->DirIn=!!(ep[2]&0x80);return USB_OK;
}
int UsbOpenStreams(USB_PIPE*p,USB_PIPE*child,int count){
    CTX*c=p->HcPriv;int i;assert(c&&count==2&&!c->child);c->child=child;c->count=count;
    for(i=0;i<count;i++){memset(&child[i],0,sizeof(child[i]));child[i].Dev=p->Dev;child[i].Opened=1;child[i].HcPriv=c;}
    return check(); /* failure may leave partial controller allocation */
}
void UsbClosePipe(USB_PIPE*p){
    CTX*c=p->HcPriv;int i;UsbCancelPipe(p);
    if(c){for(i=0;i<c->count;i++){c->child[i].Opened=0;c->child[i].HcPriv=NULL;}OsFree(c);}
    p->HcPriv=NULL;p->Opened=0;
}
static void failed_req(SCSI_REQ*r){assert(r->SrbStatus==SRBST_ERROR&&!r->Transferred&&!r->SenseLength);notifications++;}
static void desc(UCHAR proto,unsigned speed){
    unsigned n=0,i;UCHAR cfg[]={9,USB_DT_CONFIG,0,0,1,1,0,0x80,50};
    UCHAR iface[]={9,USB_DT_INTERFACE,2,1,2,USB_CLASS_MSC,6,0,0};
    memcpy(config,cfg,9);n=9;iface[7]=proto;iface[4]=proto==MSC_PROTO_UAS?4:proto==MSC_PROTO_CBI_INT?3:2;
    memcpy(config+n,iface,9);n+=9;
    for(i=0;i<iface[4];i++){
        UCHAR ep[]={7,USB_DT_ENDPOINT,0,USB_EP_BULK,0,2,0};
        ep[2]=proto==MSC_PROTO_UAS?(i==0?1:i==1?0x82:i==2?0x83:4):(i==0?0x81:i==1?2:0x83);
        if(proto==MSC_PROTO_CBI_INT&&i==2)ep[3]=USB_EP_INTERRUPT;
        memcpy(config+n,ep,7);n+=7;
        if(proto==MSC_PROTO_UAS){UCHAR comp[]={6,USB_DT_SS_EP_COMP,0,2,0,0},usage[]={4,USB_DT_PIPE_USAGE,0,0};
            memcpy(config+n,comp,6);n+=6;usage[2]=(UCHAR)(i+1);memcpy(config+n,usage,4);n+=4;}
    }
    config[2]=(UCHAR)n;dev.Config=config;dev.ConfigLen=n;dev.Hc=&hc;dev.Speed=(UCHAR)speed;
    m->Iface=2;m->Alt=1;m->SubClass=6;m->Proto=proto;m->Identity=ReadIdentity(&dev);
    m->Tags[0].Busy=m->Tags[0].Left=0;UasBusy=0;m->Resetting=1;
}
int main(void){unsigned variant,n,f,max_steps,base_blocks,i;UCHAR *iu[UAS_MAX_TAGS];SCSI_SLOT saved;SCSI_REQ waiting_req;
    memset(&hc,0,sizeof(hc));memset(&ops,0,sizeof(ops));hc.Ops=&ops;hc.MaxStreams=2;hc.QueueOk=1;
    for(variant=0;variant<5;variant++)for(n=0;n<100;n++){
        setup();desc(variant==0?MSC_PROTO_BOT:variant==1?MSC_PROTO_CBI:variant==2?MSC_PROTO_CBI_INT:MSC_PROTO_UAS,variant==4?USB_SPEED_SUPER:USB_SPEED_HIGH);
        step=fault=0;assert(MscRestoreDevice(&dev)==USB_OK);max_steps=step;MscEndDeviceReset(&dev,USB_OK);
        base_blocks=blocks;saved=Slots[0];for(i=0;i<UAS_MAX_TAGS;i++)iu[i]=m->Tags[i].Iu;
        for(f=0;f<=max_steps;f++){
            MSC_DATA data={0};ULONG done=99;int result;unsigned before=notifications;
            MscBeginDeviceReset(&dev);assert(m->Resetting&&!m->In.Opened&&!m->Out.Opened);
            memset(&waiting_req,0,sizeof(waiting_req));waiting_req.Complete=failed_req;m->WaitHead=m->WaitTail=&waiting_req;
            step=0;fault=f;result=MscRestoreDevice(&dev);assert(result==(f?USB_ERR_IO:USB_OK));MscEndDeviceReset(&dev,result);
            assert(!memcmp(&saved,&Slots[0],sizeof(saved))&&dev.Msc==m);
            for(i=0;i<UAS_MAX_TAGS;i++)assert(iu[i]==m->Tags[i].Iu);
            if(f){
                assert(m->Resetting&&!m->WaitHead&&!m->WaitTail&&notifications==before+1);
                assert(MscCommandD(m,0,(const UCHAR*)"\0\0\0\0\0\0",6,SCSI_DIR_NONE,0,&data,&done)==MSC_ERROR&&done==0);
                MscBeginDeviceReset(&dev);step=fault=0;assert(MscRestoreDevice(&dev)==USB_OK);MscEndDeviceReset(&dev,USB_OK);
            }else{assert(m->WaitHead==&waiting_req&&notifications==before);m->WaitHead=m->WaitTail=NULL;}
            assert(!m->Resetting&&blocks==base_blocks);
        }
        MscBeginDeviceReset(&dev);m->Identity^=1;assert(MscRestoreDevice(&dev)==USB_ERR_NODEV);MscEndDeviceReset(&dev,USB_ERR_NODEV);
        MscDetach(&dev);assert(!blocks&&!dev.Msc);
    }
    puts("RESULT msc-reset PASS: 500 BOT/CBI/CBI-interrupt/UAS-HS/UAS-SS fixtures; every alternate/endpoint/stream restore failure and retry; stable slots/IU buffers; queued request failure; identity rejection; removal after failed reset");return 0;
}
