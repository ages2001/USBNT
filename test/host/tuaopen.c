/* Full production OpenStream and reset restoration: fault every allocation,
 * clock/control write, endpoint open and initial submission. Hardware mocked. */
#define AUDIO_OPEN_TEST 1
#define main rundown_main
#include "tuacrundown.c"
#undef main
static unsigned step,fault,nqueued,last_rate,last_pin;
static SHORT last_volume;
static unsigned control_fail_entity;
static USB_XFER* queued[64];
static UCHAR ep[]={7,5,1,1,192,0,1},fb[]={7,5,0x81,1,4,0,1},me[]={7,5,2,2,64,0,1};
static UCHAR feature[]={10,0x24,6,2,1,1,3,0,0,0};
static int check(void){return ++step==fault?USB_ERR_IO:USB_OK;}
void PUT16(void*p,USHORT v){UCHAR*b=p;b[0]=(UCHAR)v;b[1]=(UCHAR)(v>>8);}
int UsbSetInterface(USB_DEV*d,UCHAR i,UCHAR a){(void)d;assert(i==1);return a?check():USB_OK;}
int UsbControl(USB_DEV*d,UCHAR rt,UCHAR req,USHORT val,USHORT idx,USHORT len,void*b,ULONG ms,ULONG*actual){UCHAR*p=b;(void)d;(void)ms;if(!(rt&USB_RT_IN)&&control_fail_entity&&(idx>>8)==control_fail_entity)return USB_ERR_IO;if(rt&USB_RT_IN){SHORT v=req==UAC1_GET_MIN?-15360:req==UAC1_GET_RES?256:0;PUT16(b,(USHORT)v);if(actual)*actual=len;return USB_OK;}if(check()!=USB_OK)return USB_ERR_IO;if((rt&3)==USB_RT_ENDPOINT){assert(len==3);last_rate=p[0]|((ULONG)p[1]<<8)|((ULONG)p[2]<<16);}else if((idx>>8)==2&&(val>>8)==2)last_volume=(SHORT)GET16(b);else if((idx>>8)==5){assert(len==1);last_pin=p[0];}return USB_OK;}
int UsbOpenPipe(USB_DEV*d,USB_PIPE*p,const UCHAR*e){int r=check();assert(!p->Opened);p->Dev=d;p->MaxPacket=GET16(e+4);p->IsoSlots=8;p->Opened=r==USB_OK;return r;}
int UsbSubmit(USB_XFER*x){unsigned i;if(check()!=USB_OK)return USB_ERR_IO;for(i=0;i<64;i++)if(!queued[i]){queued[i]=x;nqueued++;return USB_OK;}assert(0);return USB_ERR_NOMEM;}
void UsbCancelPipe(USB_PIPE*p){unsigned i;for(i=0;i<64;i++)if(queued[i]&&queued[i]->Pipe==p){USB_XFER*x=queued[i];queued[i]=NULL;nqueued--;x->Status=USB_ERR_ABORTED;x->Complete(x);}}
static void fresh(void){UA_ALT*a;memset(&dev,0,sizeof(dev));memset(UaDevs,0,sizeof(UaDevs));assert(!blocks&&!nqueued);step=fault=alloc_step=alloc_fault=0;UaClientSet=0;audio_dev=OsAlloc(sizeof(*audio_dev));audio_dev->Id=1;audio_dev->Dev=&dev;audio_dev->Uac=1;dev.Audio=UaDevs[0]=audio_dev;stream=&audio_dev->S[0];stream->U=audio_dev;audio_dev->S[1].U=audio_dev;audio_dev->S[1].Dir=1;audio_dev->NAlt[0]=1;audio_dev->Info.HasDir[0]=1;a=&audio_dev->Alts[0][0];a->Iface=1;a->FreqCtl=1;a->Ep=ep;a->Fb=fb;a->Fmt.Type=UA_FMT_PCM;a->Fmt.Alt=1;a->Fmt.Channels=2;a->Fmt.SubSize=2;a->Fmt.Bits=16;a->Fmt.NumRates=1;a->Fmt.Rates[0]=44100;}
static void open_ok(void){UA_STREAM_OPS ops;memset(&ops,0,sizeof(ops));ops.Context=(void*)0x1234;assert(OpenStream(audio_dev,0,44100,2,16,&ops,NULL)==USB_OK);assert(stream->Info.Rate==44100&&stream->Info.Alt==1&&stream->HasFb&&last_rate==44100);}
int main(void){unsigned round,k,total=0,max_alloc,max_steps,phase;UA_STREAM_OPS ops;memset(&ops,0,sizeof(ops));
 fresh();alloc_step=0;open_ok();max_alloc=alloc_step;max_steps=step;AudioDetach(&dev);assert(!blocks);
 for(round=0;round<50;round++)for(phase=0;phase<2;phase++)for(k=1;k<=(phase?max_steps:max_alloc);k++){
  int r;fresh();alloc_step=step=0;if(phase)fault=k;else alloc_fault=k;r=OpenStream(audio_dev,0,44100,2,16,&ops,NULL);assert(r!=USB_OK&&!stream->Open&&!stream->Pipe.Opened&&!stream->FbPipe.Opened&&blocks==1);alloc_fault=fault=step=0;open_ok();CloseNow(1,0);assert(blocks==1&&!stream->Open);AudioDetach(&dev);assert(!blocks);total++;
 }
 for(round=0;round<50;round++){
  unsigned baseline;fresh();open_ok();audio_dev->NEnt=1;audio_dev->Ent[0].Id=2;audio_dev->Ent[0].Kind=AC_FEATURE;audio_dev->Ent[0].D=feature;audio_dev->NFu[0]=1;audio_dev->Fu[0][0]=2;audio_dev->NSel[0]=1;audio_dev->SelUnit[0][0]=5;audio_dev->SelPin[0][0]=1;audio_dev->Info.HasVolume[0]=1;audio_dev->VolKnown[0]=1;audio_dev->VolL[0]=audio_dev->VolR[0]=-2560;audio_dev->SelKnown=1;audio_dev->Info.NumInputs=1;audio_dev->InSelUnit[0]=5;audio_dev->InSelPin[0]=2;audio_dev->Midi=OsAlloc(sizeof(UA_MIDI));audio_dev->Midi->Buf=OsAlloc(512);audio_dev->Midi->Ep=me;audio_dev->Midi->Iface=3;audio_dev->Midi->Opened=audio_dev->Midi->Pipe.Opened=1;audio_dev->Midi->Pipe.MaxPacket=64;stream->Running=1;baseline=blocks;
  AudioBeginDeviceReset(&dev);assert(audio_dev->ResetInfo[0].Rate==44100);step=fault=alloc_step=alloc_fault=0;assert(AudioRestoreDevice(&dev)==0);max_steps=step;max_alloc=alloc_step;AudioEndDeviceReset(&dev,0);assert(last_rate==44100&&last_volume==-2560&&last_pin==2&&blocks==baseline);
  for(phase=0;phase<2;phase++)for(k=1;k<=(phase?max_steps:max_alloc);k++){
   int r;AudioBeginDeviceReset(&dev);assert(audio_dev->Resetting&&!stream->Open&&!nqueued);assert(UaStart(1,0)!=0&&UaSetVolume(1,0,0,0,0)==USB_ERR_BUSY&&UaSelectInput(1,0)==USB_ERR_BUSY);step=fault=alloc_step=alloc_fault=0;if(phase)fault=k;else alloc_fault=k;r=AudioRestoreDevice(&dev);assert(r!=0);AudioEndDeviceReset(&dev,r);assert(audio_dev->Resetting&&!stream->Open&&!nqueued&&blocks==3);assert(audio_dev->ResetInfo[0].Rate==44100&&audio_dev->ResetOps[0].Context==(void*)0x1234);AudioBeginDeviceReset(&dev);fault=alloc_fault=step=alloc_step=0;assert(AudioRestoreDevice(&dev)==0);AudioEndDeviceReset(&dev,0);assert(!audio_dev->Resetting&&stream->Running&&stream->Open&&blocks==baseline&&last_rate==44100&&last_volume==-2560&&last_pin==2);total++;
  }
  AudioDetach(&dev);assert(!blocks&&!nqueued);
 }
 /* UAC2 clock-selector and programmable-frequency failure propagation. */
 {UCHAR clock_desc[]={8,0x24,0x0A,10,1,3,0,0};unsigned failure;
  for(failure=0;failure<4;failure++){
   fresh();audio_dev->Uac=2;audio_dev->NEnt=3;audio_dev->Ent[0].Id=1;audio_dev->Ent[0].Kind=AC_INPUT;audio_dev->Ent[0].Clock=20;
   audio_dev->Ent[1].Id=10;audio_dev->Ent[1].Kind=AC2_CLOCK_SOURCE;audio_dev->Ent[1].D=clock_desc;
   audio_dev->Ent[2].Id=20;audio_dev->Ent[2].Kind=AC2_CLOCK_SELECTOR;audio_dev->Ent[2].NIn=1;audio_dev->Ent[2].In[0]=10;
   stream->Alt=&audio_dev->Alts[0][0];stream->Alt->Terminal=1;control_fail_entity=failure==1?20:failure==2?10:0;if(failure==3)audio_dev->Ent[0].Clock=99;
   assert((SetRate(audio_dev,stream,44100)!=USB_OK)==(failure!=0));control_fail_entity=0;AudioDetach(&dev);assert(!blocks);
  }
 }
 printf("PASS: %u production audio open/reset allocation, control, pipe and submission failures; no leaks; same format/context/ID on retry; volume/input/MIDI restored; four UAC2 clock outcomes\n",total);return 0;}
