# RenderDoc audit for the ETS2 / ATS DLAA injector (v0.10.0 phase 4).
#   "C:\Program Files\RenderDoc\qrenderdoc.exe" --python scripts\rdc_fwdcolour_audit.py
# Environment: RDC_CAPTURE (default captures\ets2_flat_frame1969.rdc), RDC_OUT (default <capture>.fwdcolour_audit.txt),
#              RDC_REPLAY=0 skips the post-VS checks of the small forward draws.
# Two questions:
#  A. LIFETIME of the main scene's forward colour (the RGBA16F texture of the 1-RTV forward pass on the scene depth): who
#     writes / reads / copies / discards it between the forward pass and the end of the frame (job B: DLAA in place on it at
#     the scene depth discard -- is it still intact there, is it read before the tonemap, is it discarded / reused?).
#  B. SMALL FORWARD DRAWS (plates / decals, IndexCount <= 60, world layer): what VS cb0 rows 0..7 hold (ModelView rows 0..3,
#     MVP rows 4..7 -- or a camera-only VP with the transform baked into the vertices), the vertex POSITION range, and
#     whether SV_Position == MVP(rows 4..7) * POSITION (job A, hypothesis H1).
import os, struct, traceback, collections, math

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
OUT = os.environ.get("RDC_OUT") or (os.path.splitext(CAP)[0] + ".fwdcolour_audit.txt")
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
    def short(fmt): return (fmt or "").replace("DXGI_FORMAT_", "")
    def g(a, *names):
        for n in names:
            if n in a: return topy(a[n])
        return None

    cap = rd.OpenCaptureFile(); cap.OpenFile(CAP, '', None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None); P("replay", res, "capture", CAP)
    sf = ctl.GetStructuredFile()
    tex = {}; view = {}; dss = {}; bls = {}; buf = {}; il = {}; mem = {}
    st = dict(dss=None, bs=None, dsv=0, rtvs=[], vp=None, ps={}, cs={}, csu={}, vs={}, cb0=(0, 0, 0), ib=(0, None, 0),
              vbs={}, layout=0)
    ev = []
    in_frame = False
    for ci, ch in enumerate(sf.chunks):
        n = ch.name; a = args(ch)
        if n == "Internal::Beginning of Capture": in_frame = True; continue
        if n.endswith("::CreateTexture2D") or n.endswith("::CreateTexture2D1"):
            tex[topy(a["pTexture"])] = topy(a.get("Descriptor") or a.get("pDesc"))
        elif n.endswith("View") or n.endswith("View1"):
            if "::Create" in n and "pView" in a and "pResource" in a:
                kind = "dsv" if "DepthStencil" in n else ("rtv" if "RenderTarget" in n else ("uav" if "Unordered" in n else "srv"))
                view[topy(a["pView"])] = (kind, topy(a["pResource"]))
        if n.endswith("::CreateDepthStencilState"): dss[topy(a["pState"])] = topy(a.get("Descriptor") or a.get("pDepthStencilDesc"))
        elif n.endswith("::CreateBlendState") or n.endswith("::CreateBlendState1"): bls[topy(a["pState"])] = topy(a.get("Descriptor") or a.get("pBlendStateDesc"))
        elif n.endswith("::CreateBuffer"):
            bid = topy(a["pBuffer"]); buf[bid] = topy(a["pDesc"])
        elif n.endswith("::CreateInputLayout"): il[topy(a["pInputLayout"])] = topy(a["pInputElementDescs"])
        if n.endswith("::Unmap") and "MapWrittenData" in a:
            bid = topy(a["pResource"]); data = b""; o = a["MapWrittenData"]
            for getter in (lambda: o.data.basic.u, lambda: o.AsInt()):
                try:
                    data = bytes(sf.buffers[int(getter())])
                    if data: break
                except Exception: pass
            start = g(a, "Byte offset to start of written data") or 0
            if bid in buf:
                bw = buf[bid].get("ByteWidth", 0)
                m = mem.setdefault(bid, bytearray(bw))
                if data and start + len(data) <= len(m): m[start:start + len(data)] = data
        if not in_frame: continue
        if n.endswith("::OMSetDepthStencilState"): st["dss"] = topy(a["pDepthStencilState"])
        elif n.endswith("::OMSetBlendState"): st["bs"] = topy(a["pBlendState"])
        elif n.endswith("::OMSetRenderTargets") or n.endswith("::OMSetRenderTargetsAndUnorderedAccessViews"):
            if "ppRenderTargetViews" in a:
                st["rtvs"] = [x for x in (topy(a["ppRenderTargetViews"]) or []) if x]
            st["dsv"] = g(a, "pDepthStencilView") or 0
        elif n.endswith("::RSSetViewports"):
            vps = topy(a["pViewports"]); st["vp"] = vps[0] if vps else None
        elif n.endswith("SetShaderResources"):
            stage = n.split("::")[-1][:2]
            s0 = topy(a["StartSlot"]); vl = topy(a["ppShaderResourceViews"]) or []
            d = st["ps"] if stage == "PS" else (st["cs"] if stage == "CS" else (st["vs"] if stage == "VS" else None))
            if d is not None:
                for i, v in enumerate(vl): d[s0 + i] = v
        elif n.endswith("::CSSetUnorderedAccessViews"):
            s0 = topy(a["StartSlot"]); vl = topy(a["ppUnorderedAccessViews"]) or []
            for i, v in enumerate(vl): st["csu"][s0 + i] = v
        elif n.endswith("::VSSetConstantBuffers1") or n.endswith("::VSSetConstantBuffers"):
            if topy(a["StartSlot"]) == 0:
                b = topy(a["ppConstantBuffers"]); fc = g(a, "pFirstConstant") or [0]; nc = g(a, "pNumConstants") or [4096]
                st["cb0"] = (b[0] if b else 0, fc[0] if fc else 0, nc[0] if nc else 4096)
        elif n.endswith("::IASetIndexBuffer"): st["ib"] = (topy(a["pIndexBuffer"]), topy(a["Format"]), topy(a["Offset"]))
        elif n.endswith("::IASetVertexBuffers"):
            s0 = topy(a["StartSlot"]); vbl = topy(a["ppVertexBuffers"]); strd = topy(a["pStrides"]); offs = topy(a["pOffsets"])
            for i, v in enumerate(vbl): st["vbs"][s0 + i] = (v, strd[i] if i < len(strd) else 0, offs[i] if i < len(offs) else 0)
        elif n.endswith("::IASetInputLayout"): st["layout"] = topy(a["pInputLayout"])
        kind = None
        nm = n.split("::")[-1]
        if nm in ("DrawIndexed", "DrawIndexedInstanced", "Draw", "DrawInstanced", "Dispatch", "CopyResource",
                  "CopySubresourceRegion", "CopySubresourceRegion1", "DiscardView", "DiscardView1", "DiscardResource",
                  "ClearRenderTargetView", "ClearDepthStencilView", "GenerateMips", "ResolveSubresource", "Present"):
            kind = nm
        if kind is None: continue
        e = dict(ci=ci, kind=kind, rtvs=list(st["rtvs"]), dsv=st["dsv"], ps=dict(st["ps"]), cs=dict(st["cs"]),
                 csu=dict(st["csu"]), vp=st["vp"], dss=st["dss"], bs=st["bs"], cb0=st["cb0"], ib=st["ib"],
                 vbs=dict(st["vbs"]), layout=st["layout"])
        if kind == "DrawIndexed":
            e.update(ic=topy(a["IndexCount"]), si=topy(a["StartIndexLocation"]), bv=topy(a["BaseVertexLocation"]))
        elif kind in ("Draw",): e.update(ic=topy(a["VertexCount"]))
        elif kind == "DrawIndexedInstanced": e.update(ic=topy(a["IndexCountPerInstance"]), inst=topy(a["InstanceCount"]))
        elif kind == "CopyResource": e.update(dst=topy(a["pDstResource"]), src=topy(a["pSrcResource"]))
        elif kind.startswith("CopySubresourceRegion"): e.update(dst=topy(a["pDstResource"]), src=topy(a["pSrcResource"]))
        elif kind == "ResolveSubresource": e.update(dst=topy(a["pDstResource"]), src=topy(a["pSrcResource"]))
        elif kind.startswith("DiscardView"): e.update(view=g(a, "pResourceView"))
        elif kind == "DiscardResource": e.update(res=g(a, "pResource"))
        elif kind == "ClearRenderTargetView": e.update(view=g(a, "pRenderTargetView"))
        elif kind == "ClearDepthStencilView": e.update(view=g(a, "pDepthStencilView"))
        elif kind == "GenerateMips": e.update(view=g(a, "pShaderResourceView"))
        # the cb0 window as it was at this draw (rows 0..7)
        if kind == "DrawIndexed":
            b, first, num = st["cb0"]
            m = mem.get(b)
            if m is not None and num * 16 >= 128 and first * 16 + 128 <= len(m):
                e["rows"] = struct.unpack("<32f", bytes(m[first * 16:first * 16 + 128]))
        ev.append(e)

    def res_of(v): return view.get(v, (None, 0))[1]
    def tdesc(t): return tex.get(t, {})
    def tfmt(t): return short(tdesc(t).get("Format", "?"))
    def tsz(t): d = tdesc(t); return (d.get("Width", 0), d.get("Height", 0))

    # scene depth = the largest D32S8 DSV texture
    depths = {}
    for v, (k, t) in view.items():
        if k == "dsv" and any(tfmt(t).endswith(x) for x in DEPTH_FMTS_S): depths[t] = tsz(t)
    scene = max(depths, key=lambda t: depths[t][0] * depths[t][1]) if depths else 0
    SW, SH = depths.get(scene, (0, 0))
    P("scene depth R%d %dx%d" % (scene, SW, SH))
    # main forward pass(es): 1 RTV RGBA16F scene-size + scene DSV
    fwd = []
    for e in ev:
        if len(e["rtvs"]) == 1 and res_of(e["dsv"]) == scene:
            t = res_of(e["rtvs"][0])
            if tfmt(t).startswith("R16G16B16A16_FLOAT") and tsz(t) == (SW, SH) and e["kind"].startswith("Draw"):
                fwd.append((e["ci"], t))
    ftex = collections.Counter(t for _, t in fwd)
    P("main forward pass RTs (RGBA16F scene size on the scene DSV): %s" % dict(ftex))
    if not ftex: raise SystemExit("no forward pass found")
    F = ftex.most_common(1)[0][0]
    fv = [v for v, (k, t) in view.items() if t == F]
    P("forward colour R%d %s %dx%d mips %s bind %s misc %s views %s" % (F, tfmt(F), SW, SH, tdesc(F).get("MipLevels"),
      tdesc(F).get("BindFlags"), tdesc(F).get("MiscFlags"), [(v, view[v][0]) for v in fv]))
    f0 = min(ci for ci, t in fwd if t == F); f1 = max(ci for ci, t in fwd if t == F)
    P("forward draws chunks %d..%d (%d draws)" % (f0, f1, sum(1 for _, t in fwd if t == F)))

    # A. every event touching F (or the scene depth discard), from the first forward draw to the end of the frame
    P("\n== A. events touching the forward colour (and the scene depth discard / clears) from chunk %d" % f0)
    lastk = None; run = 0
    def touch(e):
        why = []
        if any(res_of(v) == F for v in e["rtvs"]): why.append("RTV")
        if e["kind"].startswith("Draw"):
            for s, v in e["ps"].items():
                if v and res_of(v) == F: why.append("PS-SRV t%d" % s)
            for s, v in e.get("vs", {}).items() if False else []: pass
        if e["kind"] == "Dispatch":
            for s, v in e["cs"].items():
                if v and res_of(v) == F: why.append("CS-SRV t%d" % s)
            for s, v in e["csu"].items():
                if v and res_of(v) == F: why.append("CS-UAV u%d" % s)
        if e.get("src") == F: why.append("COPY-SRC")
        if e.get("dst") == F: why.append("COPY-DST")
        if e.get("view") and res_of(e["view"]) == F: why.append(e["kind"])
        if e.get("res") == F: why.append("DiscardResource")
        if e.get("view") and res_of(e["view"]) == scene and e["kind"].startswith(("Discard", "ClearDepth")):
            why.append("SCENE-DEPTH " + e["kind"])
        return why
    rows = []
    for e in ev:
        if e["ci"] < f0: continue
        why = touch(e)
        if not why and e["kind"] != "Present": continue
        rts = ",".join("%s %dx%d" % (tfmt(res_of(v)), tsz(res_of(v))[0], tsz(res_of(v))[1]) for v in e["rtvs"])
        key = (e["kind"], tuple(why), rts)
        if key == lastk and e["kind"].startswith("Draw"):
            run += 1; continue
        if run: P("      ... x%d more" % run); run = 0
        lastk = key
        P("   chunk %6d %-22s %-40s RT=[%s] ic=%s" % (e["ci"], e["kind"], " ".join(why), rts, e.get("ic")))
        if e["kind"] == "Present": break
    if run: P("      ... x%d more" % run)

    # B. small world-layer forward draws (plates / decals)
    P("\n== B. small world-layer forward draws on F (IndexCount <= 60, viewport depth in [0.005, 0.95])")
    acts = {}
    def walk(al):
        for c in al:
            acts[c.eventId] = c
            if c.children: walk(c.children)
    walk(ctl.GetRootActions())
    by_chunk = {}
    for eid, ac in acts.items():
        for x in ac.events: by_chunk[x.chunkIndex] = eid
    small = [e for e in ev if e["kind"] == "DrawIndexed" and f0 <= e["ci"] <= f1 and len(e["rtvs"]) == 1 and
             res_of(e["rtvs"][0]) == F and e["vp"] and e["vp"]["MinDepth"] >= 0.005 and e["vp"]["MaxDepth"] <= 0.95 and
             e["ic"] <= 60]
    P("   %d draws" % len(small))
    def bdesc(b):
        d = bls.get(b)
        if not d: return "blend null"
        r = (d.get("RenderTarget") or [{}])[0]
        return "blend %s src %s dst %s" % ("on" if r.get("BlendEnable") else "off", str(r.get("SrcBlend", "?")).replace("D3D11_BLEND_", ""),
                                          str(r.get("DestBlend", "?")).replace("D3D11_BLEND_", ""))
    def ddesc(s):
        d = dss.get(s)
        if not d: return "dss null"
        return "depth %s write %s func %s" % (d.get("DepthEnable"), str(d.get("DepthWriteMask")).replace("D3D11_DEPTH_WRITE_MASK_", ""),
                                             str(d.get("DepthFunc")).replace("D3D11_COMPARISON_", ""))
    # cb0 rows 0..7 are read at replay time from the bound window (the ring's Map data is not in the chunk stream)
    for e in small:
        eid = by_chunk.get(e["ci"])
        line = "   chunk %d eid %s ic=%d cb0 win %d consts @%d | %s | %s" % (e["ci"], eid, e["ic"], e["cb0"][2], e["cb0"][1],
                                                                     bdesc(e["bs"]), ddesc(e["dss"]))
        if eid is None or not REPLAY:
            P(line); continue
        try:
            ctl.SetFrameEvent(eid, True)
            pst = ctl.GetPipelineState(); cb = pst.GetConstantBlock(rd.ShaderStage.Vertex, 0, 0)
            bd = cb.descriptor if hasattr(cb, "descriptor") else cb
            rid = bd.resource if hasattr(bd, "resource") else bd.resourceId
            r = struct.unpack("<32f", ctl.GetBufferData(rid, bd.byteOffset, 128))
            mv = r[0:16]; M = r[16:32]
            vo = (mv[3], mv[7], mv[11])
            line += (" | MV origin (view) %.2f %.2f %.2f |%.1f m| | MVP origin clip (%.2f %.2f %.2f w %.2f) | MV row norms "
                     "%.3f %.3f %.3f | MVP row0 %.3f %.3f %.3f row3 %.3f %.3f %.3f") % (
                vo[0], vo[1], vo[2], math.sqrt(sum(x * x for x in vo)), M[3], M[7], M[11], M[15],
                math.sqrt(sum(x * x for x in mv[0:3])), math.sqrt(sum(x * x for x in mv[4:7])), math.sqrt(sum(x * x for x in mv[8:11])),
                M[0], M[1], M[2], M[12], M[13], M[14])
            P(line)
            elems = il.get(e["layout"], []); pe = [x for x in elems if x["SemanticName"].upper() == "POSITION"]
            if not pe: P("      no POSITION"); continue
            pe = pe[0]; slot = pe["InputSlot"]; vb = e["vbs"].get(slot, (0, 0, 0)); fmtp = pe["Format"]
            ibid, ibf, iboff = e["ib"]; isz = 2 if str(ibf).endswith("R16_UINT") else 4
            post = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            outb = ctl.GetBufferData(post.vertexResourceId, post.vertexByteOffset, 0) if post.vertexResourceId != rd.ResourceId.Null() else b""
            d3 = ctl.GetD3D11PipelineState(); vbr = d3.inputAssembly.vertexBuffers[slot]
            vrid = vbr.resourceId if hasattr(vbr, "resourceId") else vbr.resource
            ibr = d3.inputAssembly.indexBuffer; irid = ibr.resourceId if hasattr(ibr, "resourceId") else ibr.resource
            k8 = min(8, e["ic"])
            ids_ = struct.unpack("<%d%s" % (k8, "H" if isz == 2 else "I"), ctl.GetBufferData(irid, iboff + e["si"] * isz, k8 * isz))
            oidx = None
            if post.indexResourceId != rd.ResourceId.Null() and post.indexByteStride:
                ob = ctl.GetBufferData(post.indexResourceId, post.indexByteOffset, k8 * post.indexByteStride)
                oidx = struct.unpack("<%d%s" % (k8, "H" if post.indexByteStride == 2 else "I"), ob[:k8 * post.indexByteStride])
            worst = 0.0; pmax = 0.0; nchk = 0; pos0 = None
            for k in range(k8):
                vi = ids_[k] + e["bv"]; raw_v = ctl.GetBufferData(vrid, vb[2] + vi * vb[1] + pe["AlignedByteOffset"], 16)
                if fmtp.endswith("R32G32B32_FLOAT") or fmtp.endswith("R32G32B32A32_FLOAT"): x, y, z = struct.unpack("<3f", raw_v[:12])
                elif fmtp.endswith("R16G16B16A16_FLOAT"): x, y, z = struct.unpack("<3e", raw_v[:6])
                else: P("      POSITION fmt %s" % fmtp); break
                if pos0 is None: pos0 = (x, y, z)
                pmax = max(pmax, abs(x), abs(y), abs(z))
                pos = (x, y, z, 1.0); clip = [sum(M[rr * 4 + c] * pos[c] for c in range(4)) for rr in range(4)]
                ok_ = oidx[k] if oidx else k; ob0 = ok_ * post.vertexByteStride
                if ob0 + 16 > len(outb): break
                o = struct.unpack("<4f", outb[ob0:ob0 + 16])
                worst = max(worst, max(abs(clip[i] - o[i]) / max(1.0, abs(o[3])) for i in range(4))); nchk += 1
            P("      POSITION %s, vb slot %d usage %s, first vertex (%.3f %.3f %.3f), max |pos| %.2f, %d verts: "
              "|SV_Position - MVP*pos| worst %.2e %s" % (short(fmtp), slot, buf.get(vb[0], {}).get("Usage"),
              pos0[0] if pos0 else 0, pos0[1] if pos0 else 0, pos0[2] if pos0 else 0, pmax, nchk, worst,
              "OK (rows 4..7 = this draw's MVP)" if nchk and worst < 1e-3 else "MISMATCH"))
        except Exception:
            P(line); P("      replay: %s" % traceback.format_exc().splitlines()[-1])
    ctl.Shutdown(); cap.Shutdown()
    P("\nDONE")
except SystemExit as x:
    P("STOP", x)
except Exception:
    P("FATAL", traceback.format_exc())
f.close()
os._exit(0)
