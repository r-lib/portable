#define R_NO_REMAP

#include <R.h>
#include <Rinternals.h>
#include <R_ext/GraphicsEngine.h>
#include <R_ext/Rdynload.h>

#include <cstring>

#include "pdf_device.h"

static PortablePdfDevice* getDevice(pDevDesc dd) {
  return (PortablePdfDevice*)dd->deviceSpecific;
}

// ----------------------------------------------------------- GE callbacks

static void portable_activate(pDevDesc dd) {}
static void portable_deactivate(pDevDesc dd) {}
static void portable_mode(int mode, pDevDesc dd) {}
static void portable_onExit(pDevDesc dd) {}
static Rboolean portable_newFrameConfirm(pDevDesc dd) { return TRUE; }
static int portable_holdflush(pDevDesc dd, int level) { return 0; }

static void portable_close(pDevDesc dd) {
  PortablePdfDevice* dev = getDevice(dd);
  if (dev != nullptr) {
    dev->close();
    delete dev;
    dd->deviceSpecific = nullptr;
  }
}

static void portable_clip(double x0, double x1, double y0, double y1,
                          pDevDesc dd) {
  getDevice(dd)->clip(x0, x1, y0, y1);
}

static void portable_size(double* left, double* right, double* bottom,
                          double* top, pDevDesc dd) {
  *left = dd->left;
  *right = dd->right;
  *bottom = dd->bottom;
  *top = dd->top;
}

static void portable_newPage(const pGEcontext gc, pDevDesc dd) {
  getDevice(dd)->newPage(gc->fill);
}

static void portable_line(double x1, double y1, double x2, double y2,
                          const pGEcontext gc, pDevDesc dd) {
  getDevice(dd)->line(x1, y1, x2, y2, gc);
}

static void portable_polyline(int n, double* x, double* y,
                              const pGEcontext gc, pDevDesc dd) {
  getDevice(dd)->polyline(n, x, y, gc);
}

static void portable_polygon(int n, double* x, double* y,
                             const pGEcontext gc, pDevDesc dd) {
  getDevice(dd)->polygon(n, x, y, gc);
}

static void portable_path(double* x, double* y, int npoly, int* nper,
                          Rboolean winding, const pGEcontext gc, pDevDesc dd) {
  getDevice(dd)->path(x, y, npoly, nper, winding, gc);
}

static void portable_rect(double x0, double y0, double x1, double y1,
                          const pGEcontext gc, pDevDesc dd) {
  getDevice(dd)->rect(x0, y0, x1, y1, gc);
}

static void portable_circle(double x, double y, double r, const pGEcontext gc,
                            pDevDesc dd) {
  getDevice(dd)->circle(x, y, r, gc);
}

static void portable_raster(unsigned int* raster, int w, int h, double x,
                            double y, double width, double height, double rot,
                            Rboolean interpolate, const pGEcontext gc,
                            pDevDesc dd) {
  getDevice(dd)->raster(raster, w, h, x, y, width, height, rot, interpolate,
                        gc);
}

static void portable_text(double x, double y, const char* str, double rot,
                          double hadj, const pGEcontext gc, pDevDesc dd) {
  getDevice(dd)->text(x, y, str, rot, hadj, gc);
}

static double portable_strWidth(const char* str, const pGEcontext gc,
                                pDevDesc dd) {
  return getDevice(dd)->strWidth(str, gc);
}

static void portable_metricInfo(int c, const pGEcontext gc, double* ascent,
                                double* descent, double* width, pDevDesc dd) {
  getDevice(dd)->metricInfo(c, gc, ascent, descent, width);
}

#if R_GE_version >= 13
static SEXP portable_setPattern(SEXP pattern, pDevDesc dd) {
  return getDevice(dd)->setPattern(pattern);
}
static void portable_releasePattern(SEXP ref, pDevDesc dd) {
  getDevice(dd)->releasePattern(ref);
}
static SEXP portable_setClipPath(SEXP path, SEXP ref, pDevDesc dd) {
  return getDevice(dd)->setClipPath(path, ref);
}
static void portable_releaseClipPath(SEXP ref, pDevDesc dd) {
  getDevice(dd)->releaseClipPath(ref);
}
static SEXP portable_setMask(SEXP mask, SEXP ref, pDevDesc dd) {
  return getDevice(dd)->setMask(mask, ref);
}
static void portable_releaseMask(SEXP ref, pDevDesc dd) {
  getDevice(dd)->releaseMask(ref);
}
#endif

#if R_GE_version >= 15
static SEXP portable_defineGroup(SEXP source, int op, SEXP destination,
                                 pDevDesc dd) {
  return getDevice(dd)->defineGroup(source, op, destination);
}
static void portable_useGroup(SEXP ref, SEXP trans, pDevDesc dd) {
  getDevice(dd)->useGroup(ref, trans);
}
static void portable_releaseGroup(SEXP ref, pDevDesc dd) {
  getDevice(dd)->releaseGroup(ref);
}
static void portable_stroke_fn(SEXP path, const pGEcontext gc, pDevDesc dd) {
  getDevice(dd)->strokePath(path, gc);
}
static void portable_fill_fn(SEXP path, int rule, const pGEcontext gc,
                             pDevDesc dd) {
  getDevice(dd)->fillPath(path, rule, gc);
}
static void portable_fillStroke_fn(SEXP path, int rule, const pGEcontext gc,
                                   pDevDesc dd) {
  getDevice(dd)->fillStrokePath(path, rule, gc);
}
static SEXP portable_capabilities(SEXP cap) {
  SEXP x;
  PROTECT(x = Rf_allocVector(INTSXP, 3));
  INTEGER(x)[0] = R_GE_linearGradientPattern;
  INTEGER(x)[1] = R_GE_radialGradientPattern;
  INTEGER(x)[2] = R_GE_tilingPattern;
  SET_VECTOR_ELT(cap, R_GE_capability_patterns, x);
  UNPROTECT(1);

  PROTECT(x = Rf_allocVector(INTSXP, 1));
  INTEGER(x)[0] = 1;
  SET_VECTOR_ELT(cap, R_GE_capability_clippingPaths, x);
  UNPROTECT(1);

  PROTECT(x = Rf_allocVector(INTSXP, 2));
  INTEGER(x)[0] = R_GE_alphaMask;
  INTEGER(x)[1] = R_GE_luminanceMask;
  SET_VECTOR_ELT(cap, R_GE_capability_masks, x);
  UNPROTECT(1);

  // PDF blend modes, "over", and the Porter-Duff operators that are
  // synthesizable from alpha soft masks (all except add and saturate)
  PROTECT(x = Rf_allocVector(INTSXP, 23));
  INTEGER(x)[0] = R_GE_compositeClear;
  INTEGER(x)[1] = R_GE_compositeSource;
  INTEGER(x)[2] = R_GE_compositeOver;
  INTEGER(x)[3] = R_GE_compositeIn;
  INTEGER(x)[4] = R_GE_compositeOut;
  INTEGER(x)[5] = R_GE_compositeAtop;
  INTEGER(x)[6] = R_GE_compositeDest;
  INTEGER(x)[7] = R_GE_compositeDestOver;
  INTEGER(x)[8] = R_GE_compositeDestIn;
  INTEGER(x)[9] = R_GE_compositeDestOut;
  INTEGER(x)[10] = R_GE_compositeDestAtop;
  INTEGER(x)[11] = R_GE_compositeXor;
  INTEGER(x)[12] = R_GE_compositeMultiply;
  INTEGER(x)[13] = R_GE_compositeScreen;
  INTEGER(x)[14] = R_GE_compositeOverlay;
  INTEGER(x)[15] = R_GE_compositeDarken;
  INTEGER(x)[16] = R_GE_compositeLighten;
  INTEGER(x)[17] = R_GE_compositeColorDodge;
  INTEGER(x)[18] = R_GE_compositeColorBurn;
  INTEGER(x)[19] = R_GE_compositeHardLight;
  INTEGER(x)[20] = R_GE_compositeSoftLight;
  INTEGER(x)[21] = R_GE_compositeDifference;
  INTEGER(x)[22] = R_GE_compositeExclusion;
  SET_VECTOR_ELT(cap, R_GE_capability_compositing, x);
  UNPROTECT(1);

  PROTECT(x = Rf_allocVector(INTSXP, 1));
  INTEGER(x)[0] = 1;
  SET_VECTOR_ELT(cap, R_GE_capability_transformations, x);
  UNPROTECT(1);

  PROTECT(x = Rf_allocVector(INTSXP, 1));
  INTEGER(x)[0] = 1;
  SET_VECTOR_ELT(cap, R_GE_capability_paths, x);
  UNPROTECT(1);

#if R_GE_version >= 16
  PROTECT(x = Rf_allocVector(INTSXP, 1));
  INTEGER(x)[0] = 1;
  SET_VECTOR_ELT(cap, R_GE_capability_glyphs, x);
  UNPROTECT(1);
#endif

  return cap;
}
#endif

#if R_GE_version >= 16
static void portable_glyph(int n, int* glyphs, double* x, double* y, SEXP font,
                           double size, int colour, double rot, pDevDesc dd) {
  getDevice(dd)->glyph(n, glyphs, x, y, font, size, colour, rot);
}
#endif

// ------------------------------------------------------------ device setup

static pDevDesc portable_dev_new(PortablePdfDevice* dev, double pointsize,
                                 double scaling, const int* bg) {
  pDevDesc dd = (DevDesc*)calloc(1, sizeof(DevDesc));
  if (dd == nullptr) return nullptr;

  double res = 72.0 * scaling;
  dd->left = 0;
  dd->right = dev->pageWidth();
  dd->bottom = 0;
  dd->top = dev->pageHeight();
  dd->clipLeft = dd->left;
  dd->clipRight = dd->right;
  dd->clipBottom = dd->bottom;
  dd->clipTop = dd->top;

  dd->cra[0] = 0.9 * pointsize * scaling;
  dd->cra[1] = 1.2 * pointsize * scaling;
  // ipr is inches per device unit (see GEcvt), not device units per inch
  dd->ipr[0] = dd->ipr[1] = 1.0 / res;
  dd->yLineBias = 0.2;

  dd->gamma = 1.0;
  dd->canHAdj = 2;
  dd->canChangeGamma = FALSE;
  dd->displayListOn = FALSE;
  dd->haveTransparency = 2;   // yes
  dd->haveTransparentBg = 3;  // yes, including semi-transparent
  dd->haveRaster = 2;       // yes, with full alpha
  dd->haveCapture = 1;      // no
  dd->haveLocator = 1;      // no
  dd->canClip = TRUE;
#if R_GE_version >= 14
  // The device handles all clipping itself (rect and path based)
  dd->deviceClip = TRUE;
#endif
  dd->canGenMouseDown = FALSE;
  dd->canGenMouseMove = FALSE;
  dd->canGenMouseUp = FALSE;
  dd->canGenKeybd = FALSE;
  dd->canGenIdle = FALSE;
  dd->gettingEvent = FALSE;
  dd->hasTextUTF8 = TRUE;
  dd->wantSymbolUTF8 = TRUE;
  dd->useRotatedTextInContour = TRUE;

  dd->startps = pointsize;
  dd->startcol = R_RGB(0, 0, 0);
  dd->startfill = R_RGBA(bg[0], bg[1], bg[2], bg[3]);
  dd->startlty = LTY_SOLID;
  dd->startfont = 1;
  dd->startgamma = 1.0;

  dd->activate = portable_activate;
  dd->deactivate = portable_deactivate;
  dd->close = portable_close;
  dd->clip = portable_clip;
  dd->size = portable_size;
  dd->newPage = portable_newPage;
  dd->line = portable_line;
  dd->text = portable_text;
  dd->strWidth = portable_strWidth;
  dd->rect = portable_rect;
  dd->circle = portable_circle;
  dd->polygon = portable_polygon;
  dd->polyline = portable_polyline;
  dd->path = portable_path;
  dd->raster = portable_raster;
  dd->mode = portable_mode;
  dd->metricInfo = portable_metricInfo;
  dd->cap = nullptr;
  dd->onExit = portable_onExit;
  dd->newFrameConfirm = portable_newFrameConfirm;
  dd->textUTF8 = portable_text;
  dd->strWidthUTF8 = portable_strWidth;
  dd->holdflush = portable_holdflush;

  dd->deviceVersion = R_GE_version;
#if R_GE_version >= 13
  dd->setPattern = portable_setPattern;
  dd->releasePattern = portable_releasePattern;
  dd->setClipPath = portable_setClipPath;
  dd->releaseClipPath = portable_releaseClipPath;
  dd->setMask = portable_setMask;
  dd->releaseMask = portable_releaseMask;
#endif
#if R_GE_version >= 15
  dd->defineGroup = portable_defineGroup;
  dd->useGroup = portable_useGroup;
  dd->releaseGroup = portable_releaseGroup;
  dd->stroke = portable_stroke_fn;
  dd->fill = portable_fill_fn;
  dd->fillStroke = portable_fillStroke_fn;
  dd->capabilities = portable_capabilities;
#endif
#if R_GE_version >= 16
  dd->glyph = portable_glyph;
#endif

  dd->deviceSpecific = dev;
  return dd;
}

extern "C" SEXP portable_pdf_c(SEXP file, SEXP width, SEXP height,
                               SEXP scaling, SEXP pointsize, SEXP family,
                               SEXP bg, SEXP title, SEXP author, SEXP creator,
                               SEXP subject, SEXP keywords, SEXP creation_date,
                               SEXP colormodel, SEXP one_file_per_page) {
  const char* filename = R_ExpandFileName(CHAR(STRING_ELT(file, 0)));
  double w = REAL(width)[0];
  double h = REAL(height)[0];
  double sc = REAL(scaling)[0];
  double ps = REAL(pointsize)[0];
  const char* fam = CHAR(STRING_ELT(family, 0));
  int* bg_rgba = INTEGER(bg);
  const char* ttl = CHAR(STRING_ELT(title, 0));
  const char* auth = CHAR(STRING_ELT(author, 0));
  const char* crt = CHAR(STRING_ELT(creator, 0));
  const char* sub = CHAR(STRING_ELT(subject, 0));
  const char* kwd = CHAR(STRING_ELT(keywords, 0));
  const char* cdt = CHAR(STRING_ELT(creation_date, 0));
  const char* cm = CHAR(STRING_ELT(colormodel, 0));
  bool per_page = Rf_asLogical(one_file_per_page) == TRUE;

  if (w <= 0 || h <= 0) Rf_error("width and height must be positive");
  if (sc <= 0) Rf_error("scaling must be positive");

  ColorModel model = ColorModel::kRGB;
  if (std::strcmp(cm, "gray") == 0) model = ColorModel::kGray;
  if (std::strcmp(cm, "cmyk") == 0) model = ColorModel::kCMYK;

  PortablePdfDevice* dev =
      new PortablePdfDevice(filename, w, h, sc, fam, ttl, auth, crt, sub, kwd,
                            cdt, model, per_page);
  pDevDesc dd = portable_dev_new(dev, ps, sc, bg_rgba);
  if (dd == nullptr) {
    delete dev;
    Rf_error("Failed to allocate device");
  }
  pGEDevDesc gdd = GEcreateDevDesc(dd);
  GEaddDevice2(gdd, "portable_pdf");

  return file;
}

static const R_CallMethodDef CallEntries[] = {
    {"C_portable_pdf", (DL_FUNC)&portable_pdf_c, 15},
    {nullptr, nullptr, 0}};

extern "C" void R_init_portable(DllInfo* dll) {
  R_registerRoutines(dll, nullptr, CallEntries, nullptr, nullptr);
  R_useDynamicSymbols(dll, FALSE);
}
