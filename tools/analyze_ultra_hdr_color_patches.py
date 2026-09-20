"""Independent numerical PNG oracle and patch report; does not edit images.

Requires NumPy. prepare: input PNG + new output directory.
compare: prepared directory + probe result directory (adds new CSV/JSON only).
The probe consumes linear_p3.f32. Reference is raw PQ RGB, not ImageIO rendering.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct
import zlib
import numpy as np


def png16(path):
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, chunks, tags = 8, [], {}
    while pos < len(data):
        n = struct.unpack_from(">I", data, pos)[0]
        tag, payload = data[pos+4:pos+8], data[pos+8:pos+8+n]
        assert zlib.crc32(tag+payload) == struct.unpack_from(">I", data, pos+8+n)[0]
        tags[tag] = payload
        if tag == b"IDAT": chunks.append(payload)
        pos += n+12
    w,h,depth,color,compression,filter_method,interlace = struct.unpack(">IIBBBBB",tags[b"IHDR"])
    assert (depth,color,compression,filter_method,interlace) == (16,2,0,0,0)
    assert tags[b"cICP"] == bytes([9,16,0,1]), "expected full-range RGB BT.2020/PQ"
    raw = np.frombuffer(zlib.decompress(b"".join(chunks)),np.uint8).reshape(h,w*6+1)
    out = np.zeros((h,w*6),np.uint8)
    for y in range(h):
        f, row = int(raw[y,0]),raw[y,1:].copy()
        previous = out[y-1] if y else np.zeros(w*6,np.uint8)
        if f == 1:
            row = np.cumsum(row.reshape(-1,6),axis=0,dtype=np.uint64).astype(np.uint8).ravel()
        elif f == 2: row += previous
        elif f in (3,4):
            for x in range(w*6):
                a,b,c = (int(row[x-6]) if x>=6 else 0),int(previous[x]),(int(previous[x-6]) if x>=6 else 0)
                if f == 3: predictor=(a+b)//2
                else:
                    p=a+b-c; pa,pb,pc=abs(p-a),abs(p-b),abs(p-c)
                    predictor=a if pa<=pb and pa<=pc else b if pb<=pc else c
                row[x]=(int(row[x])+predictor)%256
        else: assert f==0
        out[y]=row
    return out.tobytes(),w,h,hashlib.sha256(data).hexdigest()


def rgb_xyz(primaries):
    p=np.array(primaries,dtype=np.float64)
    unscaled=np.array([p[:,0]/p[:,1],np.ones(3),(1-p[:,0]-p[:,1])/p[:,1]])
    white=np.array([.3127/.3290,1.,(1-.3127-.3290)/.3290])
    return unscaled*np.linalg.solve(unscaled,white)


def regions(rgb):
    # Exact constant-code rectangles, not hand-picked favorable sample points.
    # Exclude the image's background code and regions too narrow for an 8px inset.
    background=tuple(int(v) for v in rgb[0,0])
    active,found={},[]
    for y,row in enumerate(rgb):
        cuts=np.r_[0,np.nonzero(np.any(row[1:]!=row[:-1],axis=1))[0]+1,len(row)]
        current={}
        for x0,x1 in zip(cuts[:-1],cuts[1:]):
            color=tuple(int(v) for v in row[x0])
            if x1-x0<20 or color==background: continue
            key=(int(x0),int(x1),color)
            current[key]=active.pop(key,(y,y))[:1]+(y+1,)
        for key,(y0,y1) in active.items():
            if y1-y0>=20: found.append([key[0],y0,key[1],y1])
        active=current
    for key,(y0,y1) in active.items():
        if y1-y0>=20: found.append([key[0],y0,key[1],y1])
    return sorted(found,key=lambda r:(r[1],r[0]))


P3_XYZ=rgb_xyz([[.68,.32],[.265,.69],[.15,.06]])
M=np.linalg.solve(P3_XYZ,rgb_xyz([[.708,.292],[.170,.797],[.131,.046]]))


def prepare(path,out):
    raw,w,h,sha=png16(path)
    code=np.frombuffer(raw,">u2").reshape(h,w,3)
    v=(code.astype(np.float64)/65535)**(32/2523)
    # Independent ST 2084 EOTF, absolute nits -> fixed UHDR 203 nit reference.
    linear2020=10000*(np.maximum(v-3424/4096,0)/(2413/128-2392/128*v))**(16384/2610)/203
    reference=linear2020 @ M.T
    rects=regions(code)
    out.mkdir(parents=True,exist_ok=False)
    reference.astype("<f4").tofile(out/"linear_p3.f32")
    info=dict(source=str(path),sha256=sha,width=w,height=h,reference_white_nits=203,
              source_cicp=[9,16,0,1],bt2020_to_p3=M.tolist(),regions=rects,
              min_component=float(reference.min()),max_component=float(reference.max()),
              negative_channels=int((reference<0).sum()),above_10000_channels=int((reference>10000/203).sum()))
    (out/"reference.json").write_text(json.dumps(info,indent=2)+"\n")
    print(json.dumps({k:v for k,v in info.items() if k!='regions'},indent=2))
    print("regions",len(rects),rects)


def compare(prepared,result):
    info=json.loads((prepared/"reference.json").read_text()); w,h=info['width'],info['height']
    ref=np.fromfile(prepared/"linear_p3.f32",'<f4').reshape(h,w,3).astype(np.float64)
    inp=np.fromfile(result/"codec_input.f32",'<f4').reshape(h,w,3).astype(np.float64)
    rects=info['regions']; rows=[]; mask=np.zeros((h,w),bool)
    for r in rects:
        x0,y0,x1,y1=r; mask[y0+8:y1-8,x0+8:x1-8]=True
    summary={}
    for q in [100,95,85]:
        dec=np.fromfile(result/f'q{q}.f32','<f4').reshape(h,w,3).astype(np.float64)
        denom=np.maximum(.05,np.max(ref,axis=2))
        rgb_error=np.max(abs(dec-ref),axis=2)/denom
        codec_error=np.max(abs(dec-inp),axis=2)/np.maximum(.05,inp.max(axis=2))
        for i,r in enumerate(rects):
            x0,y0,x1,y1=r; sl=np.s_[y0+8:y1-8,x0+8:x1-8]
            a=ref[sl].mean(axis=(0,1)); b=dec[sl].mean(axis=(0,1)); c=inp[sl].mean(axis=(0,1))
            ya,yb=a@P3_XYZ[1],b@P3_XYZ[1]
            row=dict(quality=q,patch=i+1,x0=x0,y0=y0,x1=x1,y1=y1,pixels=int(rgb_error[sl].size),
                **{f'ref_{ch}':float(a[j]) for j,ch in enumerate('RGB')},
                **{f'dec_{ch}':float(b[j]) for j,ch in enumerate('RGB')},
                **{f'delta_{ch}':float(b[j]-a[j]) for j,ch in enumerate('RGB')},
                ref_Y_nits=float(ya*203),dec_Y_nits=float(yb*203),
                Y_error_percent=float((yb-ya)/max(.05,ya)*100),
                mean_rgb_error_percent=float(rgb_error[sl].mean()*100),
                max_rgb_error_percent=float(rgb_error[sl].max()*100),
                max_codec_error_percent=float(codec_error[sl].max()*100),
                input_error_percent=float(np.max(abs(c-a))/max(.05,a.max())*100))
            rows.append(row)
        summary[str(q)]={'bytes':(result/f'q{q}.jpg').stat().st_size}
        for name,m in [('all',np.ones((h,w),bool)),('patch_interiors',mask),('outside_patch_interiors',~mask)]:
            e=rgb_error[m]; worst=np.where(m,rgb_error,-1).argmax(); y,x=divmod(int(worst),w)
            summary[str(q)][name]=dict(pixels=int(e.size),mean_percent=float(e.mean()*100),
                p95_percent=float(np.percentile(e,95)*100),max_percent=float(e.max()*100),worst_xy=[x,y],
                worst_ref=ref[y,x].tolist(),worst_dec=dec[y,x].tolist())
    target=result/'patches.csv'; assert not target.exists()
    with target.open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=rows[0].keys()); writer.writeheader(); writer.writerows(rows)
    (result/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('mode',choices=['prepare','compare']); p.add_argument('input',type=Path); p.add_argument('output',type=Path)
    a=p.parse_args(); (prepare if a.mode=='prepare' else compare)(a.input,a.output)
