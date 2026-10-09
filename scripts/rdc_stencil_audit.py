# RenderDoc stencil / per-draw audit for the ETS2 / ATS DLAA injector (per-object motion vector feasibility).
#
# Run with RenderDoc's own Python (the renderdoc module is built into qrenderdoc.exe):
#   "C:\Program Files\RenderDoc\qrenderdoc.exe" --python scripts\rdc_stencil_audit.py
# Optional environment variables:
#   RDC_CAPTURE = path of the .rdc (default captures\ets2_flat_frame1969.rdc next to this script's parent folder)
#   RDC_OUT     = report path (default: <capture>.stencil_audit.txt)
#   RDC_REPLAY  = 0 to skip the replay-based checks (CB layout via post-VS data)
#
# What it reports (pure structured-chunk scan, no per-event replay, so it is fast on big captures):
#   * every depth-stencil state (DSS) the frame uses, with its stencil settings
#   * the scene depth (largest D32_FLOAT_S8X24 / D24S8 depth target) and every pass that binds it:
#     draw counts, DSS / StencilRef use, read-only DSV flags, clears (flags, stencil value)
#   * whether ANY draw on the scene depth has StencilEnable, and whether any shader resource view of the scene depth
#     texture with a stencil format (X24_TYPELESS_G8_UINT / X32_TYPELESS_G8X24_UINT) exists or is bound anywhere
#   * the G-buffer pass (4 RTVs + scene depth): DrawIndexed / DrawIndexedInstanced counts, distinct geometry keys
#     (IB, VB0, offsets, counts) as the injector builds them, duplicate keys, per depth layer (viewport MinDepth)
#   * VS cb0 window sizes and (from the captured ring-buffer Map data) the MVP rows 4..7 per draw
#   * replay check: for a few G-buffer draws, MVP(rows 4..7) * POSITION == the VS output SV_Position
import os, sys, struct, traceback, collections

def _default_capture():
    # qrenderdoc --python does not always define __file__: try the script folder, then the working directory
    bases = []
    if "__file__" in globals():
        bases.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    bases += [os.getcwd(), os.path.dirname(os.getcwd())]
    for b in bases:
        c = os.path.join(b, "captures", "ets2_flat_frame1969.rdc")
        if os.path.isfile(c):
            return c
    return os.path.join(bases[0], "captures", "ets2_flat_frame1969.rdc")
CAP = os.environ.get("RDC_CAPTURE") or _default_capture()
OUT = os.environ.get("RDC_OUT") or (os.path.splitext(CAP)[0] + ".stencil_audit.txt")
REPLAY = os.environ.get("RDC_REPLAY", "1") != "0"

f = open(OUT, "w", buffering=1)
def P(*a):
    f.write(" ".join(str(x) for x in a) + "\n")

DEPTH_FMTS_S = ("D32_FLOAT_S8X24_UINT", "R32G8X24_TYPELESS", "D24_UNORM_S8_UINT", "R24G8_TYPELESS")
STENCIL_SRV_FMTS = ("X32_TYPELESS_G8X24_UINT", "X24_TYPELESS_G8_UINT")

try:
    import renderdoc as rd

    def topy(o, depth=0):
        bt = o.type.basetype
        if bt == rd.SDBasic.Struct:
            return {o.GetChild(i).name: topy(o.GetChild(i), depth + 1) for i in range(o.NumChildren())}
        if bt == rd.SDBasic.Array:
            return [topy(o.GetChild(i), depth + 1) for i in range(o.NumChildren())]
        if bt == rd.SDBasic.Resource: return int(o.AsResourceId())
        if bt == rd.SDBasic.Boolean: return o.AsBool()
        if bt == rd.SDBasic.Float: return o.AsFloat()
        if bt == rd.SDBasic.Enum: return o.AsString()
        if bt in (rd.SDBasic.UnsignedInteger, rd.SDBasic.SignedInteger): return o.AsInt()
        if bt == rd.SDBasic.Null: return None
        if bt == rd.SDBasic.Buffer: return ("BUF", o.AsInt())
        try: return o.AsString()
        except Exception: return "?"

    def args(ch):
        return {ch.GetChild(k).name: ch.GetChild(k) for k in range(ch.NumChildren())}

    def short(fmt):
        return (fmt or "").replace("DXGI_FORMAT_", "")

    cap = rd.OpenCaptureFile()
    r = cap.OpenFile(CAP, '', None)
    P("capture", CAP, "open", r)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    P("replay", res)
    sf = ctl.GetStructuredFile()

    # ---------------------------------------------------------------------------------------------------------
    # resource tables + frame state machine (one pass over the chunks, in serialisation = execution order)
    tex = {}            # id -> desc dict
    dsv = {}            # view id -> (tex id, desc)
    srv = {}            # view id -> (res id, desc or None)
    rtv = {}            # view id -> (tex id, desc or None)
    dss = {}            # state id -> desc
    buf = {}            # id -> desc
    mapdata = {}        # buffer id -> (bytes, start offset) of the last Unmap
    il = {}             # input layout id -> element list

    st = dict(dss=None, ref=0, dsv=0, rtvs=[], ib=(0, None, 0), vbs={}, vscb0=(0, 0, 0), vp=None,
              ps_srv={}, vs_srv={}, cs_srv={}, layout=0)
    events = []         # dicts for draws / clears / dispatches / copies
    in_frame = False
    frame_chunk0 = None

    for ci, ch in enumerate(sf.chunks):
        n = ch.name
        a = args(ch)
        if n == "Internal::Beginning of Capture":
            in_frame = True
            frame_chunk0 = ci
            s0 = topy(a["state"]) if "state" in a else {}
            om = s0.get("OM", {}) if isinstance(s0, dict) else {}
            P("initial OM state keys:", sorted(om.keys()) if isinstance(om, dict) else om)
            if isinstance(om, dict):
                st["dss"] = om.get("DepthStencilState", om.get("DepthStencil", None))
                st["ref"] = om.get("StencRef", om.get("StencilRef", 0)) or 0
                st["dsv"] = om.get("DepthView", 0) or 0
                rt = om.get("RenderTargets", [])
                st["rtvs"] = [x for x in rt if x] if isinstance(rt, list) else []
                P("initial DSS", st["dss"], "ref", st["ref"], "dsv", st["dsv"], "rtvs", st["rtvs"])
            continue
        if n.endswith("::CreateTexture2D") or n.endswith("::CreateTexture2D1"):
            d = topy(a.get("Descriptor") or a.get("pDesc"))
            tex[topy(a["pTexture"])] = d
        elif n.endswith("::CreateDepthStencilView"):
            dsv[topy(a["pView"])] = (topy(a["pResource"]), topy(a["pDesc"]) if "pDesc" in a else None)
        elif n.endswith("::CreateShaderResourceView") or n.endswith("::CreateShaderResourceView1"):
            srv[topy(a["pView"])] = (topy(a["pResource"]), topy(a["pDesc"]) if "pDesc" in a else None)
        elif n.endswith("::CreateRenderTargetView") or n.endswith("::CreateRenderTargetView1"):
            rtv[topy(a["pView"])] = (topy(a["pResource"]), topy(a["pDesc"]) if "pDesc" in a else None)
        elif n.endswith("::CreateDepthStencilState"):
            dss[topy(a["pState"])] = topy(a["Descriptor"] if "Descriptor" in a else a["pDepthStencilDesc"])
        elif n.endswith("::CreateBuffer"):
            buf[topy(a["pBuffer"])] = topy(a["pDesc"])
        elif n.endswith("::CreateInputLayout"):
            il[topy(a["pInputLayout"])] = topy(a["pInputElementDescs"])
        if not in_frame:
            continue
        # ---- frame chunks -------------------------------------------------------------------------------------
        if n.endswith("::OMSetDepthStencilState"):
            st["dss"] = topy(a["pDepthStencilState"]); st["ref"] = topy(a["StencilRef"])
        elif n.endswith("::OMSetRenderTargets"):
            st["rtvs"] = [x for x in topy(a["ppRenderTargetViews"]) if x]
            st["dsv"] = topy(a["pDepthStencilView"])
        elif n.endswith("::OMSetRenderTargetsAndUnorderedAccessViews"):
            nv = topy(a["NumRTVs"])
            if nv != 0xFFFFFFFF:   # D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL
                st["rtvs"] = [x for x in topy(a["ppRenderTargetViews"]) if x]
                st["dsv"] = topy(a["pDepthStencilView"])
        elif n.endswith("::IASetIndexBuffer"):
            st["ib"] = (topy(a["pIndexBuffer"]), topy(a["Format"]), topy(a["Offset"]))
        elif n.endswith("::IASetVertexBuffers"):
            s0 = topy(a["StartSlot"]); vbl = topy(a["ppVertexBuffers"]); strd = topy(a["pStrides"]); offs = topy(a["pOffsets"])
            for i, v in enumerate(vbl):
                st["vbs"][s0 + i] = (v, strd[i] if i < len(strd) else 0, offs[i] if i < len(offs) else 0)
        elif n.endswith("::IASetInputLayout"):
            st["layout"] = topy(a["pInputLayout"])
        elif n.endswith("::VSSetConstantBuffers1") or n.endswith("::VSSetConstantBuffers"):
            s0 = topy(a["StartSlot"])
            if s0 == 0:
                b = topy(a["ppConstantBuffers"])
                fc = topy(a["pFirstConstant"]) if "pFirstConstant" in a else [0]
                nc = topy(a["pNumConstants"]) if "pNumConstants" in a else [4096]
                st["vscb0"] = (b[0] if b else 0, (fc or [0])[0] if fc else 0, (nc or [4096])[0] if nc else 4096)
        elif n.endswith("::RSSetViewports"):
            vps = topy(a["pViewports"])
            st["vp"] = vps[0] if vps else None
        elif n.endswith("SetShaderResources"):
            stage = n.split("::")[1][:2]
            s0 = topy(a["StartSlot"]); views = topy(a["ppShaderResourceViews"])
            key = {"PS": "ps_srv", "VS": "vs_srv", "CS": "cs_srv"}.get(stage)
            if key:
                for i, v in enumerate(views): st[key][s0 + i] = v
        elif n.endswith("::Unmap"):
            if "MapWrittenData" in a:
                bid = topy(a["pResource"])
                data = b""
                o = a["MapWrittenData"]
                for getter in (lambda: o.data.basic.u, lambda: o.AsInt()):
                    try:
                        data = bytes(sf.buffers[int(getter())])
                        if data: break
                    except Exception:
                        pass
                start = topy(a.get("Byte offset to start of written data")) if a.get("Byte offset to start of written data") is not None else 0
                mapdata[bid] = (data, start)
        kind = None
        if n.endswith("::DrawIndexed"): kind = "DI"
        elif n.endswith("::DrawIndexedInstanced"): kind = "DII"
        elif n.endswith("::Draw"): kind = "D"
        elif n.endswith("::DrawInstanced"): kind = "DInst"
        elif n.endswith("::DrawIndexedInstancedIndirect") or n.endswith("::DrawInstancedIndirect"): kind = "Indirect"
        elif n.endswith("::DrawAuto"): kind = "Auto"
        elif n.endswith("::Dispatch") or n.endswith("::DispatchIndirect"): kind = "CS"
        elif n.endswith("::ClearDepthStencilView"): kind = "CLRDS"
        elif n.endswith("::CopyResource") or n.endswith("::CopySubresourceRegion") or n.endswith("::CopySubresourceRegion1"):
            kind = "COPY"
        elif n.endswith("::DiscardView") or n.endswith("::DiscardView1") or n.endswith("::DiscardResource"):
            kind = "DISCARD"
        if kind is None:
            continue
        e = dict(ci=ci, kind=kind, dss=st["dss"], ref=st["ref"], dsv=st["dsv"], rtvs=list(st["rtvs"]),
                 ib=st["ib"], vb0=st["vbs"].get(0, (0, 0, 0)), vbs=dict(st["vbs"]), cb0=st["vscb0"], vp=st["vp"],
                 ps_srv=dict(st["ps_srv"]), vs_srv=dict(st["vs_srv"]), cs_srv=dict(st["cs_srv"]), layout=st["layout"])
        if kind == "DI":
            e.update(ic=topy(a["IndexCount"]), si=topy(a["StartIndexLocation"]), bv=topy(a["BaseVertexLocation"]), inst=1, sinst=0)
        elif kind == "DII":
            e.update(ic=topy(a["IndexCountPerInstance"]), si=topy(a["StartIndexLocation"]), bv=topy(a["BaseVertexLocation"]),
                     inst=topy(a["InstanceCount"]), sinst=topy(a["StartInstanceLocation"]))
        elif kind == "CLRDS":
            e.update(cdsv=topy(a["pDepthStencilView"]), flags=topy(a["ClearFlags"]), depth=topy(a["Depth"]), stencil=topy(a["Stencil"]))
        elif kind == "COPY":
            e.update(dst=topy(a["pDstResource"]), src=topy(a["pSrcResource"]))
        elif kind == "DISCARD":
            e.update(view=topy(a["pResourceView"]) if "pResourceView" in a else topy(a.get("pResource")))
        events.append(e)

    # ---------------------------------------------------------------------------------------------------------
    P("\n== resources: %d textures, %d DSVs, %d SRVs, %d RTVs, %d DSS, %d buffers; %d frame events (draw/clear/copy)"
      % (len(tex), len(dsv), len(srv), len(rtv), len(dss), len(buf), len(events)))

    def fmt_dss(sid):
        d = dss.get(sid)
        if d is None: return "DSS %s (unknown / null = default: depth LESS on, stencil OFF)" % sid
        ff = d["FrontFace"]; bf = d["BackFace"]
        def face(x):
            return "%s/%s/%s %s" % (x["StencilFailOp"].replace("D3D11_STENCIL_OP_", ""), x["StencilDepthFailOp"].replace("D3D11_STENCIL_OP_", ""),
                                    x["StencilPassOp"].replace("D3D11_STENCIL_OP_", ""), x["StencilFunc"].replace("D3D11_COMPARISON_", ""))
        return ("DSS %s depth=%s write=%s func=%s | StencilEnable=%s read=0x%02x write=0x%02x front[fail/zfail/pass func]=%s back=%s"
                % (sid, d["DepthEnable"], d["DepthWriteMask"].replace("D3D11_DEPTH_WRITE_MASK_", ""), d["DepthFunc"].replace("D3D11_COMPARISON_", ""),
                   d["StencilEnable"], d["StencilReadMask"], d["StencilWriteMask"], face(ff), face(bf)))

    P("\n== all depth-stencil states created")
    for sid in sorted(dss): P("  " + fmt_dss(sid))

    # scene depth = biggest depth+stencil texture that has a DSV
    def tex_of_dsv(v): return dsv.get(v, (0, None))[0]
    cands = set()
    for v, (t, d) in dsv.items():
        td = tex.get(t)
        if td and any(td["Format"].endswith(x) for x in DEPTH_FMTS_S):
            cands.add(t)
    scene = max(cands, key=lambda t: tex[t]["Width"] * tex[t]["Height"]) if cands else 0
    sd = tex.get(scene, {})
    P("\n== scene depth texture R%d %dx%d %s bind=%s" % (scene, sd.get("Width", 0), sd.get("Height", 0), short(sd.get("Format")), sd.get("BindFlags")))
    scene_dsvs = {v: d for v, (t, d) in dsv.items() if t == scene}
    for v, d in scene_dsvs.items(): P("  DSV R%d %s" % (v, d))
    scene_srvs = {v: d for v, (t, d) in srv.items() if t == scene}
    P("  SRVs of the scene depth texture: %d" % len(scene_srvs))
    for v, d in scene_srvs.items(): P("    SRV R%d %s" % (v, d))
    stencil_srvs = {v for v, d in scene_srvs.items() if d and any(str(d.get("Format", "")).endswith(x) for x in STENCIL_SRV_FMTS)}
    # every texture that ANY SRV with a stencil format views (the game reading stencil anywhere)
    any_stencil_srv = [(v, t) for v, (t, d) in srv.items() if d and any(str(d.get("Format", "")).endswith(x) for x in STENCIL_SRV_FMTS)]
    P("  SRVs with a STENCIL format in the whole capture: %d %s" % (len(any_stencil_srv), any_stencil_srv[:10]))

    # ---- pass segmentation: runs of events with the same (DSV texture, RT signature)
    def rt_sig(e):
        out = []
        for v in e["rtvs"]:
            t = rtv.get(v, (0, None))[0]
            td = tex.get(t, {})
            out.append("%s %dx%d" % (short(td.get("Format", "?")), td.get("Width", 0), td.get("Height", 0)))
        return tuple(out)
    passes = []
    cur = None
    for e in events:
        if e["kind"] in ("COPY", "DISCARD"): continue
        dtex = tex_of_dsv(e["cdsv"] if e["kind"] == "CLRDS" else e["dsv"])
        sig = (dtex, rt_sig(e) if e["kind"] != "CLRDS" else None)
        if e["kind"] == "CLRDS":
            if dtex == scene:
                passes.append(dict(sig=("CLEAR", dtex), evs=[e])); cur = None
            continue
        if cur is None or cur["sig"] != sig:
            cur = dict(sig=sig, evs=[]); passes.append(cur)
        cur["evs"].append(e)

    P("\n== passes touching the scene depth (in order); draws = Draw*/DrawIndexed*; CS = dispatches")
    stencil_draws_scene = 0
    stencil_reads_scene = 0
    stencil_writes_scene = 0
    nonzero_ref_scene = 0
    pi = 0
    gpass = []
    for p in passes:
        if p["sig"][0] == "CLEAR":
            e = p["evs"][0]
            P("  [clear] chunk %d ClearDepthStencilView DSV R%d flags=%s depth=%s stencil=%s" % (e["ci"], e["cdsv"], e["flags"], e["depth"], e["stencil"]))
            continue
        dtex, rts = p["sig"]
        evs = [e for e in p["evs"] if e["kind"] not in ("CLRDS",)]
        draws = [e for e in evs if e["kind"] in ("DI", "DII", "D", "DInst", "Indirect", "Auto")]
        # passes that do not bind the scene DSV but READ the scene depth texture through an SRV
        reads_scene_srv = [e for e in evs if any(srv.get(v, (0, None))[0] == scene for d_ in (e["ps_srv"], e["vs_srv"], e["cs_srv"]) for v in d_.values() if v)]
        reads_stencil_srv = [e for e in evs if any(v in stencil_srvs for d_ in (e["ps_srv"], e["vs_srv"], e["cs_srv"]) for v in d_.values() if v)]
        if dtex != scene and not reads_scene_srv:
            continue
        pi += 1
        kinds = collections.Counter(e["kind"] for e in evs)
        dssc = collections.Counter((e["dss"], e["ref"]) for e in draws)
        ro = scene_dsvs.get(draws[0]["dsv"]) if draws else None
        P("  [pass %d] chunks %d..%d  DSV=%s  RTs=%s  events=%s" % (pi, evs[0]["ci"] if evs else -1, evs[-1]["ci"] if evs else -1,
          ("scene R%d flags=%s" % (draws[0]["dsv"], (ro or {}).get("Flags"))) if dtex == scene and draws else ("R%d" % dtex if dtex else "none"),
          list(rts), dict(kinds)))
        if reads_scene_srv:
            P("      reads the scene depth texture through an SRV in %d events (stencil-format SRV in %d)" % (len(reads_scene_srv), len(reads_stencil_srv)))
        for (sid, ref), c in dssc.most_common():
            P("      %5d draws  ref=%d  %s" % (c, ref, fmt_dss(sid)))
            d = dss.get(sid)
            if dtex == scene and d and d["StencilEnable"]:
                stencil_draws_scene += c
                rd_ = any(d[x]["StencilFunc"] not in ("D3D11_COMPARISON_ALWAYS",) for x in ("FrontFace", "BackFace")) and d["StencilReadMask"] != 0
                wr_ = d["StencilWriteMask"] != 0 and any(d[x][k] != "D3D11_STENCIL_OP_KEEP" for x in ("FrontFace", "BackFace")
                                                         for k in ("StencilFailOp", "StencilDepthFailOp", "StencilPassOp"))
                if rd_: stencil_reads_scene += c
                if wr_: stencil_writes_scene += c
            if dtex == scene and ref != 0: nonzero_ref_scene += c
        if dtex == scene and len(rts) == 4:
            gpass.append(draws)

    P("\n== SUMMARY stencil on the scene depth: draws with StencilEnable=%d (reading stencil: %d, writing stencil: %d); "
      "draws with StencilRef != 0: %d; stencil-format SRVs of the scene depth: %d (bound anywhere: %s)"
      % (stencil_draws_scene, stencil_reads_scene, stencil_writes_scene, nonzero_ref_scene, len(stencil_srvs),
         any(any(v in stencil_srvs for d_ in (e["ps_srv"], e["vs_srv"], e["cs_srv"]) for v in d_.values() if v) for e in events)))
    copies = [e for e in events if e["kind"] == "COPY" and (e["dst"] == scene or e["src"] == scene)]
    P("   copies involving the scene depth: %d %s" % (len(copies), [(e["ci"], e["src"], e["dst"]) for e in copies][:8]))
    disc = [e for e in events if e["kind"] == "DISCARD" and (dsv.get(e["view"], (0,))[0] == scene)]
    P("   DiscardView of a scene-depth DSV: %d" % len(disc))

    # ---- G-buffer pass(es): draw counts, keys, layers, cb0 windows, MVPs
    P("\n== G-buffer passes (scene DSV + 4 RTVs): %d" % len(gpass))
    allg = []
    for gi, draws in enumerate(gpass):
        allg += draws
        kc = collections.Counter(e["kind"] for e in draws)
        P("  G-buffer pass %d: %d draws %s" % (gi, len(draws), dict(kc)))
    draws = allg
    def layer(e):
        vp = e["vp"]
        return 1 if (vp and vp["MinDepth"] >= 0.85) else (2 if (vp and vp["MaxDepth"] < 0.01) else 0)
    def key(e):
        return (e["ib"][0], e["vb0"][0], e["ib"][2], e["vb0"][2], e.get("ic"), e.get("si"), e.get("bv"))
    for kind in ("DI", "DII"):
        sub = [e for e in draws if e["kind"] == kind]
        if not sub: continue
        keys = collections.Counter(key(e) for e in sub)
        dup = sum(1 for k, c in keys.items() if c > 1)
        dupd = sum(c for k, c in keys.items() if c > 1)
        lay = collections.Counter(layer(e) for e in sub)
        P("  %s: %d draws, %d distinct geometry keys, %d keys drawn more than once (%d draws, max %d), layers world/cabin/far = %d/%d/%d"
          % (kind, len(sub), len(keys), dup, dupd, max(keys.values()), lay[0], lay[1], lay[2]))
        if kind == "DII":
            ic = collections.Counter(e["inst"] for e in sub)
            P("     InstanceCount histogram:", sorted(ic.items())[:20])
            P("     vertex buffer slots used:", collections.Counter(tuple(sorted(k for k, v in e["vbs"].items() if v[0])) for e in sub).most_common(5))
        cbn = collections.Counter(e["cb0"][2] for e in sub)
        P("     VS cb0 window sizes (float4 constants):", sorted(cbn.items()))
        # MVP from the captured ring data
        vals = []
        miss = 0
        for e in sub:
            b, first, num = e["cb0"]
            md = mapdata.get(b)
            if not md or num * 16 < 128: miss += 1; continue
            data, start = md
            off = first * 16 + 64 - start
            if off < 0 or off + 64 > len(data): miss += 1; continue
            m = struct.unpack("<16f", data[off:off + 64])
            vals.append((e, m))
        P("     MVPs readable from the captured ring Map data: %d (missing %d)" % (len(vals), miss))
        if vals:
            # row 3 = (0,0,-1?,..)? report the w row stats + translation spread + |w| of the origin
            import math
            w33 = [abs(m[15]) for e, m in vals]
            w33.sort()
            P("     |MVP[3][3]| (object-origin view depth, m) percentiles 5/25/50/75/95: %s" %
              " ".join("%.1f" % w33[int(q * (len(w33) - 1))] for q in (0.05, 0.25, 0.5, 0.75, 0.95)))
            # distinct (key, translation) identities: duplicates of a mesh separated by their MVP translation column
            ids = collections.Counter((key(e), round(m[3], 3), round(m[7], 3), round(m[15], 3)) for e, m in vals)
            P("     identities (geometry key + MVP translation column): %d distinct of %d draws; still ambiguous: %d"
              % (len(ids), len(vals), sum(c for c in ids.values() if c > 1)))
            for e, m in vals[:3]:
                P("     sample chunk %d MVP rows: %s" % (e["ci"], " | ".join(" ".join("%.4f" % x for x in m[r*4:r*4+4]) for r in range(4))))

    # draws with a non-default StencilRef in the G-buffer: layer and object-origin depth
    for e in allg:
        if e["ref"] not in (0, 1):
            b, first, num = e["cb0"]
            md = mapdata.get(b)
            w33 = None
            if md:
                data, start = md
                off = first * 16 + 64 - start
                if 0 <= off and off + 64 <= len(data):
                    w33 = abs(struct.unpack("<16f", data[off:off + 64])[15])
            P("   G-buffer draw chunk %d %s ref=%d layer=%d IndexCount=%s origin |w|=%s" % (e["ci"], e["kind"], e["ref"], layer(e), e.get("ic"),
              "%.1f m" % w33 if w33 is not None else "?"))
    # what the injector hooks today: DrawIndexed only
    P("\n   NOTE: the injector hooks DrawIndexed only; DrawIndexedInstanced draws in the G-buffer pass are not keyed today.")

    # ---------------------------------------------------------------------------------------------------------
    # replay check of the CB layout: SV_Position == MVP(rows 4..7) * POSITION for a few G-buffer DrawIndexed
    if REPLAY and allg:
        acts = {}
        def walk(al):
            for c in al:
                acts[c.eventId] = c
                if c.children: walk(c.children)
        walk(ctl.GetRootActions())
        by_chunk = {}
        for eid, ac in acts.items():
            for ev in ac.events:
                by_chunk[ev.chunkIndex] = eid
        di = [e for e in allg if e["kind"] in ("DI", "DII")]
        picks = di[:: max(1, len(di) // 12)][:12]
        P("\n== replay CB layout check (%d draws): max |SV_Position - MVP*pos| over the first 8 vertices" % len(picks))
        okc = 0
        for e in picks:
            eid = by_chunk.get(e["ci"])
            if eid is None: P("  chunk %d: no event id" % e["ci"]); continue
            try:
                ctl.SetFrameEvent(eid, True)
                pst = ctl.GetPipelineState()
                cb = pst.GetConstantBlock(rd.ShaderStage.Vertex, 0, 0)
                bd = cb.descriptor if hasattr(cb, "descriptor") else cb
                rid = bd.resource if hasattr(bd, "resource") else bd.resourceId
                raw = ctl.GetBufferData(rid, bd.byteOffset, 128)
                M = struct.unpack("<32f", raw)[16:32]
                # input POSITION
                elems = il.get(e["layout"], [])
                pe = [x for x in elems if x["SemanticName"].upper() == "POSITION"]
                if not pe: P("  EID %d: no POSITION element" % eid); continue
                pe = pe[0]
                slot = pe["InputSlot"]; vb = e["vbs"].get(slot, (0, 0, 0))
                fmtp = pe["Format"]
                # index data
                ibid, ibf, iboff = e["ib"]
                isz = 2 if ibf.endswith("R16_UINT") else 4
                post = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
                outb = ctl.GetBufferData(post.vertexResourceId, post.vertexByteOffset, 0) if post.vertexResourceId != rd.ResourceId.Null() else b""
                oidx = None
                if post.indexResourceId != rd.ResourceId.Null() and post.indexByteStride:
                    ob = ctl.GetBufferData(post.indexResourceId, post.indexByteOffset, 8 * post.indexByteStride)
                    oidx = struct.unpack("<8" + ("H" if post.indexByteStride == 2 else "I"), ob[:8 * post.indexByteStride])
                # VB resource id: look it up through the pipeline state (ids in the chunks are original ids)
                d3 = ctl.GetD3D11PipelineState()
                vbr = d3.inputAssembly.vertexBuffers[slot]
                vrid = vbr.resourceId if hasattr(vbr, "resourceId") else vbr.resource
                ibr = d3.inputAssembly.indexBuffer
                irid = ibr.resourceId if hasattr(ibr, "resourceId") else ibr.resource
                idx = ctl.GetBufferData(irid, iboff + e["si"] * isz, 8 * isz)
                ids_ = struct.unpack("<%d%s" % (8, "H" if isz == 2 else "I"), idx)
                worst = 0.0
                nchk = 0
                for k in range(8):
                    vi = ids_[k] + e["bv"]
                    raw_v = ctl.GetBufferData(vrid, vb[2] + vi * vb[1] + pe["AlignedByteOffset"], 16)
                    if fmtp.endswith("R32G32B32_FLOAT") or fmtp.endswith("R32G32B32A32_FLOAT"):
                        x, y, z = struct.unpack("<3f", raw_v[:12])
                    elif fmtp.endswith("R16G16B16A16_FLOAT"):
                        x, y, z = struct.unpack("<3e", raw_v[:6])
                    else:
                        P("  EID %d POSITION format %s not decoded" % (eid, fmtp)); break
                    pos = (x, y, z, 1.0)
                    clip = [sum(M[r * 4 + c] * pos[c] for c in range(4)) for r in range(4)]
                    # post-VS data is indexed by the k-th index of the draw
                    ok_ = oidx[k] if oidx else k
                    ob0 = ok_ * post.vertexByteStride
                    if ob0 + 16 > len(outb): break
                    o = struct.unpack("<4f", outb[ob0: ob0 + 16])
                    dlt = max(abs(clip[i] - o[i]) / max(1.0, abs(o[3])) for i in range(4))
                    worst = max(worst, dlt); nchk += 1
                if nchk:
                    okc += 1 if worst < 1e-3 else 0
                    P("  EID %d %s ic=%d vp.min=%.2f POSITION %s: %d verts, max rel |clip - MVP*pos| = %.2e %s"
                      % (eid, e["kind"], e["ic"], e["vp"]["MinDepth"] if e["vp"] else -1, short(fmtp), nchk, worst, "OK" if worst < 1e-3 else "MISMATCH"))
            except Exception as ex:
                P("  EID %s: %s" % (eid, traceback.format_exc().splitlines()[-1]))
        P("  layout check OK on %d / %d draws" % (okc, len(picks)))

        # The VS cbuffer ring is written once per frame (Map WRITE_DISCARD before the first draw), so its contents at the
        # last G-buffer draw hold every draw's constants: read it once and decode every DrawIndexed's MVP (rows 4..7).
        try:
            last = by_chunk.get(allg[-1]["ci"])
            ctl.SetFrameEvent(last, True)
            pst = ctl.GetPipelineState()
            cb = pst.GetConstantBlock(rd.ShaderStage.Vertex, 0, 0)
            bd = cb.descriptor if hasattr(cb, "descriptor") else cb
            rid = bd.resource if hasattr(bd, "resource") else bd.resourceId
            ring = ctl.GetBufferData(rid, 0, 0)
            P("\n== VS cbuffer ring at the last G-buffer draw: %d bytes (resource %d)" % (len(ring), int(rid)))
            dis = [e for e in allg if e["kind"] == "DI" and int(e["cb0"][0]) == int(rid)]
            mv = []
            for e in dis:
                off = e["cb0"][1] * 16 + 64
                if off + 64 <= len(ring) and e["cb0"][2] * 16 >= 128:
                    mv.append((e, struct.unpack("<16f", ring[off:off + 64])))
            P("  DrawIndexed with a decodable MVP: %d of %d" % (len(mv), len(dis)))
            wl = sorted(abs(m[15]) for e, m in mv if layer(e) == 0)
            if wl:
                P("  world layer |MVP[3][3]| (object-origin view depth, m) percentiles 5/25/50/75/95: %s; closer than 8 m (own-truck "
                  "exclusion): %d" % (" ".join("%.1f" % wl[int(q * (len(wl) - 1))] for q in (0.05, 0.25, 0.5, 0.75, 0.95)),
                                      sum(1 for w in wl if w < 8.0)))
            keyc = collections.Counter(key(e) for e, m in mv)
            dup = [(e, m) for e, m in mv if keyc[key(e)] > 1]
            ids = collections.Counter((key(e), round(m[3], 2), round(m[7], 2), round(m[15], 2)) for e, m in dup)
            P("  duplicate-key draws: %d; distinct after adding the MVP translation column: %d (the injector pairs duplicates by "
              "nearest origin instead)" % (len(dup), len(ids)))
            for e, m in mv:
                if e["ref"] not in (0, 1):
                    P("  ref=%d draw chunk %d layer=%d IndexCount=%d origin |w|=%.2f m" % (e["ref"], e["ci"], layer(e), e["ic"], abs(m[15])))
        except Exception:
            P("  ring read failed: %s" % traceback.format_exc().splitlines()[-1])

    ctl.Shutdown(); cap.Shutdown()
    P("\ndone")
except Exception:
    P(traceback.format_exc())
f.close()
os._exit(0)
