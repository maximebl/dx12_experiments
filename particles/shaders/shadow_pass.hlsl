#include "common.hlsl"

struct vertex_out
{
    float4 posh : SV_Position;
};

#ifdef VERTEX_SHADER
ConstantBuffer<object_data_vs> object_cb_vs : register(b1, space1);

vertex_out vs_main(float3 pos : POSITION)
{
    vertex_out vout;
    vout.posh = mul(float4(pos, 1.f), object_cb_vs.mvp);
    return vout;
}
#endif
