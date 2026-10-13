# Phase 53 follow-up to rdc_mirror_scale_audit.py: the full PS cb0 of the mirror lighting draws (PS with cb0[65]) and of the
# main scene's draws with the same pixel shader, rows 56..64 (where the lighting PS reads its texel scale cb0[61] and its view-ray
# rows cb0[60] / cb0[63]).
#   "C:\Program Files\RenderDoc\qrenderdoc.exe" --python scripts\rdc_mirror_scale_cb.py
import os, struct, traceback
CAP = os.environ.get("RDC_CAPTURE") or r"E:\ETS2-DLAA-Injector\captures\ets2_flat_frame1969.rdc"
OUT = os.path.splitext(CAP)[0] + ".mirror_scale_cb.txt"
EIDS = [int(x) for x in (os.environ.get("RDC_EIDS") or "7278,8770,10161").split(",")]
f = open(OUT, "w", buffering=1)
def P(*a): f.write(" ".join(str(x) for x in a) + "\n")
try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile(); cap.OpenFile(CAP, '', None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None); P("replay", res)
    def rid_of(d):
        for a in ("resource", "resourceId"):
            if hasattr(d, a): return getattr(d, a)
        return rd.ResourceId.Null()
    def dump(eid):
        ctl.SetFrameEvent(eid, True)
        d3 = ctl.GetD3D11PipelineState()
        psid = int(d3.pixelShader.resourceId)
        vp = d3.rasterizer.viewports[0]
        pst = ctl.GetPipelineState(); cb = pst.GetConstantBlock(rd.ShaderStage.Pixel, 0, 0)
        bd = cb.descriptor if hasattr(cb, "descriptor") else cb
        data = ctl.GetBufferData(rid_of(bd), bd.byteOffset, 65 * 16)
        fl = struct.unpack("<%df" % (len(data) // 4), data[:len(data) // 4 * 4])
        P("eid %d PS R%d vp %.0fx%.0f" % (eid, psid, vp.width, vp.height))
        for r in range(56, min(65, len(fl) // 4)):
            P("   cb0[%d] %s" % (r, " ".join("%.6g" % x for x in fl[r * 4:r * 4 + 4])))
        return psid
    ps = None
    for e in EIDS: ps = dump(e)
    # the main scene's draws with the same PS (lighting of the scene)
    acts = []
    def walk(al):
        for c in al:
            if c.flags & rd.ActionFlags.Drawcall: acts.append(c)
            if c.children: walk(c.children)
    walk(ctl.GetRootActions())
    n = 0
    for a in acts:
        if a.eventId <= max(EIDS) or n >= 3: continue
        ctl.SetFrameEvent(a.eventId, True)
        if int(ctl.GetD3D11PipelineState().pixelShader.resourceId) == ps:
            P("-- main scene draw with the same PS:"); dump(a.eventId); n += 1
    P("DONE")
except Exception:
    P("FATAL", traceback.format_exc())
