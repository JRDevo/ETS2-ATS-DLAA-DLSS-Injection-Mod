# RenderDoc audit for the ETS2 / ATS DLAA injector (v0.9.0 r5): which FORWARD-pass draws write no depth?
#
# Why: the injector's camera motion vectors reproject every pixel through the DEPTH it finds in the scene depth snapshot.
# A forward-pass draw that is depth-tested but writes no depth (alpha-blended thin geometry: power / catenary wires,
# fences, glass, particles) leaves the depth of whatever is BEHIND it (often the sky = far plane) at its pixels, so those
# pixels get the motion of the background (sky: rotation only, no parallax) instead of their own.
#
# Run with RenderDoc's own Python:
#   "C:\Program Files\RenderDoc\qrenderdoc.exe" --python scripts\rdc_forward_depthwrite_audit.py
# Environment: RDC_CAPTURE (default captures\ets2_flat_frame1969.rdc), RDC_OUT (default <capture>.fwd_depthwrite.txt),
#              RDC_REPLAY=0 skips the post-VS geometry pass, RDC_MAXGEO (default 800) caps the replayed draws,
#              RDC_WHITE (default 4.0) = white point of the saved forward RT png (the HDR scene is linear; ~0.3 for a dim frame).
# Output: the report, <capture>.fwd_depthwrite.json (screen boxes of the depth-write-off draws) and
#         <capture>.fwd_rt.png (the forward render target at the end of the forward pass) for an overlay.
import os, sys, struct, traceback, collections, json, math

def _default_capture():
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
OUT = os.environ.get("RDC_OUT") or (os.path.splitext(CAP)[0] + ".fwd_depthwrite.txt")
JSN = os.path.splitext(CAP)[0] + ".fwd_depthwrite.json"
PNG = os.path.splitext(CAP)[0] + ".fwd_rt.png"
REPLAY = os.environ.get("RDC_REPLAY", "1") != "0"
MAXGEO = int(os.environ.get("RDC_MAXGEO", "800"))

f = open(OUT, "w", buffering=1)
def P(*a):
    f.write(" ".join(str(x) for x in a) + "\n")

DEPTH_FMTS_S = ("D32_FLOAT_S8X24_UINT", "R32G8X24_TYPELESS", "D24_UNORM_S8_UINT", "R24G8_TYPELESS")

try:
    import renderdoc as rd

    def topy(o):
        bt = o.type.basetype
        if bt == rd.SDBasic.Struct:
            return {o.GetChild(i).name: topy(o.GetChild(i)) for i in range(o.NumChildren())}
        if bt == rd.SDBasic.Array:
            return [topy(o.GetChild(i)) for i in range(o.NumChildren())]
        if bt == rd.SDBasic.Resource: return int(o.AsResourceId())
        if bt == rd.SDBasic.Boolean: return o.AsBool()
        if bt == rd.SDBasic.Float: return o.AsFloat()
        if bt == rd.SDBasic.Enum: return o.AsString()
        if bt in (rd.SDBasic.UnsignedInteger, rd.SDBasic.SignedInteger): return o.AsInt()
        if bt == rd.SDBasic.Null: return None
        try: return o.AsString()
        except Exception: return "?"

    def args(ch):
        return {ch.GetChild(k).name: ch.GetChild(k) for k in range(ch.NumChildren())}
    def short(fmt): return (fmt or "").replace("DXGI_FORMAT_", "")
    def first(a, *names):
        for n in names:
            if n in a: return a[n]
        return None

    cap = rd.OpenCaptureFile()
    P("capture", CAP, "open", cap.OpenFile(CAP, '', None))
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    P("replay", res)
    sf = ctl.GetStructuredFile()

    tex, dsv, rtv, dss, bls, rss = {}, {}, {}, {}, {}, {}
    st = dict(dss=None, bs=None, rs=None, dsv=0, rtvs=[], vp=None, topo="?", ps=0, vs=0, ib=(0, None, 0), vb0=(0, 0, 0))
    events = []
    in_frame = False
    logged_names = set()
    for ci, ch in enumerate(sf.chunks):
        n = ch.name
        a = args(ch)
        if n == "Internal::Beginning of Capture":
            in_frame = True
            s0 = topy(a["state"]) if "state" in a else {}
            om = s0.get("OM", {}) if isinstance(s0, dict) else {}
            if isinstance(om, dict):
                st["dss"] = om.get("DepthStencilState", om.get("DepthStencil"))
                st["bs"] = om.get("BlendState")
                st["dsv"] = om.get("DepthView", 0) or 0
                rt = om.get("RenderTargets", [])
                st["rtvs"] = [x for x in rt if x] if isinstance(rt, list) else []
            continue
        if n.endswith("::CreateTexture2D") or n.endswith("::CreateTexture2D1"):
            tex[topy(a["pTexture"])] = topy(first(a, "Descriptor", "pDesc"))
        elif n.endswith("::CreateDepthStencilView"):
            dsv[topy(a["pView"])] = (topy(a["pResource"]), topy(a["pDesc"]) if "pDesc" in a else None)
        elif n.endswith("::CreateRenderTargetView") or n.endswith("::CreateRenderTargetView1"):
            rtv[topy(a["pView"])] = (topy(a["pResource"]), topy(a["pDesc"]) if "pDesc" in a else None)
        elif n.endswith("::CreateDepthStencilState"):
            dss[topy(a["pState"])] = topy(first(a, "Descriptor", "pDepthStencilDesc"))
        elif n.endswith("::CreateBlendState") or n.endswith("::CreateBlendState1"):
            if n not in logged_names:
                logged_names.add(n); P("chunk", n, "args", sorted(a.keys()))
            k_, d_ = first(a, "pState", "pBlendState", "ppBlendState"), first(a, "Descriptor", "pBlendStateDesc", "pDesc")
            if k_ is not None and d_ is not None: bls[topy(k_)] = topy(d_)
        elif n.endswith("::CreateRasterizerState") or n.endswith("::CreateRasterizerState1") or n.endswith("::CreateRasterizerState2"):
            k_, d_ = first(a, "pState", "pRasterizerState", "ppRasterizerState"), first(a, "Descriptor", "pRasterizerDesc", "pDesc")
            if k_ is not None and d_ is not None: rss[topy(k_)] = topy(d_)
        if not in_frame:
            continue
        if n.endswith("::OMSetDepthStencilState"):
            st["dss"] = topy(a["pDepthStencilState"])
        elif n.endswith("::OMSetBlendState"):
            st["bs"] = topy(first(a, "pBlendState", "pState"))
        elif n.endswith("::RSSetState"):
            st["rs"] = topy(first(a, "pRasterizerState", "pState"))
        elif n.endswith("::OMSetRenderTargets"):
            st["rtvs"] = [x for x in topy(a["ppRenderTargetViews"]) if x]
            st["dsv"] = topy(a["pDepthStencilView"])
        elif n.endswith("::OMSetRenderTargetsAndUnorderedAccessViews"):
            if topy(a["NumRTVs"]) != 0xFFFFFFFF:
                st["rtvs"] = [x for x in topy(a["ppRenderTargetViews"]) if x]
                st["dsv"] = topy(a["pDepthStencilView"])
        elif n.endswith("::IASetPrimitiveTopology"):
            st["topo"] = topy(a["Topology"]).replace("D3D11_PRIMITIVE_TOPOLOGY_", "").replace("D3D_PRIMITIVE_TOPOLOGY_", "")
        elif n.endswith("::PSSetShader"):
            st["ps"] = topy(first(a, "pPixelShader", "pShader"))
        elif n.endswith("::VSSetShader"):
            st["vs"] = topy(first(a, "pVertexShader", "pShader"))
        elif n.endswith("::RSSetViewports"):
            vps = topy(a["pViewports"]); st["vp"] = vps[0] if vps else None
        elif n.endswith("::IASetIndexBuffer"):
            st["ib"] = (topy(a["pIndexBuffer"]), topy(a["Format"]), topy(a["Offset"]))
        elif n.endswith("::IASetVertexBuffers"):
            if topy(a["StartSlot"]) == 0:
                vbl = topy(a["ppVertexBuffers"]); s_ = topy(a["pStrides"]); o_ = topy(a["pOffsets"])
                if vbl: st["vb0"] = (vbl[0], s_[0] if s_ else 0, o_[0] if o_ else 0)
        kind = None
        if n.endswith("::DrawIndexed"): kind = "DI"
        elif n.endswith("::DrawIndexedInstanced"): kind = "DII"
        elif n.endswith("::Draw"): kind = "D"
        elif n.endswith("::DrawInstanced"): kind = "DInst"
        elif n.endswith("Indirect"): kind = "Indirect"
        elif n.endswith("::ClearDepthStencilView"): kind = "CLRDS"
        elif n.endswith("::DiscardView") or n.endswith("::DiscardView1") or n.endswith("::DiscardResource"): kind = "DISCARD"
        elif n.endswith("::CopyResource") or n.endswith("::CopySubresourceRegion"): kind = "COPY"
        if kind is None:
            continue
        e = dict(ci=ci, kind=kind, dss=st["dss"], bs=st["bs"], rs=st["rs"], dsv=st["dsv"], rtvs=list(st["rtvs"]), vp=st["vp"],
                 topo=st["topo"], ps=st["ps"], vs=st["vs"], ib=st["ib"], vb0=st["vb0"])
        if kind == "DI": e.update(ic=topy(a["IndexCount"]), inst=1)
        elif kind == "DII": e.update(ic=topy(a["IndexCountPerInstance"]), inst=topy(a["InstanceCount"]))
        elif kind == "D": e.update(ic=topy(a["VertexCount"]), inst=1)
        elif kind == "DInst": e.update(ic=topy(a["VertexCountPerInstance"]), inst=topy(a["InstanceCount"]))
        elif kind == "CLRDS": e.update(cdsv=topy(a["pDepthStencilView"]))
        elif kind == "DISCARD": e.update(view=topy(first(a, "pResourceView", "pResource")))
        elif kind == "COPY": e.update(dst=topy(a["pDstResource"]), src=topy(a["pSrcResource"]))
        events.append(e)

    def tex_of_dsv(v): return dsv.get(v, (0, None))[0]
    cands = {t for v, (t, d) in dsv.items() if tex.get(t) and any(tex[t]["Format"].endswith(x) for x in DEPTH_FMTS_S)}
    scene = max(cands, key=lambda t: tex[t]["Width"] * tex[t]["Height"]) if cands else 0
    sd = tex.get(scene, {})
    W, H = sd.get("Width", 0), sd.get("Height", 0)
    P("\n== scene depth R%d %dx%d %s" % (scene, W, H, short(sd.get("Format"))))

    def rtfmt(v):
        t = rtv.get(v, (0, None))[0]; td = tex.get(t, {})
        return short(td.get("Format", "?")), td.get("Width", 0), td.get("Height", 0), t

    def classify(e):
        if tex_of_dsv(e["dsv"]) != scene: return None
        if len(e["rtvs"]) == 4: return "gbuf"
        if len(e["rtvs"]) == 1 and rtfmt(e["rtvs"][0])[0].startswith("R16G16B16A16"): return "fwd"
        return "other-scene-dsv(%d rt)" % len(e["rtvs"])

    # ---- timeline on the scene depth (clears, discards, gbuf / fwd runs)
    P("\n== scene-depth timeline (runs)")
    run = None
    def flush(r):
        if r: P("  chunks %6d..%6d  %-24s %5d draws" % (r[1], r[2], r[0], r[3]))
    for e in events:
        if e["kind"] == "CLRDS":
            if tex_of_dsv(e["cdsv"]) == scene:
                flush(run); run = None; P("  chunk  %6d  ClearDepthStencilView (scene)" % e["ci"])
            continue
        if e["kind"] == "DISCARD":
            if tex_of_dsv(e["view"]) == scene or e["view"] == scene:
                flush(run); run = None; P("  chunk  %6d  Discard of the scene depth  <- the injector's depth snapshot point (flat)" % e["ci"])
            continue
        if e["kind"] == "COPY":
            if e["src"] == scene or e["dst"] == scene:
                flush(run); run = None; P("  chunk  %6d  Copy src=R%d dst=R%d" % (e["ci"], e["src"], e["dst"]))
            continue
        c = classify(e)
        if c is None: continue
        if run and run[0] == c: run[2] = e["ci"]; run[3] += 1
        else:
            flush(run); run = [c, e["ci"], e["ci"], 1]
    flush(run)

    def dssd(e):
        d = dss.get(e["dss"])
        if d is None: return (True, True, "LESS(default)")
        return (bool(d["DepthEnable"]), d["DepthWriteMask"].endswith("ALL"), d["DepthFunc"].replace("D3D11_COMPARISON_", ""))
    def blend0(e):
        b = bls.get(e["bs"])
        if b is None: return "off(default)"
        rt0 = b["RenderTarget"][0] if isinstance(b.get("RenderTarget"), list) else None
        if not rt0: return "?"
        if not rt0.get("BlendEnable"): return "off"
        s = lambda k: str(rt0.get(k, "?")).replace("D3D11_BLEND_OP_", "").replace("D3D11_BLEND_", "")
        return "%s*%s %s %s" % (s("SrcBlend"), "src", s("BlendOp"), s("DestBlend"))
    def a2c(e):
        b = bls.get(e["bs"]); return bool(b and b.get("AlphaToCoverageEnable"))

    fwd = [e for e in events if e["kind"] in ("DI", "DII", "D", "DInst", "Indirect") and classify(e) == "fwd"]
    gb = [e for e in events if e["kind"] in ("DI", "DII", "D", "DInst", "Indirect") and classify(e) == "gbuf"]
    P("\n== G-buffer draws %d, forward draws %d (1 RTV RGBA16F + scene DSV)" % (len(gb), len(fwd)))
    combo = collections.Counter()
    for e in fwd:
        de, dw, fn = dssd(e)
        combo[("depthTest" if de else "noTest", "WRITE" if dw else "nowrite", fn, blend0(e), "A2C" if a2c(e) else "", e["topo"],
               "layer=%s" % ("cabin" if e["vp"] and e["vp"]["MinDepth"] >= 0.85 else ("far" if e["vp"] and e["vp"]["MaxDepth"] < 0.011 else "world")))] += 1
    P("  forward draws by (depth test, depth write, func, RT0 blend, alpha-to-coverage, topology, depth layer):")
    for k, c in combo.most_common():
        P("   %5d  %s" % (c, "  ".join(k)))
    nw = [e for e in fwd if dssd(e)[0] and not dssd(e)[1]]
    wr = [e for e in fwd if dssd(e)[1]]
    P("  forward: depth-tested + NO depth write: %d draws (%d indices total); depth WRITE: %d draws"
      % (len(nw), sum(e.get("ic", 0) * max(1, e.get("inst", 1)) for e in nw), len(wr)))
    ich = collections.Counter()
    for e in nw:
        ic = e.get("ic", 0)
        b = "<=12" if ic <= 12 else ("<=96" if ic <= 96 else ("<=600" if ic <= 600 else ("<=3000" if ic <= 3000 else ">3000")))
        ich[b] += 1
    P("  depth-write-off index-count histogram:", dict(ich))
    P("  depth-write-off draw API:", dict(collections.Counter(e["kind"] for e in nw)), "| depth-write draws:",
      [(e["ci"], e["kind"], e.get("ic"), blend0(e), dssd(e)) for e in wr])
    P("  the r5 'over' set (depth test on, write off, RT0 blend DEST=INV_SRC_ALPHA, world layer):",
      sum(1 for e in nw if blend0(e).endswith("INV_SRC_ALPHA") and not (e["vp"] and (e["vp"]["MinDepth"] >= 0.85 or e["vp"]["MaxDepth"] < 0.011))))
    P("  depth-write-off distinct pixel shaders: %d (%s)" % (len(set(e["ps"] for e in nw)),
      collections.Counter(e["ps"] for e in nw).most_common(12)))

    # ---- replay: screen-space geometry of the depth-write-off forward draws
    geo = []
    if REPLAY and nw:
        acts = {}
        def walk(al):
            for c in al:
                acts[c.eventId] = c
                if c.children: walk(c.children)
        walk(ctl.GetRootActions())
        by_chunk = {}
        for eid, ac in acts.items():
            for ev in ac.events: by_chunk[ev.chunkIndex] = eid
        picks = nw[:MAXGEO]
        P("\n== replay: screen geometry of %d depth-write-off forward draws (instance 0)" % len(picks))
        for e in picks:
            eid = by_chunk.get(e["ci"])
            if eid is None: continue
            try:
                ctl.SetFrameEvent(eid, True)
                post = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
                if post.vertexResourceId == rd.ResourceId.Null() or not post.vertexByteStride: continue
                vb = ctl.GetBufferData(post.vertexResourceId, post.vertexByteOffset, 0)
                n_ = post.numIndices
                if post.indexResourceId != rd.ResourceId.Null() and post.indexByteStride:
                    ib = ctl.GetBufferData(post.indexResourceId, post.indexByteOffset, n_ * post.indexByteStride)
                    idx = struct.unpack("<%d%s" % (n_, "H" if post.indexByteStride == 2 else "I"), ib[:n_ * post.indexByteStride])
                    idx = [i + post.baseVertex for i in idx]
                else:
                    idx = list(range(n_))
                stride = post.vertexByteStride
                pts = []
                for i in idx:
                    o = i * stride
                    if o + 16 > len(vb): pts.append(None); continue
                    x, y, z, w = struct.unpack("<4f", vb[o:o + 16])
                    if w <= 1e-4: pts.append(None); continue
                    pts.append(((x / w * 0.5 + 0.5) * W, (0.5 - y / w * 0.5) * H, w, z / w))
                # triangle areas (list topology only; strips approximated as lists). Unclipped screen-space areas: a draw
                # with vertices far outside the screen (sky dome, big ground decals) reports a huge, meaningless area --
                # only the small / thin draws' numbers are meaningful (that is all the THIN test uses).
                area = 0.0; tris = 0
                if "TRIANGLE" in e["topo"]:
                    for t in range(0, len(pts) - 2, 3 if "LIST" in e["topo"] else 1):
                        a_, b_, c_ = pts[t], pts[t + 1], pts[t + 2]
                        if not (a_ and b_ and c_): continue
                        if min(a_[2], b_[2], c_[2]) < 0.05: continue   # near-plane crossing: clipped by the GPU, area meaningless
                        area += abs((b_[0] - a_[0]) * (c_[1] - a_[1]) - (c_[0] - a_[0]) * (b_[1] - a_[1])) * 0.5; tris += 1
                vis = [p for p in pts if p]
                if not vis: continue
                x0 = max(0.0, min(p[0] for p in vis)); x1 = min(float(W), max(p[0] for p in vis))
                y0 = max(0.0, min(p[1] for p in vis)); y1 = min(float(H), max(p[1] for p in vis))
                if x1 <= x0 or y1 <= y0: continue
                bw, bh = x1 - x0, y1 - y0
                wmin = min(p[2] for p in vis); wmax = max(p[2] for p in vis)
                zmin = min(p[3] for p in vis); zmax = max(p[3] for p in vis)
                vp_ = e["vp"] or {}
                vz0, vz1 = vp_.get("MinDepth", 0.0), vp_.get("MaxDepth", 1.0)
                fill = area / max(1.0, bw * bh)
                # "thin": the draw spans a long screen distance but covers a small fraction of its box (wire / cable / rail)
                span = math.hypot(bw, bh)
                thin = span > 0.15 * W and fill < 0.05 and area < 0.02 * W * H
                geo.append(dict(eid=eid, ci=e["ci"], ic=e.get("ic", 0), inst=e.get("inst", 1), topo=e["topo"], ps=e["ps"],
                                box=[round(x0), round(y0), round(x1), round(y1)], area=round(area), fill=round(fill, 4),
                                wmin=round(wmin, 2), wmax=round(wmax, 2), thin=thin, blend=blend0(e),
                                depth=[round(vz0 + (vz1 - vz0) * max(0.0, min(1.0, zmin)), 5), round(vz0 + (vz1 - vz0) * max(0.0, min(1.0, zmax)), 5)],
                                vp=[vz0, vz1], func=dssd(e)[2], vs=e["vs"], dss=e["dss"], bs=e["bs"]))
            except Exception:
                P("  EID %s: %s" % (eid, traceback.format_exc().splitlines()[-1]))
        thin = [g for g in geo if g["thin"]]
        P("  measured %d draws; THIN (screen span > 15%% of the width, fill < 5%% of the box, < 2%% of the screen): %d"
          % (len(geo), len(thin)))
        for g in sorted(thin, key=lambda g: -g["box"][2] + g["box"][0])[:40]:
            P("   THIN EID %d ic=%d inst=%d %s ps=R%d box=%s area=%dpx fill=%.4f view depth w=%.1f..%.1f m blend=%s"
              % (g["eid"], g["ic"], g["inst"], g["topo"], g["ps"], g["box"], g["area"], g["fill"], g["wmin"], g["wmax"], g["blend"]))
        P("  depth written by the over-blend draws if they wrote depth (viewport-mapped z range) / viewport range / func / VS:")
        for g in geo:
            if g["blend"].endswith("INV_SRC_ALPHA"):
                P("   EID %d ic=%d ps=R%d vs=R%d box=%s w=%.2f..%.2f m depth=%s vp=%s func=%s blend=%s"
                  % (g["eid"], g["ic"], g["ps"], g["vs"], g["box"], g["wmin"], g["wmax"], g["depth"], g["vp"], g["func"], g["blend"]))
        P("  the 25 largest-area depth-write-off draws:")
        for g in sorted(geo, key=lambda g: -g["area"])[:25]:
            P("   EID %d ic=%d inst=%d ps=R%d box=%s area=%dpx fill=%.3f w=%.1f..%.1f m blend=%s"
              % (g["eid"], g["ic"], g["inst"], g["ps"], g["box"], g["area"], g["fill"], g["wmin"], g["wmax"], g["blend"]))
        json.dump(dict(W=W, H=H, draws=geo), open(JSN, "w"))
        # the forward RT at the last forward draw (for an overlay of the boxes)
        try:
            last = by_chunk.get(fwd[-1]["ci"])
            ctl.SetFrameEvent(last, True)
            d3 = ctl.GetD3D11PipelineState()
            rt0 = d3.outputMerger.renderTargets[0]
            rid = rt0.resource if hasattr(rt0, "resource") else rt0.resourceId
            ts = rd.TextureSave()
            ts.resourceId = rid
            ts.destType = rd.FileType.PNG
            ts.alpha = rd.AlphaMapping.Discard
            try:
                ts.comp.blackPoint = 0.0; ts.comp.whitePoint = float(os.environ.get("RDC_WHITE", "4.0"))
            except Exception:
                pass
            P("  saved forward RT:", ctl.SaveTexture(ts, PNG), PNG)
        except Exception:
            P("  forward RT save failed: %s" % traceback.format_exc().splitlines()[-1])

    ctl.Shutdown(); cap.Shutdown()
    P("\ndone")
except Exception:
    P(traceback.format_exc())
f.close()
os._exit(0)
