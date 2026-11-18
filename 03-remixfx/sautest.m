#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <stdio.h>

void* sau_load(const char* dylib, const char* fixups);
static OSType FOUR(const char*s){return (s[0]<<24)|(s[1]<<16)|(s[2]<<8)|s[3];}

int main(int argc,char**argv){
    setbuf(stdout,NULL); setbuf(stderr,NULL);
    void (*reg)(void) = (void(*)(void)) sau_load(argv[1], argv[2]);
    if(!reg){ fprintf(stderr,"sau_load failed\n"); return 1; }
    printf("calling registerSonicAudioUnits...\n");
    reg();
    printf("registerSonicAudioUnits returned OK\n");

    AudioComponentDescription d = { FOUR("aufx"), FOUR("remx"), FOUR("appl"), 0, 0 };
    AudioComponent c = AudioComponentFindNext(NULL,&d);
    printf("AudioComponentFindNext(aufx/remx/appl) = %p\n", c);
    if(!c) return 3;
    CFStringRef nm=NULL; AudioComponentCopyName(c,&nm);
    printf("component name: %s\n", nm?[(__bridge NSString*)nm UTF8String]:"(none)");

    AudioUnit au=NULL; OSStatus st=AudioComponentInstanceNew(c,&au);
    printf("AudioComponentInstanceNew = %d  au=%p\n",(int)st,au);
    if(st) return 4;

    AudioStreamBasicDescription f={0};
    f.mSampleRate=48000; f.mFormatID=kAudioFormatLinearPCM;
    f.mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame=2; f.mBitsPerChannel=32; f.mFramesPerPacket=1; f.mBytesPerFrame=8; f.mBytesPerPacket=8;
    OSStatus e1=AudioUnitSetProperty(au,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&f,sizeof(f));
    OSStatus e2=AudioUnitSetProperty(au,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&f,sizeof(f));
    printf("set stream format in/out = %d/%d\n",(int)e1,(int)e2);
    st=AudioUnitInitialize(au);
    printf("AudioUnitInitialize = %d\n",(int)st);

    UInt32 sz=0; AudioUnitGetPropertyInfo(au,kAudioUnitProperty_ParameterList,kAudioUnitScope_Global,0,&sz,NULL);
    int n=sz/sizeof(AudioUnitParameterID);
    printf("PARAMETER COUNT = %d\n",n);
    if(n>0 && n<256){
        AudioUnitParameterID ids[256]; AudioUnitGetProperty(au,kAudioUnitProperty_ParameterList,kAudioUnitScope_Global,0,ids,&sz);
        printf("%-5s %-30s %12s %12s %12s\n","id","name","min","max","default");
        for(int i=0;i<n;i++){
            AudioUnitParameterInfo pi; UInt32 s2=sizeof(pi);
            if(AudioUnitGetProperty(au,kAudioUnitProperty_ParameterInfo,kAudioUnitScope_Global,ids[i],&pi,&s2)) continue;
            char name[128]=""; if((pi.flags&kAudioUnitParameterFlag_HasCFNameString)&&pi.cfNameString) CFStringGetCString(pi.cfNameString,name,128,kCFStringEncodingUTF8); else strncpy(name,pi.name,127);
            printf("%-5u %-30s %12.3f %12.3f %12.3f\n",ids[i],name,pi.minValue,pi.maxValue,pi.defaultValue);
        }
    }

    // ---- render test ----
    AudioComponentInstanceDispose(au); AudioComponentInstanceNew(c,&au);
    AudioStreamBasicDescription nf={0};
    nf.mSampleRate=48000; nf.mFormatID=kAudioFormatLinearPCM;
    nf.mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked|kAudioFormatFlagIsNonInterleaved;
    nf.mChannelsPerFrame=2; nf.mBitsPerChannel=32; nf.mFramesPerPacket=1; nf.mBytesPerFrame=4; nf.mBytesPerPacket=4;
    OSStatus fi=AudioUnitSetProperty(au,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&nf,sizeof(nf));
    OSStatus fo=AudioUnitSetProperty(au,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&nf,sizeof(nf));
    printf("\n[render] set noninterleaved float in/out = %d/%d\n",(int)fi,(int)fo);
    extern OSStatus feed(void*,AudioUnitRenderActionFlags*,const AudioTimeStamp*,UInt32,UInt32,AudioBufferList*);
    AURenderCallbackStruct cb; cb.inputProc=feed; cb.inputProcRefCon=NULL;
    AudioUnitSetProperty(au,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&cb,sizeof(cb));
    AudioUnitInitialize(au);
    AudioUnitSetParameter(au,0,kAudioUnitScope_Global,0,128.0,0);
    AudioUnitSetParameter(au,7,kAudioUnitScope_Global,0,1.0,0);
    AudioUnitSetParameter(au,8,kAudioUnitScope_Global,0,4.0,0);
    UInt32 N=512; float L[512],R[512];
    AudioBufferList*abl=malloc(sizeof(AudioBufferList)+sizeof(AudioBuffer));
    abl->mNumberBuffers=2;
    abl->mBuffers[0].mNumberChannels=1; abl->mBuffers[0].mDataByteSize=N*4; abl->mBuffers[0].mData=L;
    abl->mBuffers[1].mNumberChannels=1; abl->mBuffers[1].mDataByteSize=N*4; abl->mBuffers[1].mData=R;
    AudioUnitRenderActionFlags flags=0; AudioTimeStamp ts={0}; ts.mFlags=kAudioTimeStampSampleTimeValid;
    OSStatus rs=noErr; double energy=0;
    for(int blk=0;blk<64 && rs==noErr;blk++){ ts.mSampleTime=blk*N; rs=AudioUnitRender(au,&flags,&ts,0,N,abl); for(UInt32 i=0;i<N;i++) energy+=L[i]*L[i]+R[i]*R[i]; }
    printf("[render] AudioUnitRender status=%d output energy=%.3f %s\n",(int)rs,energy,(rs==0&&energy>0)?"*** DSP PROCESSES AUDIO ***":"(check)");
    return 0;
}
OSStatus feed(void*r,AudioUnitRenderActionFlags*fl,const AudioTimeStamp*ts,UInt32 bn,UInt32 n,AudioBufferList*io){
    static double ph=0;
    for(UInt32 b=0;b<io->mNumberBuffers;b++){ float*p=(float*)io->mBuffers[b].mData; for(UInt32 i=0;i<n;i++) p[i]=0.25f*sinf(ph+i*0.06); }
    ph+=n*0.06; return noErr;
}
