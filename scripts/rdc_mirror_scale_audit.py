# RenderDoc audit for phase 53 (mirror render scale): can the mirror views be rendered into a SMALLER sub-rect of
# their own textures (viewport / scissor scaled by s) and DLSS-upscaled back to the full size?
#   "C:\Program Files\RenderDoc\qrenderdoc.exe" --python scripts\rdc_mirror_scale_audit.py
# For every draw bound to a mirror depth (the small D32S8 views, not the scene's) and the draws right after each
# mirror's DiscardView (the down-sample), it records: RT / depth sizes, viewport, scissor state, PS SRVs (size,
# format), and dumps every distinct VS / PS disassembly into <capture>.mirror_scale_dump\. For the lighting (n=2)
# draw, the forward draws that read a view-sized SRV and the down-sample it also dumps the bound constant buffers as
# floats (looking for the view size / inverse size). It also lists every later draw that reads a mirror texture.
import os, struct, traceback, collections, re
CAP = os.environ.get("RDC_CAPTURE") or r"E:\ETS2-DLAA-Injector\captures\ets2_flat_frame1969.rdc"
OUT = os.environ.get("RDC_OUT") or (os.path.splitext(CAP)[0] + ".mirror_scale_audit.txt")
DUMP = os.path.splitext(CAP)[0] + ".mirror_scale_dump"
os.makedirs(DUMP, exist_ok=True)
f = open(OUT, "w", buffering=1)
def P(*a): f.write(" ".join(str(x) for x in a) + "\n")
try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile(); cap.OpenFile(CAP, '', None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None); P("replay", res)
    texd = {int(t.resourceId): t for t in ctl.GetTextures()}
    bufd = {int(b.resourceId): b for b in ctl.GetBuffers()}
    def tdesc(rid):
        t = texd.get(int(rid))
        if not t: return None
        return (t.width, t.height, str(t.format.Name()) if hasattr(t.format, "Name") else str(t.format))
    def rid_of(d):
        for a in ("resource", "resourceId"):
            if hasattr(d, a): return getattr(d, a)
        if hasattr(d, "descriptor"): return rid_of(d.descriptor)
        return rd.ResourceId.Null()
    acts = []
    def walk(al):
        for c in al:
            if c.flags & rd.ActionFlags.Drawcall or c.flags & rd.ActionFlags.Clear or c.flags & rd.ActionFlags.Dispatch:
                acts.append(c)
            if c.children: walk(c.children)
    walk(ctl.GetRootActions())
    P("actions", len(acts))
    # D32S8 depth textures
    dts = {k: v for k, v in texd.items() if "D32" in (str(v.format.Name()) if hasattr(v.format, "Name") else "") or "R32G8X24" in (str(v.format.Name()) if hasattr(v.format, "Name") else "")}
    scene = max(dts, key=lambda k: dts[k].width * dts[k].height) if dts else 0
    P("depth textures:", sorted((k, dts[k].width, dts[k].height) for k in dts), "scene", scene)
    targets = ctl.GetDisassemblyTargets(True)
    shaders = {}
    def dis(stage_state, refl):
        sid = int(stage_state.resourceId) if hasattr(stage_state, "resourceId") else int(stage_state.shaderResourceId)
        if sid in shaders: return sid
        try:
            txt = ctl.DisassembleShader(rd.ResourceId.Null(), refl, targets[0] if targets else "")
        except Exception:
            txt = "disassembly failed: " + traceback.format_exc().splitlines()[-1]
        open(os.path.join(DUMP, "%d.txt" % sid), "w").write(txt)
        shaders[sid] = txt
        return sid
    def summary(txt):
        lines = [l for l in txt.splitlines() if re.match(r"^\s*\d+:", l)]
        dcl = [l.strip() for l in txt.splitlines() if l.strip().startswith("dcl_")]
        k = collections.Counter()
        for l in lines:
            m = re.match(r"^\s*\d+:\s*([a-z_0-9]+)", l)
            if m: k[m.group(1)] += 1
        svpos = any("position" in d and ("dcl_input_ps_siv" in d or "dcl_input_siv" in d) for d in dcl)
        return "%d inst, ops %s, SV_Position-in=%s, dcl %s" % (len(lines), dict((o, n) for o, n in k.items() if o.startswith(("sample", "ld", "resinfo", "gather", "discard"))), svpos, [d for d in dcl if d.startswith(("dcl_input", "dcl_constantbuffer", "dcl_resource", "dcl_output"))])
    def cbfloats(stage, slot, n=64):
        try:
            pst = ctl.GetPipelineState(); cb = pst.GetConstantBlock(stage, slot, 0)
            bd = cb.descriptor if hasattr(cb, "descriptor") else cb
            rid = rid_of(bd)
            if rid == rd.ResourceId.Null(): return None
            sz = getattr(bd, "byteSize", 0) or n * 4
            data = ctl.GetBufferData(rid, bd.byteOffset, min(sz, n * 4))
            fl = struct.unpack("<%df" % (len(data) // 4), data[:len(data) // 4 * 4])
            return " ".join(("%.6g" % x) for x in fl)
        except Exception:
            return "err " + traceback.format_exc().splitlines()[-1]
    def srvs(stage):
        out = []
        try:
            pst = ctl.GetPipelineState()
            for u in pst.GetReadOnlyResources(stage):
                d = u.descriptor if hasattr(u, "descriptor") else u
                rid = rid_of(d); idx = u.access.index if hasattr(u, "access") else -1
                if rid != rd.ResourceId.Null(): out.append((idx, int(rid), tdesc(rid)))
        except Exception:
            try:
                for i, r in enumerate(ctl.GetPipelineState().GetReadOnlyResources(stage)):
                    for b in r.resources:
                        if b.resourceId != rd.ResourceId.Null(): out.append((i, int(b.resourceId), tdesc(b.resourceId)))
            except Exception:
                out.append(("err", traceback.format_exc().splitlines()[-1]))
        return out
    mirror_tex = set()
    last_mirror_end = -1
    logged_full = 0
    view = None
    for i, a in enumerate(acts):
        ctl.SetFrameEvent(a.eventId, True)
        d3 = ctl.GetD3D11PipelineState()
        om = d3.outputMerger
        rts = [rid_of(v) for v in om.renderTargets]
        rts = [r for r in rts if r != rd.ResourceId.Null()]
        dt = rid_of(om.depthTarget)
        dti = int(dt)
        is_mirror = dti in dts and dti != scene and dts[dti].width >= 256 and not (dts[dti].width == 256 and dts[dti].height == 256)
        if dti == scene and view is not None:
            P("\n== SCENE begins at eid %d" % a.eventId); view = None
        if is_mirror:
            key = (dti, len(rts))
            if view is None or view != dti:
                P("\n== mirror view depth R%d %dx%d" % (dti, dts[dti].width, dts[dti].height)); view = dti
            for r in rts: mirror_tex.add(int(r))
        name = a.customName if hasattr(a, "customName") and a.customName else ""
        if not name:
            try: name = a.GetName(ctl.GetStructuredFile())
            except Exception: name = ""
        # draws right after a mirror view (down-sample): no mirror depth but reads a mirror texture
        ps_srv = srvs(rd.ShaderStage.Pixel)
        reads_mirror = [s for s in ps_srv if len(s) == 3 and s[1] in mirror_tex]
        if not is_mirror and not reads_mirror: continue
        vp = d3.rasterizer.viewports[0] if d3.rasterizer.viewports else None
        sc = d3.rasterizer.scissors[0] if d3.rasterizer.scissors else None
        scen = d3.rasterizer.state.scissorEnable if hasattr(d3.rasterizer.state, "scissorEnable") else "?"
        vsid = dis(d3.vertexShader, d3.vertexShader.reflection) if d3.vertexShader.reflection else 0
        psid = dis(d3.pixelShader, d3.pixelShader.reflection) if d3.pixelShader.reflection else 0
        tag = "MIR n=%d" % len(rts) if is_mirror else ("SCENE-READ" if view is None else "READ")
        P("  eid %d %s %s | RT %s | vp %s scissorEn %s sc %s | VS R%d PS R%d | PS SRV %s" % (
            a.eventId, tag, name[:40], [(int(r), tdesc(r)) for r in rts],
            (round(vp.x, 2), round(vp.y, 2), round(vp.width, 2), round(vp.height, 2), round(vp.minDepth, 3), round(vp.maxDepth, 3)) if vp else None,
            scen, (sc.x, sc.y, sc.width, sc.height) if sc else None, vsid, psid, ps_srv))
        big = (len(rts) == 2) or (not is_mirror) or any(s[2] and dts.get(dti) and s[2][0] == dts[dti].width and s[2][1] == dts[dti].height for s in ps_srv if len(s) == 3 and s[2])
        if big and logged_full < 400:
            logged_full += 1
            for st, nm in ((rd.ShaderStage.Vertex, "VS"), (rd.ShaderStage.Pixel, "PS")):
                for slot in range(0, 6):
                    c = cbfloats(st, slot)
                    if c and not c.startswith("err"): P("      %s cb%d: %s" % (nm, slot, c))
    P("\n== distinct shaders (summaries; full text in %s)" % DUMP)
    for sid, txt in shaders.items():
        P("  R%d: %s" % (sid, summary(txt)))
    P("\nDONE")
except Exception:
    P("FATAL", traceback.format_exc())
