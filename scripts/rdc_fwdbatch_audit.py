# RenderDoc audit for the ETS2 / ATS DLAA injector v0.10.0 phase 10: can the forward-depth re-draws be BATCHED at the end of
# the forward pass (replayed from a record, with the game's own pixel shader for the alpha-to-coverage), and what would it
# take to let the game render into OUR depth texture (job C)?
#   "C:\Program Files\RenderDoc\qrenderdoc.exe" --python scripts\rdc_fwdbatch_audit.py
# Environment: RDC_CAPTURE (default captures\ets2_flat_frame1969.rdc), RDC_OUT (default <capture>.fwdbatch_audit.txt),
#              RDC_REPLAY=0 skips the replay part (GPU durations, reflection).
# Answers:
#   1. the "over" set (the r5 qualifying forward draws: depth test without write, RT0 blend DestBlend INV_SRC_ALPHA, world
#      viewport [0.005, 0.95]): which PS state they read -- PS cbuffer slots (buffer, first, count, set via *1), PS SRVs, samplers
#   2. every Map / UpdateSubresource / Copy between the first over draw and the end of the forward pass that writes a resource a
#      STILL-PENDING over draw reads (VS / PS cbuffers, VBs, IB, VS / PS SRV resources) -- a batched replay at the pass end would
#      read the new contents: WRITE_DISCARD can be caught by hkMap (forced replay), UpdateSubresource / NO_OVERWRITE-in-place can not
#   3. replay: GPU duration of every forward draw (EventGPUDuration counter), the over set's share; PS reflection of the over set
#   4. job C: every chunk of the whole capture that names the scene depth texture or one of its views
import os, sys, struct, traceback, collections

def _default_capture():
    bases = []
    if "__file__" in globals():
        bases.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    bases += [os.getcwd(), os.path.dirname(os.getcwd()), r"E:\ETS2-DLAA-Injector"]
    for b in bases:
        c = os.path.join(b, "captures", "ets2_flat_frame1969.rdc")
        if os.path.isfile(c):
            return c
    return os.path.join(bases[0], "captures", "ets2_flat_frame1969.rdc")
CAP = os.environ.get("RDC_CAPTURE") or _default_capture()
OUT = os.environ.get("RDC_OUT") or (os.path.splitext(CAP)[0] + ".fwdbatch_audit.txt")
REPLAY = os.environ.get("RDC_REPLAY", "1") != "0"
f = open(OUT, "w", buffering=1)
def P(*a): f.write(" ".join(str(x) for x in a) + "\n")
DEPTH_FMTS_S = ("D32_FLOAT_S8X24_UINT", "R32G8X24_TYPELESS", "D24_UNORM_S8_UINT", "R24G8_TYPELESS")

try:
    import renderdoc as rd
    def topy(o):
        bt = o.type.basetype
        if bt == rd.SDBasic.Struct: return {o.GetChild(i).name: topy(o.GetChild(i)) for i in range(o.NumChildren())}
        if bt == rd.SDBasic.Array: return [topy(o.GetChild(i)) for i in range(o.NumChildren())]
        if bt == rd.SDBasic.Resource: return int(o.AsResourceId())
        if bt == rd.SDBasic.Boolean: return o.AsBool()
        if bt == rd.SDBasic.Float: return o.AsFloat()
        if bt == rd.SDBasic.Enum: return o.AsString()
        if bt in (rd.SDBasic.UnsignedInteger, rd.SDBasic.SignedInteger): return o.AsInt()
        if bt == rd.SDBasic.Null: return None
        try: return o.AsString()
        except Exception: return "?"
    def args(ch): return {ch.GetChild(k).name: ch.GetChild(k) for k in range(ch.NumChildren())}
    def first(a, *names):
        for nm in names:
            if nm in a: return a[nm]
        return None
    def short(x): return (x or "").replace("DXGI_FORMAT_", "").replace("D3D11_", "")

    cap = rd.OpenCaptureFile(); r = cap.OpenFile(CAP, '', None)
    P("capture", CAP, "open", r)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None); P("replay", res)
    sf = ctl.GetStructuredFile()

    tex = {}; buf = {}; view = {}; vdesc = {}; dssd = {}; bsd = {}
    for ci, ch in enumerate(sf.chunks):
        n = ch.name; a = args(ch)
        if n.endswith("::CreateTexture2D") or n.endswith("::CreateTexture2D1"):
            tex[topy(a["pTexture"])] = topy(first(a, "Descriptor", "pDesc"))
        elif n.endswith("::CreateBuffer"):
            buf[topy(a["pBuffer"])] = topy(a["pDesc"])
        elif ("::Create" in n) and "pView" in a and "pResource" in a:
            kind = "dsv" if "DepthStencil" in n else ("rtv" if "RenderTarget" in n else ("uav" if "Unordered" in n else "srv"))
            view[topy(a["pView"])] = (kind, topy(a["pResource"]))
            vdesc[topy(a["pView"])] = topy(a["pDesc"]) if "pDesc" in a else None
        elif n.endswith("::CreateDepthStencilState"):
            dssd[topy(a["pState"])] = topy(first(a, "Descriptor", "pDepthStencilDesc"))
        elif n.endswith("::CreateBlendState") or n.endswith("::CreateBlendState1"):
            k_ = first(a, "pState", "pBlendState", "ppBlendState"); d_ = first(a, "Descriptor", "pBlendStateDesc", "pDesc")
            if k_ is not None and d_ is not None: bsd[topy(k_)] = topy(d_)
    def res_of(v): return view.get(v, (None, 0))[1]
    cands = {}
    for v, (k, t) in view.items():
        td = tex.get(t)
        if k == "dsv" and td and any(str(td.get("Format", "")).endswith(x) for x in DEPTH_FMTS_S):
            cands[t] = td["Width"] * td["Height"]
    scene = max(cands, key=lambda t: cands[t]) if cands else 0
    sd = tex.get(scene, {})
    SW, SH = sd.get("Width", 0), sd.get("Height", 0)
    P("scene depth R%d %dx%d %s BindFlags=%s" % (scene, SW, SH, short(sd.get("Format")), sd.get("BindFlags")))

    # ---- job C: every chunk naming the scene depth texture or one of its views -----------------------------------------
    sviews = set(v for v, (k, t) in view.items() if t == scene)
    P("\n== (4) job C: views of the scene depth: %s" % sorted((v, view[v][0], vdesc.get(v)) for v in sviews))
    touch = collections.Counter(); touchFirst = {}
    in_frame = False
    for ci, ch in enumerate(sf.chunks):
        n = ch.name
        if n == "Internal::Beginning of Capture": in_frame = True
        a = args(ch)
        hit = False
        for k, v in a.items():
            try:
                val = topy(v)
            except Exception:
                continue
            vals = val if isinstance(val, list) else [val]
            for x in vals:
                if isinstance(x, int) and x and (x == scene or x in sviews): hit = True
        if hit:
            key = ("frame" if in_frame else "init") + " " + n.split("::")[-1]
            touch[key] += 1
            touchFirst.setdefault(key, ci)
    for k, c in sorted(touch.items(), key=lambda kv: touchFirst[kv[0]]):
        P("   %-48s x%-4d first chunk %d" % (k, c, touchFirst[k]))

    # ---- state walk ----------------------------------------------------------------------------------------------------
    st = dict(dss=0, ref=0, bs=0, rs=0, dsv=0, rtvs=[], ib=0, vbs={}, vs=0, ps=0, vscb={}, pscb={}, vssrv={}, pssrv={},
              pssmp={}, vps=[])
    draws = []; writes = []
    in_frame = False
    for ci, ch in enumerate(sf.chunks):
        n = ch.name; a = args(ch)
        if n == "Internal::Beginning of Capture": in_frame = True; continue
        if not in_frame: continue
        nm = n.split("::")[-1]
        if nm == "OMSetDepthStencilState": st["dss"] = topy(a["pDepthStencilState"]); st["ref"] = topy(a["StencilRef"])
        elif nm == "OMSetBlendState": st["bs"] = topy(first(a, "pBlendState"))
        elif nm == "RSSetState": st["rs"] = topy(first(a, "pRasterizerState", "pState"))
        elif nm == "OMSetRenderTargets":
            st["rtvs"] = [x for x in topy(a["ppRenderTargetViews"]) if x]; st["dsv"] = topy(a["pDepthStencilView"])
            draws.append(dict(ci=ci, kind="OM", rtvs=list(st["rtvs"]), dsv=st["dsv"]))
        elif nm == "IASetIndexBuffer": st["ib"] = topy(a["pIndexBuffer"])
        elif nm == "IASetVertexBuffers":
            s0 = topy(a["StartSlot"])
            for i, v in enumerate(topy(a["ppVertexBuffers"])): st["vbs"][s0 + i] = v
        elif nm == "VSSetShader": st["vs"] = topy(first(a, "pVertexShader", "pShader")) or 0
        elif nm == "PSSetShader": st["ps"] = topy(first(a, "pPixelShader", "pShader")) or 0
        elif nm in ("VSSetConstantBuffers", "VSSetConstantBuffers1", "PSSetConstantBuffers", "PSSetConstantBuffers1"):
            s0 = topy(a["StartSlot"]); b = topy(a["ppConstantBuffers"])
            fc = topy(a["pFirstConstant"]) if "pFirstConstant" in a else None
            nc = topy(a["pNumConstants"]) if "pNumConstants" in a else None
            d = st["vscb"] if nm.startswith("VS") else st["pscb"]
            for i, x in enumerate(b):
                d[s0 + i] = (x, fc[i] if fc and i < len(fc) else 0, nc[i] if nc and i < len(nc) else 4096, nm.endswith("1"))
        elif nm in ("VSSetShaderResources", "PSSetShaderResources"):
            s0 = topy(a["StartSlot"]); d = st["vssrv"] if nm.startswith("VS") else st["pssrv"]
            for i, x in enumerate(topy(a["ppShaderResourceViews"])): d[s0 + i] = x
        elif nm == "PSSetSamplers":
            s0 = topy(a["StartSlot"])
            for i, x in enumerate(topy(a["ppSamplers"])): st["pssmp"][s0 + i] = x
        elif nm == "RSSetViewports": st["vps"] = topy(a["pViewports"]) or []
        elif nm == "Map":
            writes.append((ci, "Map", topy(first(a, "pResource")), short(str(topy(first(a, "MapType")) or "?"))))
        elif nm in ("UpdateSubresource", "UpdateSubresource1"):
            writes.append((ci, "Update", topy(first(a, "pDstResource")), "UPDATE"))
        elif nm in ("CopySubresourceRegion", "CopySubresourceRegion1", "CopyResource", "CopyStructureCount"):
            writes.append((ci, "Copy", topy(first(a, "pDstResource", "pDstBuffer")), "COPY"))
        elif nm in ("DiscardView", "DiscardView1", "DiscardResource"):
            draws.append(dict(ci=ci, kind="DISCARD", view=topy(first(a, "pResourceView", "pResource"))))
        kind = None
        if nm == "DrawIndexed": kind = "DI"
        elif nm == "DrawIndexedInstanced": kind = "DII"
        elif nm == "Draw": kind = "D"
        elif nm == "DrawInstanced": kind = "DInst"
        if kind is None: continue
        draws.append(dict(ci=ci, kind=kind, dss=st["dss"], bs=st["bs"], dsv=st["dsv"], rtvs=list(st["rtvs"]), ib=st["ib"],
                          vbs=dict(st["vbs"]), vs=st["vs"], ps=st["ps"], vscb=dict(st["vscb"]), pscb=dict(st["pscb"]),
                          vssrv=dict(st["vssrv"]), pssrv=dict(st["pssrv"]), pssmp=dict(st["pssmp"]), vps=list(st["vps"]),
                          ic=topy(first(a, "IndexCount", "IndexCountPerInstance", "VertexCount", "VertexCountPerInstance")) or 0))

    # forward binding: 1 RTV (RGBA16F, scene size) + the scene DSV; the over set inside it
    fwdBinds = [d for d in draws if d["kind"] == "OM" and len(d["rtvs"]) == 1 and res_of(d["dsv"]) == scene]
    if not fwdBinds: raise SystemExit("no forward binding")
    fb = fwdBinds[-1]["ci"]
    nxt = [d["ci"] for d in draws if d["kind"] == "OM" and d["ci"] > fb]
    fe = nxt[0] if nxt else len(sf.chunks)
    disc = [d["ci"] for d in draws if d["kind"] == "DISCARD" and fb < d["ci"] < fe and res_of(d["view"]) == scene]
    P("\n== forward binding chunk %d .. next OM bind chunk %d; scene depth discard inside it: %s" % (fb, fe, disc))
    fwd = [d for d in draws if d["kind"] != "OM" and d["kind"] != "DISCARD" and fb < d["ci"] < fe]
    def wmask_rgb(m):
        if isinstance(m, int): return (m & 7) != 0
        m = str(m)
        try: return (int(m, 0) & 7) != 0
        except Exception: return any(x in m for x in ("RED", "GREEN", "BLUE", "ALL"))
    def qualifies(d):
        ds = dssd.get(d["dss"]) or {}
        b = (bsd.get(d["bs"]) or {}).get("RenderTarget", [{}])
        b0 = b[0] if isinstance(b, list) and b else {}
        vp = d["vps"][0] if d["vps"] else None
        return (ds.get("DepthEnable") and "ZERO" in str(ds.get("DepthWriteMask")) and b0.get("BlendEnable") and
                "INV_SRC_ALPHA" in str(b0.get("DestBlend")) and wmask_rgb(b0.get("RenderTargetWriteMask", 0)) and vp and
                vp["MinDepth"] >= 0.005 and vp["MaxDepth"] <= 0.95)
    over = [d for d in fwd if qualifies(d)]
    P("   forward draws %d, the over set %d (kinds %s), distinct PS %s"
      % (len(fwd), len(over), dict(collections.Counter(d["kind"] for d in over)), collections.Counter(d["ps"] for d in over).most_common(8)))

    # (1) PS state of the over set
    P("\n== (1) PS state of the over set")
    cbc = collections.Counter(); srvc = collections.Counter(); smpc = collections.Counter(); nsrv = collections.Counter()
    for d in over:
        for s, (b, fc, nc, one) in d["pscb"].items():
            if b: cbc[(s, "R%d" % b, buf.get(b, {}).get("ByteWidth", 0), short(str(buf.get(b, {}).get("Usage"))), nc, one)] += 1
        k = 0
        for s, v in d["pssrv"].items():
            if v:
                t = res_of(v); td = tex.get(t) or {}
                srvc[(s, short(str(td.get("Format", "buffer?"))), "%sx%s" % (td.get("Width", "?"), td.get("Height", "?")))] += 1
                k += 1
        nsrv[k] += 1
        for s, v in d["pssmp"].items():
            if v: smpc[s] += 1
    P("   PS cbuffer slots (slot, buffer, bytes, usage, window, via *1): %s" % cbc.most_common(16))
    P("   PS SRVs bound per draw (count histogram) %s; (slot, format, size) %s" % (dict(nsrv), srvc.most_common(20)))
    P("   PS sampler slots bound (slot: draws) %s" % dict(smpc))
    P("   max PS SRV slot bound %s, max PS cb slot %s, max sampler slot %s" % (
        max([s for d in over for s, v in d["pssrv"].items() if v] or [-1]),
        max([s for d in over for s, v in d["pscb"].items() if v[0]] or [-1]),
        max([s for d in over for s, v in d["pssmp"].items() if v] or [-1])))

    # (2) writes to resources a still-pending over draw reads
    P("\n== (2) writes between the first over draw and the forward pass end that hit a resource of an EARLIER over draw")
    def reads(d):
        r = set()
        for s, (b, fc, nc, one) in d["vscb"].items():
            if b: r.add(("vscb", b))
        for s, (b, fc, nc, one) in d["pscb"].items():
            if b: r.add(("pscb", b))
        for s, v in d["vbs"].items():
            if v: r.add(("vb", v))
        if d["ib"]: r.add(("ib", d["ib"]))
        for s, v in d["vssrv"].items():
            if v: r.add(("vssrv", res_of(v)))
        for s, v in d["pssrv"].items():
            if v: r.add(("pssrv", res_of(v)))
        return r
    if over:
        f0 = over[0]["ci"]
        win = [w for w in writes if f0 <= w[0] <= fe]
        P("   all writes in the window: %s" % dict(collections.Counter((w[1], w[3]) for w in win)))
        hz = collections.Counter(); hzEx = {}
        for w in win:
            prior = [d for d in over if d["ci"] < w[0]]
            kinds = set()
            for d in prior:
                for (kk, rr) in reads(d):
                    if rr == w[2]: kinds.add(kk)
            if kinds:
                key = (w[1], w[3], ",".join(sorted(kinds)))
                hz[key] += 1
                hzEx.setdefault(key, (w[0], w[2], buf.get(w[2], {}).get("ByteWidth", 0)))
        if not hz: P("   NONE -- every input of every over draw is unchanged at the forward pass end (a batch there sees draw-time inputs)")
        for k, c in hz.most_common():
            P("   %s x%d (first chunk %d, R%d %d B)" % (k, c, hzEx[k][0], hzEx[k][1], hzEx[k][2]))

    # (3) replay: GPU durations + PS reflection
    if REPLAY and fwd:
        acts = {}
        def walk(al):
            for c in al:
                acts[c.eventId] = c
                if c.children: walk(c.children)
        walk(ctl.GetRootActions())
        by_chunk = {}
        for eid, ac in acts.items():
            for ev in ac.events: by_chunk[ev.chunkIndex] = eid
        try:
            avail = ctl.EnumerateCounters()
            if rd.GPUCounter.EventGPUDuration in avail:
                rs = ctl.FetchCounters([rd.GPUCounter.EventGPUDuration])
                dur = {}
                for r_ in rs: dur[r_.eventId] = r_.value.d
                def ms(L): return sum(dur.get(by_chunk.get(d["ci"]), 0.0) for d in L) * 1000.0
                P("\n== (3) GPU durations (this PC, RenderDoc replay, full resolution %dx%d, real scene depth bound)" % (SW, SH))
                P("   forward pass all %d draws %.3f ms; the over set %d draws %.3f ms; the rest %.3f ms"
                  % (len(fwd), ms(fwd), len(over), ms(over), ms([d for d in fwd if not qualifies(d)])))
                top = sorted(over, key=lambda d: -dur.get(by_chunk.get(d["ci"]), 0.0))[:12]
                P("   heaviest over draws: %s" % [("chunk %d ic %d PS R%d" % (d["ci"], d["ic"], d["ps"]),
                                                     round(dur.get(by_chunk.get(d["ci"]), 0.0) * 1000.0, 4)) for d in top])
                gb = [d for d in draws if d["kind"] != "OM" and d["kind"] != "DISCARD" and len(d["rtvs"]) == 4 and res_of(d["dsv"]) == scene]
                P("   (G-buffer %d draws %.3f ms for scale)" % (len(gb), ms(gb)))
            else:
                P("\n== (3) EventGPUDuration counter not available")
        except Exception:
            P("   counters: %s" % traceback.format_exc().splitlines()[-1])
        seen = {}
        for d in over:
            if d["ps"] not in seen: seen[d["ps"]] = d
        P("\n== (3b) PS reflection of the over set (%d distinct)" % len(seen))
        for sid, d in seen.items():
            eid = by_chunk.get(d["ci"])
            if eid is None: continue
            try:
                ctl.SetFrameEvent(eid, True)
                refl = ctl.GetD3D11PipelineState().pixelShader.reflection
                if not refl: continue
                cbs = [(cb.name, cb.fixedBindNumber if hasattr(cb, "fixedBindNumber") else -1) for cb in refl.constantBlocks]
                srvs = [(x.name, x.fixedBindNumber if hasattr(x, "fixedBindNumber") else -1) for x in refl.readOnlyResources]
                smps = [(x.name, x.fixedBindNumber if hasattr(x, "fixedBindNumber") else -1) for x in refl.samplers]
                outs = [(o.semanticName, o.semanticIndex, str(o.systemValue)) for o in refl.outputSignature]
                dis = ""
                try:
                    targets = ctl.GetDisassemblyTargets(True)
                    txt = ctl.DisassembleShader(ctl.GetD3D11PipelineState().pixelShader.resourceId if hasattr(ctl.GetD3D11PipelineState().pixelShader, "resourceId") else rd.ResourceId.Null(), refl, targets[0] if targets else "")
                    lines = [l for l in txt.splitlines() if l.strip() and not l.strip().startswith("//") and not l.strip().startswith("dcl_")]
                    ninst = len(lines)
                    sample = sum(1 for l in lines if "sample" in l)
                    disc = sum(1 for l in lines if "discard" in l)
                    dis = " | ~%d instructions, %d sample*, %d discard" % (ninst, sample, disc)
                except Exception:
                    dis = " | disassembly n/a (%s)" % traceback.format_exc().splitlines()[-1]
                P("   PS R%d (%d over draws): cbuffers %s, SRVs %s, samplers %s, outputs %s%s"
                  % (sid, sum(1 for x in over if x["ps"] == sid), cbs, srvs, smps, outs, dis))
            except Exception:
                P("   PS R%d: %s" % (sid, traceback.format_exc().splitlines()[-1]))
    ctl.Shutdown(); cap.Shutdown()
    P("\nDONE")
except SystemExit as e:
    P("STOP:", e)
except Exception:
    P(traceback.format_exc())
f.close()
os._exit(0)
