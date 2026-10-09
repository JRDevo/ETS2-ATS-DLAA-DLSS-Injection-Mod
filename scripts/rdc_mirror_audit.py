# RenderDoc audit of the MIRROR (secondary) G-buffer views in the ETS2/ATS frame, for the DLAA-on-mirrors plan.
#   "C:\Program Files\RenderDoc\qrenderdoc.exe" --python scripts\rdc_mirror_audit.py
# Reuses the structured-chunk scan of rdc_stencil_audit.py, but enumerates ALL 4-RTV G-buffer passes grouped by
# their depth texture (main scene = the largest; mirrors = the smaller ones), reports each mirror pass's RTV
# formats / draw counts / viewport layers / cb0 window sizes / whether the per-draw MVP is in cb0 rows 4..7, and
# replays a few mirror draws to prove SV_Position == MVP(rows4..7) * POSITION. Also lists where each mirror's
# depth is discarded and the lighting(n=2)/forward(n=1) sub-passes bound on the same mirror depth.
import os, struct, traceback, collections
CAP = os.environ.get("RDC_CAPTURE") or r"E:\ETS2-DLAA-Injector\captures\ets2_flat_frame1969.rdc"
OUT = os.environ.get("RDC_OUT") or (os.path.splitext(CAP)[0] + ".mirror_audit.txt")
f = open(OUT, "w", buffering=1)
def P(*a): f.write(" ".join(str(x) for x in a) + "\n")
try:
    import renderdoc as rd
    def topy(o, depth=0):
        bt = o.type.basetype
        if bt == rd.SDBasic.Struct: return {o.GetChild(i).name: topy(o.GetChild(i), depth+1) for i in range(o.NumChildren())}
        if bt == rd.SDBasic.Array: return [topy(o.GetChild(i), depth+1) for i in range(o.NumChildren())]
        if bt == rd.SDBasic.Resource: return int(o.AsResourceId())
        if bt == rd.SDBasic.Boolean: return o.AsBool()
        if bt == rd.SDBasic.Float: return o.AsFloat()
        if bt == rd.SDBasic.Enum: return o.AsString()
        if bt in (rd.SDBasic.UnsignedInteger, rd.SDBasic.SignedInteger): return o.AsInt()
        if bt == rd.SDBasic.Null: return None
        if bt == rd.SDBasic.Buffer: return ("BUF", o.AsInt())
        try: return o.AsString()
        except Exception: return "?"
    def args(ch): return {ch.GetChild(k).name: ch.GetChild(k) for k in range(ch.NumChildren())}
    def short(fmt): return (fmt or "").replace("DXGI_FORMAT_", "")
    DEPTH_FMTS_S = ("D32_FLOAT_S8X24_UINT", "R32G8X24_TYPELESS", "D24_UNORM_S8_UINT", "R24G8_TYPELESS")

    cap = rd.OpenCaptureFile(); cap.OpenFile(CAP, '', None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None); P("replay", res)
    sf = ctl.GetStructuredFile()
    tex={}; dsv={}; srv={}; rtv={}; dss={}; buf={}; mapdata={}; il={}
    st=dict(dss=None,ref=0,dsv=0,rtvs=[],ib=(0,None,0),vbs={},vscb0=(0,0,0),vp=None,ps_srv={},vs_srv={},cs_srv={},layout=0)
    events=[]; in_frame=False
    for ci,ch in enumerate(sf.chunks):
        n=ch.name; a=args(ch)
        if n=="Internal::Beginning of Capture": in_frame=True; continue
        if n.endswith("::CreateTexture2D") or n.endswith("::CreateTexture2D1"): tex[topy(a["pTexture"])]=topy(a.get("Descriptor") or a.get("pDesc"))
        elif n.endswith("::CreateDepthStencilView"): dsv[topy(a["pView"])]=(topy(a["pResource"]), topy(a["pDesc"]) if "pDesc" in a else None)
        elif n.endswith("::CreateShaderResourceView") or n.endswith("::CreateShaderResourceView1"): srv[topy(a["pView"])]=(topy(a["pResource"]), topy(a["pDesc"]) if "pDesc" in a else None)
        elif n.endswith("::CreateRenderTargetView") or n.endswith("::CreateRenderTargetView1"): rtv[topy(a["pView"])]=(topy(a["pResource"]), topy(a["pDesc"]) if "pDesc" in a else None)
        elif n.endswith("::CreateDepthStencilState"): dss[topy(a["pState"])]=topy(a["Descriptor"] if "Descriptor" in a else a["pDepthStencilDesc"])
        elif n.endswith("::CreateBuffer"): buf[topy(a["pBuffer"])]=topy(a["pDesc"])
        elif n.endswith("::CreateInputLayout"): il[topy(a["pInputLayout"])]=topy(a["pInputElementDescs"])
        if not in_frame: continue
        if n.endswith("::OMSetDepthStencilState"): st["dss"]=topy(a["pDepthStencilState"]); st["ref"]=topy(a["StencilRef"])
        elif n.endswith("::OMSetRenderTargets"): st["rtvs"]=[x for x in topy(a["ppRenderTargetViews"]) if x]; st["dsv"]=topy(a["pDepthStencilView"])
        elif n.endswith("::IASetIndexBuffer"): st["ib"]=(topy(a["pIndexBuffer"]), topy(a["Format"]), topy(a["Offset"]))
        elif n.endswith("::IASetVertexBuffers"):
            s0=topy(a["StartSlot"]); vbl=topy(a["ppVertexBuffers"]); strd=topy(a["pStrides"]); offs=topy(a["pOffsets"])
            for i,v in enumerate(vbl): st["vbs"][s0+i]=(v, strd[i] if i<len(strd) else 0, offs[i] if i<len(offs) else 0)
        elif n.endswith("::IASetInputLayout"): st["layout"]=topy(a["pInputLayout"])
        elif n.endswith("::VSSetConstantBuffers1") or n.endswith("::VSSetConstantBuffers"):
            if topy(a["StartSlot"])==0:
                b=topy(a["ppConstantBuffers"]); fc=topy(a["pFirstConstant"]) if "pFirstConstant" in a else [0]; nc=topy(a["pNumConstants"]) if "pNumConstants" in a else [4096]
                st["vscb0"]=(b[0] if b else 0, (fc or [0])[0] if fc else 0, (nc or [4096])[0] if nc else 4096)
        elif n.endswith("::RSSetViewports"):
            vps=topy(a["pViewports"]); st["vp"]=vps[0] if vps else None
        elif n.endswith("::Unmap") and "MapWrittenData" in a:
            bid=topy(a["pResource"]); data=b""; o=a["MapWrittenData"]
            for getter in (lambda: o.data.basic.u, lambda: o.AsInt()):
                try:
                    data=bytes(sf.buffers[int(getter())])
                    if data: break
                except Exception: pass
            start=topy(a.get("Byte offset to start of written data")) if a.get("Byte offset to start of written data") is not None else 0
            mapdata[bid]=(data,start)
        kind=None
        if n.endswith("::DrawIndexed"): kind="DI"
        elif n.endswith("::DrawIndexedInstanced"): kind="DII"
        elif n.endswith("::Draw"): kind="D"
        elif n.endswith("::ClearDepthStencilView"): kind="CLRDS"
        elif n.endswith("::DiscardView") or n.endswith("::DiscardView1") or n.endswith("::DiscardResource"): kind="DISCARD"
        if kind is None: continue
        e=dict(ci=ci,kind=kind,dss=st["dss"],ref=st["ref"],dsv=st["dsv"],rtvs=list(st["rtvs"]),ib=st["ib"],
               vb0=st["vbs"].get(0,(0,0,0)),vbs=dict(st["vbs"]),cb0=st["vscb0"],vp=st["vp"],layout=st["layout"])
        if kind=="DI": e.update(ic=topy(a["IndexCount"]),si=topy(a["StartIndexLocation"]),bv=topy(a["BaseVertexLocation"]))
        elif kind=="DII": e.update(ic=topy(a["IndexCountPerInstance"]),si=topy(a["StartIndexLocation"]),bv=topy(a["BaseVertexLocation"]),inst=topy(a["InstanceCount"]))
        elif kind=="CLRDS": e.update(cdsv=topy(a["pDepthStencilView"]),flags=topy(a["ClearFlags"]),depth=topy(a["Depth"]),stencil=topy(a["Stencil"]))
        elif kind=="DISCARD": e.update(view=topy(a["pResourceView"]) if "pResourceView" in a else topy(a.get("pResource")))
        events.append(e)

    def tex_of_dsv(v): return dsv.get(v,(0,None))[0]
    def rt_fmts(e): return tuple(short(tex.get(rtv.get(v,(0,None))[0],{}).get("Format","?")) for v in e["rtvs"])
    def tsize(t): td=tex.get(t,{}); return (td.get("Width",0),td.get("Height",0))

    # all D32S8 depth textures with sizes
    depth_tex={}
    for v,(t,d) in dsv.items():
        td=tex.get(t)
        if td and any(td["Format"].endswith(x) for x in DEPTH_FMTS_S): depth_tex[t]=tsize(t)
    scene=max(depth_tex,key=lambda t:depth_tex[t][0]*depth_tex[t][1]) if depth_tex else 0
    P("== D32S8 depth textures (size): main scene = R%d %s" % (scene, depth_tex.get(scene)))
    for t,s in sorted(depth_tex.items(), key=lambda x:-x[1][0]*x[1][1]):
        P("   R%d %dx%d %s" % (t,s[0],s[1],"<= SCENE" if t==scene else "<= mirror/secondary"))

    # segment every 4-RTV pass, keyed by the DSV texture; also track lighting(n=2)/forward(n=1) on mirror depths and discards
    # a "pass" = maximal run of draws with the same dsv-texture + same rtv count
    passes=[]; cur=None
    for e in events:
        if e["kind"]=="DISCARD":
            passes.append(dict(discard=dsv.get(e["view"],(0,None))[0], ci=e["ci"])); cur=None; continue
        if e["kind"]=="CLRDS": cur=None; continue
        dt=tex_of_dsv(e["dsv"]); nrt=len(e["rtvs"]); sig=(dt,nrt)
        if cur is None or cur["sig"]!=sig:
            cur=dict(sig=sig,dt=dt,nrt=nrt,draws=[],ci0=e["ci"]); passes.append(cur)
        cur["draws"].append(e)

    mirror_gpasses=[]
    P("\n== pass timeline (4-RTV G-buffer / 2-RTV lighting / 1-RTV forward / discards), mirror depths only + scene for reference")
    for p in passes:
        if "discard" in p:
            dt=p["discard"]
            if dt in depth_tex: P("   [discard] chunk %d  depth R%d %s" % (p["ci"], dt, depth_tex.get(dt)))
            continue
        if p["dt"] not in depth_tex: continue
        tag = "SCENE" if p["dt"]==scene else "mirror"
        if p["nrt"] in (1,2,4):
            draws=p["draws"]; vps=sorted(set((round(e["vp"]["MinDepth"],3),round(e["vp"]["MaxDepth"],3)) for e in draws if e["vp"]))
            P("   [%s n=%d] depth R%d %s  draws=%d  RTfmt=%s  vpZ=%s" % (tag,p["nrt"],p["dt"],depth_tex.get(p["dt"]),len(draws),rt_fmts(draws[0]) if draws else (),vps))
            if p["nrt"]==4 and p["dt"]!=scene: mirror_gpasses.append(p)

    # cb0 MVP readable-from-ring for mirror G-buffer draws + a replay proof
    P("\n== MIRROR G-buffer passes: cb0 window + per-draw MVP check")
    allm=[]
    for p in mirror_gpasses:
        allm += [e for e in p["draws"] if e["kind"] in ("DI","DII")]
    cbn=collections.Counter(e["cb0"][2] for e in allm)
    P("   mirror G-buffer DI/DII draws: %d across %d passes; cb0 window sizes %s" % (len(allm),len(mirror_gpasses),sorted(cbn.items())))
    # readable MVP
    vals=[]; miss=0
    for e in allm:
        b,first,num=e["cb0"]; md=mapdata.get(b)
        if not md or num*16<128: miss+=1; continue
        data,start=md; off=first*16+64-start
        if off<0 or off+64>len(data): miss+=1; continue
        vals.append((e,struct.unpack("<16f",data[off:off+64])))
    P("   MVP (rows4..7) readable from the captured ring: %d (missing %d)" % (len(vals),miss))
    for e,m in vals[:3]:
        P("     sample chunk %d |origin w|=%.1f rows: %s" % (e["ci"],abs(m[15])," | ".join(" ".join("%.3f"%x for x in m[r*4:r*4+4]) for r in range(4))))

    # replay: SV_Position == MVP*POSITION on a few mirror draws
    acts={}
    def walk(al):
        for c in al:
            acts[c.eventId]=c
            if c.children: walk(c.children)
    walk(ctl.GetRootActions())
    by_chunk={}
    for eid,ac in acts.items():
        for ev in ac.events: by_chunk[ev.chunkIndex]=eid
    picks=[e for e in allm if e["kind"]=="DI"][::max(1,len(allm)//8)][:8]
    P("\n== replay CB-layout check on %d mirror DrawIndexed: max rel |SV_Position - MVP(rows4..7)*pos|" % len(picks))
    okc=0
    for e in picks:
        eid=by_chunk.get(e["ci"])
        if eid is None: P("   chunk %d no eid"%e["ci"]); continue
        try:
            ctl.SetFrameEvent(eid,True)
            pst=ctl.GetPipelineState(); cb=pst.GetConstantBlock(rd.ShaderStage.Vertex,0,0)
            bd=cb.descriptor if hasattr(cb,"descriptor") else cb
            rid=bd.resource if hasattr(bd,"resource") else bd.resourceId
            M=struct.unpack("<32f", ctl.GetBufferData(rid,bd.byteOffset,128))[16:32]
            elems=il.get(e["layout"],[]); pe=[x for x in elems if x["SemanticName"].upper()=="POSITION"]
            if not pe: P("   eid %d no POSITION"%eid); continue
            pe=pe[0]; slot=pe["InputSlot"]; vb=e["vbs"].get(slot,(0,0,0)); fmtp=pe["Format"]
            ibid,ibf,iboff=e["ib"]; isz=2 if ibf.endswith("R16_UINT") else 4
            post=ctl.GetPostVSData(0,0,rd.MeshDataStage.VSOut)
            outb=ctl.GetBufferData(post.vertexResourceId,post.vertexByteOffset,0) if post.vertexResourceId!=rd.ResourceId.Null() else b""
            oidx=None
            if post.indexResourceId!=rd.ResourceId.Null() and post.indexByteStride:
                ob=ctl.GetBufferData(post.indexResourceId,post.indexByteOffset,8*post.indexByteStride)
                oidx=struct.unpack("<8"+("H" if post.indexByteStride==2 else "I"),ob[:8*post.indexByteStride])
            d3=ctl.GetD3D11PipelineState(); vbr=d3.inputAssembly.vertexBuffers[slot]
            vrid=vbr.resourceId if hasattr(vbr,"resourceId") else vbr.resource
            ibr=d3.inputAssembly.indexBuffer; irid=ibr.resourceId if hasattr(ibr,"resourceId") else ibr.resource
            ids_=struct.unpack("<8%s"%("H" if isz==2 else "I"), ctl.GetBufferData(irid,iboff+e["si"]*isz,8*isz))
            worst=0.0; nchk=0
            for k in range(8):
                vi=ids_[k]+e["bv"]; raw_v=ctl.GetBufferData(vrid,vb[2]+vi*vb[1]+pe["AlignedByteOffset"],16)
                if fmtp.endswith("R32G32B32_FLOAT") or fmtp.endswith("R32G32B32A32_FLOAT"): x,y,z=struct.unpack("<3f",raw_v[:12])
                elif fmtp.endswith("R16G16B16A16_FLOAT"): x,y,z=struct.unpack("<3e",raw_v[:6])
                else: P("   eid %d POSITION fmt %s"%(eid,fmtp)); break
                pos=(x,y,z,1.0); clip=[sum(M[r*4+c]*pos[c] for c in range(4)) for r in range(4)]
                ok_=oidx[k] if oidx else k; ob0=ok_*post.vertexByteStride
                if ob0+16>len(outb): break
                o=struct.unpack("<4f",outb[ob0:ob0+16]); worst=max(worst,max(abs(clip[i]-o[i])/max(1.0,abs(o[3])) for i in range(4))); nchk+=1
            if nchk:
                okc+=1 if worst<1e-3 else 0
                P("   eid %d ic=%d vp.min=%.2f POS %s: %d verts worst=%.2e %s" % (eid,e["ic"],e["vp"]["MinDepth"] if e["vp"] else -1,short(fmtp),nchk,worst,"OK" if worst<1e-3 else "MISMATCH"))
        except Exception:
            P("   eid %s: %s" % (eid, traceback.format_exc().splitlines()[-1]))
    P("   mirror layout check OK on %d / %d" % (okc,len(picks)))
    P("\nDONE")
except Exception:
    P("FATAL", traceback.format_exc())
