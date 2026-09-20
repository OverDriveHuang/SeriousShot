"""Offline display-referred Delta E ITP (ITU-R BT.2124 Annex 1).

Usage: python analyze_ultra_hdr_itp.py reference_dir probe_dir
Uses existing reference/probe float files; writes itp_patches.csv/itp_summary.json.
1 linear P3 unit = 203 cd/m2. No tone mapping, peak normalization, or RGB clamp.
"""
import csv
import json
from pathlib import Path
import sys
import numpy as np
from analyze_ultra_hdr_color_patches import M

RGB_TO_LMS=np.array([[1688,2146,262],[683,2951,462],[99,309,3688]],dtype=np.float64)/4096
LMS_PQ_TO_ITP=np.array([[2048,2048,0],[6610/2,-13613/2,7003/2],[17933,-17390,-543]],dtype=np.float64)/4096
XYZ_TO_2020=np.array([[1.716651187971268,-.355670783776392,-.253366281373660],
    [-.666684351832489,1.616481236634939,.015768545813911],
    [.017639857445311,-.042770613257809,.942103121235474]])


def itp_from_2020_nits(rgb):
    # BT.2124 permits out-of-gamut RGB; do not clamp negative RGB before LMS.
    lms=np.asarray(rgb,dtype=np.float64) @ RGB_TO_LMS.T
    if not np.isfinite(lms).all() or lms.min()<0:
        raise ValueError('PQ metric input has invalid/negative LMS; no silent clipping')
    y=(lms/10000)**(2610/16384)
    pq=((3424/4096+(2413/128)*y)/(1+(2392/128)*y))**(2523/32)
    # Values above 10000 are evaluated analytically, but recorded as outside
    # the nominal reference volume; do not interpret those as validated JNDs.
    return pq @ LMS_PQ_TO_ITP.T, dict(min_lms_nits=float(lms.min()),
        max_lms_nits=float(lms.max()),above_10000_lms_components=int((lms>10000).sum()))


def delta(a,b): return 720*np.linalg.norm(a-b,axis=-1)


def self_test():
    # Published BT.2124 Annex 4 example, not an oracle from production code.
    code=np.array([296.,201.,582.])/1023
    v=code**(32/2523)
    rgb=10000*(np.maximum(v-3424/4096,0)/(2413/128-2392/128*v))**(16384/2610)
    a,_=itp_from_2020_nits(rgb)
    b,_=itp_from_2020_nits(XYZ_TO_2020 @ np.array([36.,15.,190.]))
    # The informative example's first printed I=.3554 does not reproduce
    # from its [296,201,582] codes with normative Annex 1 (I=.35572053).
    # Do not change the normative math to fit that intermediate printed value.
    np.testing.assert_allclose(a,[.35572053,.134647,-.161395],atol=.000001,rtol=0)
    np.testing.assert_allclose(b,[.3568,.1321,-.1629],atol=.0001,rtol=0)
    assert abs(float(delta(np.array([.3554,.1346,-.1613]),
                          np.array([.3568,.1321,-.1629])))-2.363)<.001
    np.testing.assert_allclose(delta(a,a),0,atol=0)
    np.testing.assert_allclose(delta(a,a+[1/720,0,0]),1,atol=1e-12)
    np.testing.assert_allclose(delta(a,b),delta(b,a),atol=1e-12)
    white,_=itp_from_2020_nits([10000,10000,10000])
    np.testing.assert_allclose(white,[1,0,0],atol=1e-12)
    diffuse,_=itp_from_2020_nits([203,203,203])
    np.testing.assert_allclose(diffuse,[.58068888104161,0,0],atol=1e-12)
    print('BT.2124 normative conversion/invariant self-tests passed;',
          'Annex 4 input-code recomputation =',float(delta(a,b)),
          '(the rounded printed ITP pair gives 2.363; see source note)')


def stats(e,mask):
    selected=e[mask]; at=int(np.where(mask,e,-1).argmax()); y,x=divmod(at,e.shape[1])
    return dict(pixels=int(selected.size),mean=float(selected.mean()),
        median=float(np.median(selected)),p95=float(np.percentile(selected,95)),
        p99=float(np.percentile(selected,99)),maximum=float(selected.max()),
        above_1_percent=float((selected>1).mean()*100),above_2_percent=float((selected>2).mean()*100),
        above_3_percent=float((selected>3).mean()*100),worst_xy=[x,y])


def compare(prepared,result):
    info=json.loads((prepared/'reference.json').read_text()); w,h=info['width'],info['height']
    assert info['reference_white_nits']==203
    ref=np.fromfile(prepared/'linear_p3.f32','<f4').reshape(h,w,3).astype(np.float64)
    p3_to_2020=np.linalg.inv(M)
    ref_itp,ref_range=itp_from_2020_nits((ref @ p3_to_2020.T)*203)
    mask=np.zeros((h,w),bool)
    for x0,y0,x1,y1 in info['regions']: mask[y0+8:y1-8,x0+8:x1-8]=True
    summary=dict(metric='ITU-R BT.2124-0 Delta E ITP',reference_white_nits=203,
        source_sha256=info['sha256'],reference_lms_range=ref_range,quality={})
    rows=[]
    for q in [100,95,85]:
        dec=np.fromfile(result/f'q{q}.f32','<f4').reshape(h,w,3).astype(np.float64)
        dec_itp,dec_range=itp_from_2020_nits((dec @ p3_to_2020.T)*203)
        e=delta(ref_itp,dec_itp)
        summary['quality'][str(q)]=dict(decoded_lms_range=dec_range,
            all=stats(e,np.ones((h,w),bool)),patch_interiors=stats(e,mask),
            outside_patch_interiors=stats(e,~mask))
        for i,(x0,y0,x1,y1) in enumerate(info['regions']):
            a=e[y0+8:y1-8,x0+8:x1-8]
            rows.append(dict(quality=q,patch=i+1,x0=x0,y0=y0,x1=x1,y1=y1,
                pixels=int(a.size),mean=float(a.mean()),p95=float(np.percentile(a,95)),maximum=float(a.max()),
                above_1_percent=float((a>1).mean()*100),above_2_percent=float((a>2).mean()*100),
                above_3_percent=float((a>3).mean()*100)))
    output=result/'itp_patches.csv'; assert not output.exists()
    with output.open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=rows[0].keys()); writer.writeheader(); writer.writerows(rows)
    (result/'itp_summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary,indent=2))


if __name__=='__main__':
    self_test()
    if len(sys.argv)==3: compare(Path(sys.argv[1]),Path(sys.argv[2]))
    elif len(sys.argv)!=1: raise SystemExit('usage: script [reference_dir probe_dir]')
