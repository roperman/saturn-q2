#!/usr/bin/env python3
"""portal_estimate.py's flow with the portals as the Saturn could store them:
each a box (s16), leaf to leaf ("leafbox") or merged per pair of clusters
("cluster"), one rectangle per leaf or cluster, each portal projected once
(its 8 corners). Prints the cells kept and the projections a view.

    portal_clusters.py   (demo1 and demo2, from data/ and cd/)
"""
import sys, math
sys.path.insert(0, __import__('os').path.dirname(__file__))
import portal_estimate as pe
from q2data import Pak, Bsp
W,H,F,CX,CY,NEAR=320,224,160.0,160.0,112.0,8.0
def run(m, mapfile, mode):
    b=Bsp(Pak('data/pak0.pak').read('maps/%s.bsp'%m))
    leaves=pe.make_portals(b)
    faces,fv,nb=pe.read_baked(mapfile)
    seen=set(); cp={}
    for l in leaves:
        for p in l.portals:
            if id(p) in seen: continue
            seen.add(id(p)); a,c=p.nodes
            if a.leaf is None or c.leaf is None or a.leaf<0 or c.leaf<0 or (a.contents|c.contents)&1: continue
            if mode=='cluster':
                ka,kc=b.leafs[a.leaf]['cluster'],b.leafs[c.leaf]['cluster']
                if ka==kc or ka<0 or kc<0: continue
            else:
                ka,kc=a.leaf,c.leaf
            k=(min(ka,kc),max(ka,kc))
            lo=[math.floor(min(q[i] for q in p.w)) for i in range(3)]; hi=[math.ceil(max(q[i] for q in p.w)) for i in range(3)]
            if k in cp:
                o=cp[k]; cp[k]=([min(o[0][i],lo[i]) for i in range(3)],[max(o[1][i],hi[i]) for i in range(3)])
            else: cp[k]=(lo,hi)
    adj={}
    for (x,y),box in cp.items():
        adj.setdefault(x,[]).append((y,box)); adj.setdefault(y,[]).append((x,box))
    face_polys=[b.face_verts(i) for i in range(len(b.faces))]
    face_norm=[]
    for f in b.faces:
        n,d,_=b.planes[f['plane']]
        face_norm.append((n,d) if not f['side'] else (tuple(-x for x in n),-d))
    face_units={}
    for li,l in enumerate(b.leafs):
        u=l['cluster'] if mode=='cluster' else li
        for fi in b.leaffaces[l['firstface']:l['firstface']+l['numfaces']]:
            face_units.setdefault(fi,set()).add(u)
    tot=[0,0,0]
    print(m, mode, 'portals', len(cp))
    for vi,(x,y,z,yaw,pitch) in enumerate(pe.VIEWS[m]):
        cam=(float(x),float(y),float(z)); a=yaw/65536*2*math.pi; pt=pitch/65536*2*math.pi
        fwd=(math.cos(a)*math.cos(pt),math.sin(a)*math.cos(pt),-math.sin(pt)); right=(math.sin(a),-math.cos(a),0.0); up=(math.cos(a)*math.sin(pt),math.sin(a)*math.sin(pt),math.cos(pt))
        def proj(pts):
            vs=[(pe.dot((q[0]-cam[0],q[1]-cam[1],q[2]-cam[2]),right),pe.dot((q[0]-cam[0],q[1]-cam[1],q[2]-cam[2]),up),pe.dot((q[0]-cam[0],q[1]-cam[1],q[2]-cam[2]),fwd)) for q in pts]
            if any(v[2]<NEAR for v in vs):
                if all(v[2]<NEAR for v in vs): return None
                return 'near'
            xs=[CX+v[0]*F/v[2] for v in vs]; ys=[CY-v[1]*F/v[2] for v in vs]
            r=(max(min(xs),0),max(min(ys),0),min(max(xs),W),min(max(ys),H))
            return r if r[0]<r[2] and r[1]<r[3] else None
        def box_rect(lo,hi):
            pts=[(lo[0] if i&1 else hi[0], lo[1] if i&2 else hi[1], lo[2] if i&4 else hi[2]) for i in range(8)]
            return proj(pts)
        def inter(r,s):
            q=(max(r[0],s[0]),max(r[1],s[1]),min(r[2],s[2]),min(r[3],s[3])); return q if q[0]<q[2] and q[1]<q[3] else None
        n=b.models[0]['headnode']
        while n>=0:
            nd=b.nodes[n]; pn,pd,_=b.planes[nd[0]]; n=nd[1][0] if pe.dot(cam,pn)-pd>=0 else nd[1][1]
        cl=-1-n; cluster=b.leafs[cl]['cluster']; start=cluster if mode=='cluster' else cl
        row=pe.facevis_row(fv,cluster,nb)
        lrect={start:(0,0,W,H)}; work=[start]; q={start}; nproj=0; pr_cache={}; visits=0
        while work:
            u=work.pop(); q.discard(u); visits+=1; r=lrect[u]
            for (o,box) in adj.get(u,[]):
                key=(min(u,o),max(u,o))
                if key not in pr_cache:
                    nproj+=1; pr=box_rect(*box); pr_cache[key]=(0,0,W,H) if pr=='near' else pr
                pr=pr_cache[key]
                pr=inter(pr,r) if pr else None
                if not pr: continue
                old=lrect.get(o); nw=pr if not old else (min(old[0],pr[0]),min(old[1],pr[1]),max(old[2],pr[2]),max(old[3],pr[3]))
                if nw!=old:
                    lrect[o]=nw
                    if o not in q: q.add(o); work.append(o)
        cur=[0,0]; new=0
        for fi,(flags,cells) in enumerate(faces):
            if flags&(1|32) or not (row[fi>>3]>>(fi&7))&1: continue
            nn,dd=face_norm[fi]
            if pe.dot(cam,nn)-dd<=0: continue
            vs=face_polys[fi]
            fr=pe_rect(vs,cam,right,up,fwd)
            if not fr: continue
            cur[0]+=1; cur[1]+=cells
            if any(u in lrect and inter(fr,lrect[u]) for u in face_units.get(fi,())): new+=cells
        tot[0]+=cur[1]; tot[1]+=new
        print('  view %d: cells %d -> %d (%.0f%%), %d projections, %d visits'%(vi+1,cur[1],new,100.0*new/max(cur[1],1),nproj,visits))
    print('  all: %d -> %d (%.0f%%)'%(tot[0],tot[1],100.0*tot[1]/max(tot[0],1)))
def pe_rect(poly,cam,right,up,fwd):
    vs=[]
    for q in poly:
        d=(q[0]-cam[0],q[1]-cam[1],q[2]-cam[2]); vs.append((pe.dot(d,right),pe.dot(d,up),pe.dot(d,fwd)))
    out=[]
    for i,v in enumerate(vs):
        u=vs[(i+1)%len(vs)]
        if v[2]>=NEAR: out.append(v)
        if (v[2]>=NEAR)!=(u[2]>=NEAR):
            t=(NEAR-v[2])/(u[2]-v[2]); out.append(tuple(v[k]+t*(u[k]-v[k]) for k in range(3)))
    if not out: return None
    xs=[CX+v[0]*F/v[2] for v in out]; ys=[CY-v[1]*F/v[2] for v in out]
    r=(max(min(xs),0),max(min(ys),0),min(max(xs),W),min(max(ys),H))
    return r if r[0]<r[2] and r[1]<r[3] else None
for m in ('demo1','demo2'):
    for mode in ('leafbox','cluster'):
        run(m,'cd/%s.MAP'%m.upper(),mode)
