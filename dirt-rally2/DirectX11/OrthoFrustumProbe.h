#pragma once
#include <cmath>
#include <cstring>

// Diagnostic032 only. Scale an orthographic box about its plane-derived centre,
// preserving all six normals, plane order and eight corner identities.
// Perspective/degenerate/inconsistent volumes are rejected without writing.
inline bool tt_scale_ortho_frustum(float *p, float factor, float *span_out = nullptr)
{
    if (!(factor >= 1 && factor <= 8)) return false;
    float centre[3] = {};
    float extent[6], span=0;
    for (int i=0; i<6; ++i) {
        const float *n=p+i*4;
        float len=n[0]*n[0]+n[1]*n[1]+n[2]*n[2];
        if (!(std::fabs(len-1)<0.002f)) return false;
        if (!(std::fabs(n[3])<1e7f)) return false;
    }
    for (int i=0; i<6; i+=2) {
        float sum=0;
        for(int a=0; a<3; ++a) {float v=p[i*4+a]+p[(i+1)*4+a];sum+=v*v;}
        if (!(sum<1e-7f)) return false;
        extent[i]=extent[i+1]=(p[i*4+3]+p[(i+1)*4+3])*0.5f;
        if (!(extent[i]>0.001f && extent[i]<1e6f)) return false;
        if(extent[i]>span) span=extent[i];
        // Orthographic camera axes must be orthonormal, not sheared.
        for(int j=0;j<i;j+=2) {
            float dot=0;for(int a=0;a<3;++a)dot+=p[i*4+a]*p[j*4+a];
            if(!(std::fabs(dot)<0.0001f)) return false;
        }
        float midpoint=(p[(i+1)*4+3]-p[i*4+3])*0.5f;
        for(int a=0;a<3;++a) centre[a]+=midpoint*p[i*4+a];
    }
    // Native BuildFrustum uses clip z=-1/+1 for corners, but z=0/+1
    // for planes. Thus near corners may be THREE half-extents from centre.
    // Preserve that convention; do not silently regenerate different corners.
    float tolerance=0.02f+span*0.0001f;
    for(int c=0;c<8;++c) {
        for(int i=0;i<6;i+=2) {
            const float *n=p+i*4, *v=p+24+c*4;
            for(int a=0;a<3;++a) if(!(std::fabs(v[a])<1e7f)) return false;
            float d=n[3]+n[0]*v[0]+n[1]*v[1]+n[2]*v[2];
            bool edge=std::fabs(d)<=tolerance || std::fabs(d-2*extent[i])<=tolerance;
            if(i==0) edge=edge || std::fabs(d+2*extent[i])<=tolerance;
            if(!edge) return false;
        }
    }
    if(span_out) *span_out=span;
    if(factor==1) return true;
    float updated[56];std::memcpy(updated,p,sizeof updated);
    for(int i=0;i<6;++i) updated[i*4+3] += (factor-1)*extent[i];
    for(int c=0;c<8;++c) for(int a=0;a<3;++a)
        updated[24+c*4+a]=centre[a]+factor*(p[24+c*4+a]-centre[a]);
    std::memcpy(p,updated,sizeof updated);
    return true;
}
