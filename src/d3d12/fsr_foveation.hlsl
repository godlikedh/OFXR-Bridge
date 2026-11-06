// Native FSR guides -> one uniform unjittered projection per final eye.
// Motion is current->previous UV, with endpoints transformed independently.
Texture2D<float> PeripheralDepth : register(t0);
Texture2D<float2> PeripheralMotion : register(t1);
Texture2D<float> InsetDepth : register(t2);
Texture2D<float2> InsetMotion : register(t3);
RWTexture2D<float> Depth : register(u0);
RWTexture2D<float2> Motion : register(u1);
cbuffer Params : register(b0) {
    float4 CurrentMap[2];
    float4 PreviousMap[2];
    float4 MotionScaleCorrection[2]; // normalized scale xy, jitter cancellation zw
    float4 SamplingJitter[2]; // depth UV xy, motion UV zw
    uint2 OutputSize;
    uint HasInset;
    uint Padding;
};
float2 mapped_motion(uint i,float2 sourceUV,float2 raw) {
    float2 previousUV=sourceUV+raw*MotionScaleCorrection[i].xy-MotionScaleCorrection[i].zw;
    return PreviousMap[i].xy+previousUV*PreviousMap[i].zw-
        (CurrentMap[i].xy+sourceUV*CurrentMap[i].zw);
}
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if(any(id.xy>=OutputSize))return;
    float2 uv=(float2(id.xy)+0.5)/OutputSize;
    float2 p=(uv-CurrentMap[0].xy)/CurrentMap[0].zw;
    uint dw,dh,mw,mh;
    PeripheralDepth.GetDimensions(dw,dh);PeripheralMotion.GetDimensions(mw,mh);
    int2 dp=clamp(int2(floor((p+SamplingJitter[0].xy)*float2(dw,dh))),int2(0,0),int2(dw-1,dh-1));
    int2 mp=clamp(int2(floor((p+SamplingJitter[0].zw)*float2(mw,mh))),int2(0,0),int2(mw-1,mh-1));
    float depth=PeripheralDepth.Load(int3(dp,0));
    float2 motion=mapped_motion(0,p,PeripheralMotion.Load(int3(mp,0)));
    if(HasInset) {
        float2 insetUV=(uv-CurrentMap[1].xy)/CurrentMap[1].zw;
        InsetDepth.GetDimensions(dw,dh);InsetMotion.GetDimensions(mw,mh);
        // Leave a two-texel boundary to the full-coverage periphery. Never average
        // foreground/background depth or motion across an inset edge.
        float2 guard=2.0/float2(dw,dh);
        if(all(insetUV>guard)&&all(insetUV<1.0-guard)) {
            dp=clamp(int2(floor((insetUV+SamplingJitter[1].xy)*float2(dw,dh))),int2(0,0),int2(dw-1,dh-1));
            mp=clamp(int2(floor((insetUV+SamplingJitter[1].zw)*float2(mw,mh))),int2(0,0),int2(mw-1,mh-1));
            float2 raw=InsetMotion.Load(int3(mp,0));
            float2 oldUV=insetUV+raw*MotionScaleCorrection[1].xy-MotionScaleCorrection[1].zw;
            if(all(oldUV>=0)&&all(oldUV<=1)) {
                depth=InsetDepth.Load(int3(dp,0));motion=mapped_motion(1,insetUV,raw);
            }
        }
    }
    Depth[id.xy]=depth;Motion[id.xy]=motion;
}
