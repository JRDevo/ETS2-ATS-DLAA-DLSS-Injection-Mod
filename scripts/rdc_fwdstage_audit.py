# RenderDoc audit for the ETS2 / ATS DLAA injector (v0.10.0 phase 7: the pre-tonemap stage must not flip).
#   "C:\Program Files\RenderDoc\qrenderdoc.exe" --python scripts\rdc_fwdstage_audit.py
# Environment: RDC_CAPTURE (default captures\ets2_flat_frame1969.rdc), RDC_OUT (default <capture>.fwdstage_audit.txt).
# Question: between the end of the main G-buffer and the tonemap, which OUTPUT-MERGER BINDINGS exist (every
# OMSetRenderTargets with its RTVs / DSV, draws per binding, queries begun / ended inside it), which of them use the SCENE
# DEPTH as DSV or as a PS SRV, and what is bound at the scene depth discard. The in-game log (v0.10.0 phase 6c) has frames
# where the forward colour is NOT the RTV0 at the discard; this lists what the frame structure offers instead.
import os, collections, traceback

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
OUT = os.environ.get("RDC_OUT") or (os.path.splitext(CAP)[0] + ".fwdstage_audit.txt")
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
    def short(fmt): return (fmt or "").replace("DXGI_FORMAT_", "")
    def g(a, *names):
        for n in names:
            if n in a: return topy(a[n])
        return None

    cap = rd.OpenCaptureFile(); cap.OpenFile(CAP, '', None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None); P("replay", res, "capture", CAP)
    sf = ctl.GetStructuredFile()
    tex = {}; view = {}; query = {}
    in_frame = False
    binds = []          # [ci, rtv ids, dsv id, draws, queries begun, queries ended, {(slot, srv)} read by PS]
    cur = None
    psrv = {}
    events = []
    names = collections.Counter()
    for ci, ch in enumerate(sf.chunks):
        n = ch.name; a = args(ch); nm = n.split("::")[-1]
        if n == "Internal::Beginning of Capture": in_frame = True; continue
        if n.endswith("::CreateTexture2D") or n.endswith("::CreateTexture2D1"):
            tex[topy(a["pTexture"])] = topy(a.get("Descriptor") or a.get("pDesc"))
        elif ("::Create" in n) and "pView" in a and "pResource" in a:
            kind = "dsv" if "DepthStencil" in n else ("rtv" if "RenderTarget" in n else ("uav" if "Unordered" in n else "srv"))
            view[topy(a["pView"])] = (kind, topy(a["pResource"]))
        elif n.endswith("::CreateQuery") or n.endswith("::CreatePredicate"):
            d = topy(a.get("Descriptor") or a.get("pQueryDesc") or a.get("pPredicateDesc")) or {}
            q = g(a, "pQuery", "pPredicate")
            query[q] = d.get("Query", "?") if isinstance(d, dict) else "?"
        if not in_frame: continue
        names[nm] += 1
        if nm in ("OMSetRenderTargets", "OMSetRenderTargetsAndUnorderedAccessViews"):
            rt = [x for x in (topy(a["ppRenderTargetViews"]) or [])] if "ppRenderTargetViews" in a else (cur[1] if cur else [])
            cur = [ci, rt, g(a, "pDepthStencilView") or 0, 0, [], [], set()]
            binds.append(cur)
        elif nm.endswith("SetShaderResources") and nm.startswith("PS"):
            s0 = topy(a["StartSlot"]); vl = topy(a["ppShaderResourceViews"]) or []
            for i, v in enumerate(vl): psrv[s0 + i] = v
        elif nm in ("Begin", "End") and cur is not None:
            q = g(a, "pAsync")
            (cur[4] if nm == "Begin" else cur[5]).append(query.get(q, "async R%s" % q))
        elif nm.startswith("Draw") and cur is not None:
            cur[3] += 1
            for s, v in psrv.items():
                if v: cur[6].add((s, v))
        if nm in ("DiscardView", "DiscardView1", "DiscardResource", "ClearDepthStencilView", "ClearRenderTargetView",
                  "Present", "SetPredication", "CopyResource", "CopySubresourceRegion"):
            events.append((ci, nm, g(a, "pResourceView", "pResource", "pDepthStencilView", "pRenderTargetView", "pPredicate",
                                     "pDstResource"), cur[0] if cur else -1))

    def res_of(v): return view.get(v, (None, 0))[1]
    def tdesc(t): return tex.get(t, {})
    def tfmt(t): return short(tdesc(t).get("Format", "?"))
    def tsz(t): d = tdesc(t); return (d.get("Width", 0), d.get("Height", 0))
    def tname_res(t):
        w, h = tsz(t)
        return "R%d %s %dx%d" % (t, tfmt(t), w, h)
    def tname(v):
        if not v: return "-"
        return tname_res(res_of(v))
    depths = {}
    for v, (k, t) in view.items():
        if k == "dsv" and any(tfmt(t).endswith(x) for x in DEPTH_FMTS_S): depths[t] = tsz(t)
    scene = max(depths, key=lambda t: depths[t][0] * depths[t][1]) if depths else 0
    SW, SH = depths.get(scene, (0, 0))
    P("scene depth R%d %dx%d" % (scene, SW, SH))
    P("queries created: %s" % dict(collections.Counter(query.values())))
    P("in-frame call counts (selected): %s" % {k: names[k] for k in ("Begin", "End", "SetPredication", "GetData",
                                                                     "OMSetRenderTargets", "DiscardView", "DiscardResource")})
    gb = [b for b in binds if len([x for x in b[1] if x]) == 4 and res_of(b[2]) == scene]
    if not gb: raise SystemExit("no G-buffer bind")
    start = gb[-1][0]
    disc = [e for e in events if e[1].startswith("Discard") and (res_of(e[2]) == scene or e[2] == scene)]
    P("scene depth discards in the frame: %s" % [(e[0], e[1]) for e in disc])
    end = disc[-1][0] if disc else start
    P("\n== every OM binding from the last main G-buffer bind (chunk %d) to the scene depth discard (chunk %d), + 6 after"
      % (start, end))
    after = 0
    for b in binds:
        if b[0] < start: continue
        if b[0] > end:
            after += 1
            if after > 6: break
        rts = [x for x in b[1] if x]
        sdsv = res_of(b[2]) == scene
        srvd = sorted(s for s, v in b[6] if res_of(v) == scene)
        P("  chunk %6d  RTVs %d [%s]  DSV %s%s  draws %d%s%s%s" % (b[0], len(rts), "; ".join(tname(v) for v in rts),
          tname(b[2]), " (SCENE)" if sdsv else "", b[3],
          (" | queries begun %s" % dict(collections.Counter(b[4]))) if b[4] else "",
          (" ended %s" % dict(collections.Counter(b[5]))) if b[5] else "",
          (" | a PS reads the scene depth at t%s" % srvd) if srvd else ""))
    P("\n== discard / clear / copy / predication events from the last G-buffer bind to the discard + 400 chunks")
    for e in events:
        if start <= e[0] <= end + 400:
            if e[1] == "SetPredication": what = "R%s" % e[2]
            elif e[1] in ("DiscardResource", "CopyResource", "CopySubresourceRegion"): what = tname_res(e[2]) if e[2] else "-"
            else: what = tname(e[2])
            P("  chunk %6d %-22s %s (in the binding of chunk %d)" % (e[0], e[1], what, e[3]))
    P("\n== every binding in the frame with the scene depth as DSV: RTV set -> bindings, draws")
    agg = collections.OrderedDict()
    for b in binds:
        if res_of(b[2]) != scene: continue
        k = "; ".join(tname(v) for v in b[1] if v) or "(no RTV: depth only)"
        a_ = agg.setdefault(k, [0, 0, b[0]]); a_[0] += 1; a_[1] += b[3]
    for k, v in agg.items(): P("  %-90s binds %3d draws %5d first chunk %d" % (k, v[0], v[1], v[2]))
    ctl.Shutdown(); cap.Shutdown()
    P("\nDONE")
except SystemExit as x:
    P("STOP", x)
except Exception:
    P("FATAL", traceback.format_exc())
f.close()
os._exit(0)
