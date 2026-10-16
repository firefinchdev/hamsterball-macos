"""Patch gl4es for the browser build (idempotent). Usage: python3 patch_gl4es.py <gl4es source dir>

WebGL 1 has packed depth/stencil in core (DEPTH_STENCIL renderbuffers on the
DEPTH_STENCIL_ATTACHMENT point) but doesn't advertise GL_OES_packed_depth_stencil, so gl4es
falls back to separate depth and stencil buffers, which WebGL 1 refuses. Only __EMSCRIPTEN__
builds change; Android is unaffected."""
import os
import sys

path = os.path.join(sys.argv[1], "src/gl/framebuffers.c")
src = open(path).read()
marker = "hbwrap: WebGL packed depth/stencil"
if marker not in src:
    edits = [
        ("""    // check if internal format is GL_DEPTH_STENCIL_EXT
    if (internalformat == GL_DEPTH_STENCIL)
        internalformat = GL_DEPTH24_STENCIL8;""",
         """    // check if internal format is GL_DEPTH_STENCIL_EXT
#ifdef __EMSCRIPTEN__
    // %s
    if (internalformat == GL_DEPTH24_STENCIL8)
        internalformat = GL_DEPTH_STENCIL;
    else
#endif
    if (internalformat == GL_DEPTH_STENCIL)
        internalformat = GL_DEPTH24_STENCIL8;""" % marker),
        ("""    if (attachment==GL_DEPTH_STENCIL_ATTACHMENT) {
        // doesn't seems to be supported "as-is" on GLES on PVR""",
         """    if (attachment==GL_DEPTH_STENCIL_ATTACHMENT) {
#ifdef __EMSCRIPTEN__
        // WebGL 1: a DEPTH_STENCIL renderbuffer goes on the combined attachment point
        SetAttachment(fb, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, renderbuffer, 0);
        SetAttachment(fb, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, renderbuffer, 0);
        GLenum ntarget = ReadDraw_Push(target);
        errorGL();
        gles_glFramebufferRenderbuffer(ntarget, attachment, renderbuffertarget, renderbuffer);
        ReadDraw_Pop(target);
        return;
#endif
        // doesn't seems to be supported "as-is" on GLES on PVR"""),
    ]
    for old, new in edits:
        assert src.count(old) == 1, old
        src = src.replace(old, new)
    open(path, "w").write(src)
    print("patched", path)
