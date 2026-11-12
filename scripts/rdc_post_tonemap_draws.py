# RenderDoc dump for phase 82 (game_taa): the draws the game issues into the TONEMAP target after the tonemap draw and before
# the blit reads it (in the TAA capture frame15407: 2 x DrawIndexed, PS 674, EID 13108 / 13110, no texture inputs). For each:
# VS / PS (DXBC hash, disassembly), index / vertex counts, viewport, scissor, blend state, its constant buffers' first rows, inputs.
# Also, for the tonemap draw and the first G-buffer draws: the VS cb0 rows (projection: does it carry a sub-pixel jitter?).
# Run:  "C:\Program Files\RenderDoc\qrenderdoc.exe" --python scripts\rdc_post_tonemap_draws.py
# Environment: RDC_CAPTURE (required), RDC_OUT (default <capture>.post_tonemap.txt).
import os, traceback

CAP = os.environ.get("RDC_CAPTURE")
OUT = os.environ.get("RDC_OUT") or (os.path.splitext(CAP)[0] + ".post_tonemap.txt")
f = open(OUT, "w", buffering=1)
def P(*a): f.write(" ".join(str(x) for x in a) + "\n")
try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile()
    P("capture", CAP, cap.OpenFile(CAP, "", None))
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    P("replay", res)
    tex = {t.resourceId: t for t in ctl.GetTextures()}
    def td(rid):
        t = tex.get(rid)
        if not t: return "%s(buf)" % int(rid)
        return "%s %dx%d %s" % (int(rid), t.width, t.height, t.format.Name())
    acts = []
    def walk(a):
        for c in a:
            if c.children: walk(c.children)
            elif c.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch | rd.ActionFlags.Clear | rd.ActionFlags.Copy):
                acts.append(c)
    walk(ctl.GetRootActions())
    sf = ctl.GetStructuredFile()
    targets = ctl.GetDisassemblyTargets(True)
    info = []
    for a in acts:
        ctl.SetFrameEvent(a.eventId, False)
        st = ctl.GetPipelineState()
        outs = [o.resource for o in st.GetOutputTargets() if o.resource != rd.ResourceId.Null()]
        ins = [r.descriptor.resource for r in st.GetReadOnlyResources(rd.ShaderStage.Pixel) if r.descriptor.resource != rd.ResourceId.Null()]
        info.append((a, outs, ins))
    # the tonemap: RGBA8 scene-size out, RGBA16F same-size in; the last such draw of the frame
    tm = None
    for i, (a, outs, ins) in enumerate(info):
        if len(outs) != 1 or not (a.flags & rd.ActionFlags.Drawcall): continue
        o = tex.get(outs[0])
        if not o or "R8G8B8A8" not in o.format.Name(): continue
        for r in ins:
            t = tex.get(r)
            if t and t.format.Name().startswith("R16G16B16A16_FLOAT") and t.width == o.width and t.height == o.height:
                tm = i
    if tm is None:
        P("no tonemap draw found")
    else:
        tmRt = info[tm][1][0]
        P("tonemap: EID", info[tm][0].eventId, "->", td(tmRt), "reads", ", ".join(td(r) for r in info[tm][2]))
        def dump(a, full):
            ctl.SetFrameEvent(a.eventId, True)
            st = ctl.GetPipelineState()
            P("\n=========== EID %d %s  indices=%d instances=%d" % (a.eventId, a.GetName(sf), a.numIndices, a.numInstances))
            P("outputs:", ", ".join(td(o.resource) for o in st.GetOutputTargets() if o.resource != rd.ResourceId.Null()))
            try:
                vp = st.GetViewport(0); P("viewport %.1f,%.1f %.1fx%.1f" % (vp.x, vp.y, vp.width, vp.height))
            except Exception as e: P("viewport ?", e)
            try:
                for i, b in enumerate(st.GetColorBlends()[:1]):
                    P("blend[0] enabled=%s src=%s dst=%s op=%s | a src=%s dst=%s | mask=0x%x" % (b.enabled, b.colorBlend.source,
                      b.colorBlend.destination, b.colorBlend.operation, b.alphaBlend.source, b.alphaBlend.destination, b.writeMask))
            except Exception as e: P("blend ?", e)
            for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
                refl = st.GetShaderReflection(stage)
                if not refl: continue
                P("---- stage", stage, "shader", int(st.GetShader(stage)))
                for r in st.GetReadOnlyResources(stage):
                    if r.descriptor.resource != rd.ResourceId.Null(): P("  t%d: %s" % (r.access.index, td(r.descriptor.resource)))
                for i, cb in enumerate(refl.constantBlocks):
                    try:
                        d = st.GetConstantBlock(stage, i, 0).descriptor
                        vars_ = ctl.GetCBufferVariableContents(st.GetGraphicsPipelineObject(), refl.resourceId, stage, refl.entryPoint,
                                                                i, d.resource, d.byteOffset, d.byteSize)
                        n = 0
                        def dv(v, pre=""):
                            nonlocal_n = 0
                            if v.members:
                                for m in v.members: dv(m, pre + v.name + ".")
                            else:
                                P("  cb%d %s%s = %s" % (i, pre, v.name, [round(x, 6) for x in v.value.f32v[:v.columns * max(1, v.rows)]]))
                        for v in vars_[:24]: dv(v)
                    except Exception as e:
                        P("  cb%d read failed: %s" % (i, e))
                if full or stage == rd.ShaderStage.Pixel:
                    P(ctl.DisassembleShader(st.GetGraphicsPipelineObject(), refl, targets[0]))
        # every action after the tonemap that writes the tonemap target, up to the first one that reads it
        for a, outs, ins in info[tm + 1:]:
            if tmRt in ins:
                P("\nfirst reader of the tonemap target: EID", a.eventId, a.GetName(sf), "->", ", ".join(td(o) for o in outs))
                break
            if tmRt in outs: dump(a, True)
    # the first 3 G-buffer-like draws (4 render targets): their VS cb0 (the projection carries the jitter, if the game jitters it)
    n = 0
    for a, outs, ins in info:
        if len(outs) >= 4 and (a.flags & rd.ActionFlags.Drawcall) and n < 3:
            ctl.SetFrameEvent(a.eventId, True)
            st = ctl.GetPipelineState()
            refl = st.GetShaderReflection(rd.ShaderStage.Vertex)
            P("\n=========== G-buffer draw EID %d, VS %d" % (a.eventId, int(st.GetShader(rd.ShaderStage.Vertex))))
            try:
                vp = st.GetViewport(0); P("viewport %.4f,%.4f %.4fx%.4f" % (vp.x, vp.y, vp.width, vp.height))
            except Exception as e: P("viewport ?", e)
            if refl and refl.constantBlocks:
                d = st.GetConstantBlock(rd.ShaderStage.Vertex, 0, 0).descriptor
                vars_ = ctl.GetCBufferVariableContents(st.GetGraphicsPipelineObject(), refl.resourceId, rd.ShaderStage.Vertex,
                                                        refl.entryPoint, 0, d.resource, d.byteOffset, d.byteSize)
                for v in vars_[:12]:
                    P("  cb0 %s = %s" % (v.name, [round(x, 7) for x in v.value.f32v[:v.columns * max(1, v.rows)]]))
            n += 1
    ctl.Shutdown(); cap.Shutdown()
    P("done")
except Exception:
    P(traceback.format_exc())
f.close()
os._exit(0)
