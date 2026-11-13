#include "pdf_device.h"

#include <systemfonts.h>
#include <textshaping.h>

#include <cmath>
#include <cstring>

static const double DEG_TO_RAD = M_PI / 180.0;

PortablePdfDevice::PortablePdfDevice(const std::string& file, double width,
                                     double height, double scaling,
                                     const std::string& family,
                                     const std::string& title,
                                     const std::string& author,
                                     const std::string& creator,
                                     const std::string& subject,
                                     const std::string& keywords,
                                     const std::string& creation_date,
                                     ColorModel colormodel_,
                                     bool one_file_per_page, bool compress)
    : doc(file, scaling, one_file_per_page, compress),
      page_width(width * 72.0 * scaling),
      page_height(height * 72.0 * scaling),
      scale(scaling > 0 ? scaling : 1.0),
      default_family(family),
      colormodel(colormodel_) {
  doc.setInfo(title, author, creator, subject, keywords, creation_date);
}

void PortablePdfDevice::newPage(unsigned int fill) {
  if (has_page) doc.endPage();
  doc.beginPage(page_width / (72.0 * scale), page_height / (72.0 * scale));
  has_page = true;
  ever_had_page = true;
  clipped = false;
  path_clip_active = false;
  current_mask = -1;
  if (R_ALPHA(fill) != 0) {
    std::ostringstream& s = doc.stream();
    setFillColor(s, fill);
    if (R_ALPHA(fill) != 255) {
      s << "/" << doc.addGState(1.0, R_ALPHA(fill) / 255.0) << " gs\n";
    }
    s << "0 0 " << pdf_num(page_width) << " " << pdf_num(page_height)
      << " re f\n";
  }
}

void PortablePdfDevice::close() {
  if (has_page) {
    doc.endPage();
    has_page = false;
  }
  // A page-less PDF is rejected by some viewers; emit one blank page
  if (!ever_had_page) {
    doc.beginPage(page_width / (72.0 * scale), page_height / (72.0 * scale));
    doc.endPage();
  }
  if (!doc.finish()) {
    Rf_warning("failed to write the PDF file");
  }
}

void PortablePdfDevice::clip(double x0, double x1, double y0, double y1) {
  std::ostringstream& s = doc.stream();
  if (appending) {
    // Recording a path: a clip here can only shrink the recorded region
    s << pdf_num(x0) << " " << pdf_num(y0) << " " << pdf_num(x1 - x0) << " "
      << pdf_num(y1 - y0) << " re W n\n";
    return;
  }
  // Reset to the enclosing graphics state before applying the new clip
  if (clipped || path_clip_active) s << "Q q\n";
  s << pdf_num(x0) << " " << pdf_num(y0) << " " << pdf_num(x1 - x0) << " "
    << pdf_num(y1 - y0) << " re W n\n";
  clipped = true;
  path_clip_active = false;
  clip_x0 = x0;
  clip_x1 = x1;
  clip_y0 = y0;
  clip_y1 = y1;
  applyMask();
}

// ------------------------------------------------------- graphics state

void PortablePdfDevice::setStrokeColor(std::ostringstream& s,
                                       unsigned int col) {
  double r = R_RED(col) / 255.0, g = R_GREEN(col) / 255.0,
         b = R_BLUE(col) / 255.0;
  switch (colormodel) {
    case ColorModel::kGray:
      s << pdf_num(0.2126 * r + 0.7152 * g + 0.0722 * b) << " G\n";
      break;
    case ColorModel::kCMYK: {
      double k = 1.0 - std::max(r, std::max(g, b));
      double d = 1.0 - k;
      double c = d > 0 ? (d - r) / d : 0.0;
      double m = d > 0 ? (d - g) / d : 0.0;
      double y = d > 0 ? (d - b) / d : 0.0;
      s << pdf_num(c) << " " << pdf_num(m) << " " << pdf_num(y) << " "
        << pdf_num(k) << " K\n";
      break;
    }
    default:
      s << pdf_num(r) << " " << pdf_num(g) << " " << pdf_num(b) << " RG\n";
  }
}

void PortablePdfDevice::setFillColor(std::ostringstream& s, unsigned int col) {
  double r = R_RED(col) / 255.0, g = R_GREEN(col) / 255.0,
         b = R_BLUE(col) / 255.0;
  switch (colormodel) {
    case ColorModel::kGray:
      s << pdf_num(0.2126 * r + 0.7152 * g + 0.0722 * b) << " g\n";
      break;
    case ColorModel::kCMYK: {
      double k = 1.0 - std::max(r, std::max(g, b));
      double d = 1.0 - k;
      double c = d > 0 ? (d - r) / d : 0.0;
      double m = d > 0 ? (d - g) / d : 0.0;
      double y = d > 0 ? (d - b) / d : 0.0;
      s << pdf_num(c) << " " << pdf_num(m) << " " << pdf_num(y) << " "
        << pdf_num(k) << " k\n";
      break;
    }
    default:
      s << pdf_num(r) << " " << pdf_num(g) << " " << pdf_num(b) << " rg\n";
  }
}

static bool blankLty(int lty) {
  return lty == -1 || lty == NA_INTEGER;
}

void PortablePdfDevice::setStroke(const pGEcontext gc) {
  std::ostringstream& s = doc.stream();
  setStrokeColor(s, gc->col);

  double lwd = gc->lwd * 0.75 * scale;  // lwd is in 1/96 in
  s << pdf_num(lwd) << " w\n";

  if (gc->lty == 0 || blankLty(gc->lty)) {
    s << "[] 0 d\n";
  } else {
    // Collect the lty nibbles (pen down/up lengths in units of lwd)
    double nibbles[8];
    int nlty = 0;
    for (int i = 0; i < 8 && (gc->lty >> (i * 4)) & 0xF; i++) {
      nibbles[nlty++] = (gc->lty >> (i * 4)) & 0xF;
    }
    // PDF dashes are drawn including the line cap, so an "on" segment of
    // nominal length L inks L + lwd with round/square caps. Adjust like the
    // pdf()/ps() devices: shorten "on" segments and lengthen "off" segments
    // by one lwd unless caps are butt, clamping at zero. The single-element
    // "1" case is kept at 1 so dotted stays dotted with square caps.
    double a = (gc->lend == GE_BUTT_CAP) ? 0.0 : 1.0;
    double dash[8];
    bool allzero = true;
    for (int i = 0; i < nlty; i++) {
      dash[i] = lwd * ((i % 2)
                           ? (nibbles[i] + a)
                           : ((nlty == 1 && nibbles[i] == 1.0) ? 1.0
                                                               : nibbles[i] - a));
      if (dash[i] < 0) dash[i] = 0;
      if (dash[i] > 0.01) allzero = false;
    }
    // An all-zero dash array is a PDF error; solid + round caps gives dots
    if (allzero) {
      s << "[] 0 d\n";
    } else {
      s << "[";
      for (int i = 0; i < nlty; i++) {
        s << pdf_num(dash[i]) << " ";
      }
      s << "] 0 d\n";
    }
  }

  int cap = 0;  // GE_BUTT_CAP
  if (gc->lend == GE_ROUND_CAP) cap = 1;
  if (gc->lend == GE_SQUARE_CAP) cap = 2;
  s << cap << " J\n";
  int join = 0;
  if (gc->ljoin == GE_ROUND_JOIN) join = 1;
  if (gc->ljoin == GE_BEVEL_JOIN) join = 2;
  s << join << " j " << pdf_num(gc->lmitre) << " M\n";
}

void PortablePdfDevice::setFill(const pGEcontext gc) {
  std::ostringstream& s = doc.stream();
#if R_GE_version >= 13
  if (gc->patternFill != R_NilValue) {
    int idx = INTEGER(gc->patternFill)[0];
    if (idx >= 0 && idx < (int)patterns.size() &&
        !patterns[idx].name.empty()) {
      s << "/Pattern cs /" << patterns[idx].name << " scn\n";
      PatternEntry& pe = patterns[idx];
      if (!pe.alpha_gs.empty()) {
        if (current_mask >= 0 && current_mask < (int)masks.size() &&
            !masks[current_mask].empty()) {
          // A gstate carries a single SMask, so the gradient's alpha mask and
          // the active user mask are combined by painting the alpha shading
          // through the user mask inside a new soft-mask form
          auto it = pe.masked_gs.find(current_mask);
          if (it == pe.masked_gs.end()) {
            std::string fstream = "q\n/" + masks[current_mask] + " gs\n/" +
                                  pe.alpha_sh + " sh\nQ\n";
            std::string form = doc.addForm(fstream, true);
            it = pe.masked_gs
                     .emplace(current_mask,
                              doc.addGState(1.0, 1.0, "", form, true))
                     .first;
          }
          s << "/" << it->second << " gs\n";
        } else {
          s << "/" << pe.alpha_gs << " gs\n";
        }
      }
      return;
    }
  }
#endif
  setFillColor(s, gc->fill);
}

// Combined paint: emits geometry exactly once per pass, in separate passes
// when stroke and fill alphas differ (PDF has one paint op per alpha pair).
void PortablePdfDevice::paint(const std::string& path_ops,
                              const pGEcontext gc, bool winding) {
  bool stroke = R_ALPHA(gc->col) != 0 && !blankLty(gc->lty);
  bool pattern = false;
  bool pattern_alpha = false;
#if R_GE_version >= 13
  pattern = gc->patternFill != R_NilValue;
  if (pattern) {
    int idx = INTEGER(gc->patternFill)[0];
    pattern_alpha = idx >= 0 && idx < (int)patterns.size() &&
                    !patterns[idx].alpha_gs.empty();
  }
#endif
  bool fill = pattern || R_ALPHA(gc->fill) != 0;
  if (!stroke && !fill) return;

  std::ostringstream& s = doc.stream();
  double sa = R_ALPHA(gc->col) / 255.0;
  // Pattern fills carry their own alpha; the fill gstate alpha does not apply
  double fa = pattern ? 1.0 : R_ALPHA(gc->fill) / 255.0;
  const char* fop = winding ? "f" : "f*";

  // An alpha-gradient fill is painted through its own soft mask, which must
  // not cover the stroke, so it always gets its own pass
  if (stroke && fill && (sa != fa || pattern_alpha)) {
    s << "q\n";
    setFill(gc);
    if (fa != 1.0) s << "/" << doc.addGState(1.0, fa) << " gs\n";
    s << path_ops << fop << "\nQ\n";
    s << "q\n";
    setStroke(gc);
    s << "/" << doc.addGState(sa, 1.0) << " gs\n";
    s << path_ops << "S\nQ\n";
  } else {
    s << "q\n";
    if (fill) setFill(gc);
    if (stroke) setStroke(gc);
    if ((stroke && sa != 1.0) || (fill && fa != 1.0)) {
      s << "/" << doc.addGState(stroke ? sa : 1.0, fill ? fa : 1.0)
        << " gs\n";
    }
    const char* op = "S";
    if (fill && stroke) op = winding ? "B" : "B*";
    if (fill && !stroke) op = fop;
    s << path_ops << op << "\nQ\n";
  }
}

// -------------------------------------------------------------- geometry

void PortablePdfDevice::emitPath(std::ostringstream& s, int n, const double* x,
                                 const double* y, bool close) {
  s << pdf_num(x[0]) << " " << pdf_num(y[0]) << " m\n";
  for (int i = 1; i < n; i++) {
    s << pdf_num(x[i]) << " " << pdf_num(y[i]) << " l\n";
  }
  if (close) s << "h\n";
}

void PortablePdfDevice::line(double x1, double y1, double x2, double y2,
                             const pGEcontext gc) {
  std::ostringstream p;
  p << pdf_num(x1) << " " << pdf_num(y1) << " m " << pdf_num(x2) << " "
    << pdf_num(y2) << " l\n";
  if (appending) {
    doc.stream() << p.str();
    return;
  }
  if (R_ALPHA(gc->col) == 0 || blankLty(gc->lty)) return;
  std::ostringstream& s = doc.stream();
  s << "q\n";
  setStroke(gc);
  if (R_ALPHA(gc->col) != 255) {
    s << "/" << doc.addGState(R_ALPHA(gc->col) / 255.0, 1.0) << " gs\n";
  }
  s << p.str() << "S\nQ\n";
}

void PortablePdfDevice::polyline(int n, const double* x, const double* y,
                                 const pGEcontext gc) {
  if (n < 2) return;
  if (appending) {
    emitPath(doc.stream(), n, x, y, false);
    return;
  }
  if (R_ALPHA(gc->col) == 0 || blankLty(gc->lty)) return;
  std::ostringstream p;
  emitPath(p, n, x, y, false);
  std::ostringstream& s = doc.stream();
  s << "q\n";
  setStroke(gc);
  if (R_ALPHA(gc->col) != 255) {
    s << "/" << doc.addGState(R_ALPHA(gc->col) / 255.0, 1.0) << " gs\n";
  }
  s << p.str() << "S\nQ\n";
}

void PortablePdfDevice::polygon(int n, const double* x, const double* y,
                                const pGEcontext gc) {
  if (n < 2) return;
  std::ostringstream p;
  emitPath(p, n, x, y, true);
  if (appending) {
    doc.stream() << p.str();
    return;
  }
  paint(p.str(), gc, true);
}

void PortablePdfDevice::path(double* x, double* y, int npoly, const int* nper,
                             bool winding, const pGEcontext gc) {
  std::ostringstream p;
  int ind = 0;
  for (int i = 0; i < npoly; i++) {
    emitPath(p, nper[i], x + ind, y + ind, true);
    ind += nper[i];
  }
  if (appending) {
    doc.stream() << p.str();
    return;
  }
  paint(p.str(), gc, winding);
}

void PortablePdfDevice::rect(double x0, double y0, double x1, double y1,
                             const pGEcontext gc) {
  double left = std::min(x0, x1), w = std::fabs(x1 - x0);
  double bottom = std::min(y0, y1), h = std::fabs(y1 - y0);
  std::ostringstream p;
  p << pdf_num(left) << " " << pdf_num(bottom) << " " << pdf_num(w) << " "
    << pdf_num(h) << " re\n";
  if (appending) {
    doc.stream() << p.str();
    return;
  }
  paint(p.str(), gc, true);
}

void PortablePdfDevice::circle(double x, double y, double r,
                               const pGEcontext gc) {
  if (r <= 0) return;
  std::ostringstream p;
  const double k = 0.5522847498307936 * r;  // 4/3 (sqrt(2) - 1)
  p << pdf_num(x + r) << " " << pdf_num(y) << " m\n";
  p << pdf_num(x + r) << " " << pdf_num(y + k) << " " << pdf_num(x + k) << " "
    << pdf_num(y + r) << " " << pdf_num(x) << " " << pdf_num(y + r) << " c\n";
  p << pdf_num(x - k) << " " << pdf_num(y + r) << " " << pdf_num(x - r) << " "
    << pdf_num(y + k) << " " << pdf_num(x - r) << " " << pdf_num(y) << " c\n";
  p << pdf_num(x - r) << " " << pdf_num(y - k) << " " << pdf_num(x - k) << " "
    << pdf_num(y - r) << " " << pdf_num(x) << " " << pdf_num(y - r) << " c\n";
  p << pdf_num(x + k) << " " << pdf_num(y - r) << " " << pdf_num(x + r) << " "
    << pdf_num(y - k) << " " << pdf_num(x + r) << " " << pdf_num(y) << " c\n";
  if (appending) {
    doc.stream() << p.str();
    return;
  }
  paint(p.str(), gc, true);
}

// ---------------------------------------------------------------- raster

void PortablePdfDevice::raster(const unsigned int* raster, int w, int h,
                               double x, double y, double width, double height,
                               double rot, bool interpolate,
                               const pGEcontext gc) {
  if (appending || w <= 0 || h <= 0) return;
  std::vector<unsigned char> rgba(size_t(w) * h * 4);
  for (size_t i = 0; i < size_t(w) * h; i++) {
    rgba[i * 4 + 0] = R_RED(raster[i]);
    rgba[i * 4 + 1] = R_GREEN(raster[i]);
    rgba[i * 4 + 2] = R_BLUE(raster[i]);
    rgba[i * 4 + 3] = R_ALPHA(raster[i]);
  }
  PdfDoc::ImageColor img_color = PdfDoc::ImageColor::kAuto;
  if (colormodel == ColorModel::kGray) img_color = PdfDoc::ImageColor::kGray;
  if (colormodel == ColorModel::kCMYK) img_color = PdfDoc::ImageColor::kCMYK;
  std::string name = doc.addImage(w, h, rgba.data(), interpolate, img_color);

  double rad = rot * DEG_TO_RAD;
  double c = std::cos(rad), sn = std::sin(rad);
  // R rotates rasters about the anchor corner (x, y), not the centre
  std::ostringstream& s = doc.stream();
  s << "q\n1 0 0 1 " << pdf_num(x) << " " << pdf_num(y) << " cm\n"
    << pdf_num(c) << " " << pdf_num(sn) << " " << pdf_num(-sn) << " "
    << pdf_num(c) << " 0 0 cm\n"
    << pdf_num(width) << " 0 0 " << pdf_num(height) << " 0 0 cm\n/"
    << name << " Do\nQ\n";
}

// ------------------------------------------------------------------ text

FontSettings2 PortablePdfDevice::locateFont(const pGEcontext gc,
                                            double* size_out) {
  const char* family =
      gc->fontfamily[0] != '\0' ? gc->fontfamily : default_family.c_str();
  int face = gc->fontface;
  bool bold = face == 2 || face == 4;
  bool italic = face == 3 || face == 4;
  if (size_out) *size_out = gc->ps * gc->cex * scale;
  return systemfonts::ver2::locate_font(family, italic ? 1.0 : 0.0,
                                        bold ? 700.0 : 400.0, 0.0, nullptr,
                                        nullptr, 0);
}

bool PortablePdfDevice::colorGlyph(PdfFont* font, uint32_t orig_gid,
                                   double gx, double gy, double size,
                                   double rot, const pGEcontext gc) {
  SEXP raster_sexp = PROTECT(systemfonts::ver2::get_glyph_raster(
      (int)orig_gid, font->settings, size, 72.0, gc->col));
  if (raster_sexp == R_NilValue) {
    // Bitmap strikes can be sparse per glyph (e.g. Apple Color Emoji glyphs
    // added in later Unicode versions lack the mid-size strikes), and the
    // strike is chosen from the requested size. Retry against the largest
    // strike, which always carries the canonical artwork; the PDF placement
    // scales the image down to size
    UNPROTECT(1);
    raster_sexp = PROTECT(systemfonts::ver2::get_glyph_raster(
        (int)orig_gid, font->settings, 4096.0, 72.0, gc->col));
  }
  if (raster_sexp == R_NilValue || !Rf_isInteger(raster_sexp)) {
    UNPROTECT(1);
    return false;
  }

  int w = Rf_ncols(raster_sexp);
  int h = Rf_nrows(raster_sexp);
  int* pixels = INTEGER(raster_sexp);
  // nativeRaster is h x w column-major with the top row first; PDF image
  // data is row-major, also top row first
  std::vector<unsigned char> rgba(size_t(w) * h * 4);
  for (int yy = 0; yy < h; yy++) {
    for (int xx = 0; xx < w; xx++) {
      int col = pixels[yy + (size_t)xx * h];
      size_t i = (size_t)yy * w + xx;
      rgba[i * 4 + 0] = R_RED(col);
      rgba[i * 4 + 1] = R_GREEN(col);
      rgba[i * 4 + 2] = R_BLUE(col);
      rgba[i * 4 + 3] = R_ALPHA(col);
    }
  }

  // Placement: offset = c(top, left) and size = c(rows, width) in device
  // units, accounting for bitmap strike scaling
  double off_top = 0, off_left = 0, rows = h, cols = w;
  SEXP offset = Rf_getAttrib(raster_sexp, Rf_install("offset"));
  if (Rf_isReal(offset) && Rf_length(offset) == 2) {
    off_top = REAL(offset)[0];
    off_left = REAL(offset)[1];
  }
  SEXP rsize = Rf_getAttrib(raster_sexp, Rf_install("size"));
  if (Rf_isReal(rsize) && Rf_length(rsize) == 2) {
    rows = REAL(rsize)[0];
    cols = REAL(rsize)[1];
  }
  UNPROTECT(1);
  if (rows <= 0 || cols <= 0) return false;

  bool interpolate = cols > w;  // upscaling a bitmap strike
  std::string name = doc.addImage(w, h, rgba.data(), interpolate);

  double rad = rot * DEG_TO_RAD;
  double c = std::cos(rad), sn = std::sin(rad);
  // Rotate around the glyph anchor (gx, gy); the image hangs below the
  // top offset, y-up
  std::ostringstream& s = doc.stream();
  s << "q\n" << pdf_num(c) << " " << pdf_num(sn) << " " << pdf_num(-sn) << " "
    << pdf_num(c) << " " << pdf_num(gx) << " " << pdf_num(gy) << " cm\n"
    << pdf_num(cols) << " 0 0 " << pdf_num(rows) << " " << pdf_num(off_left)
    << " " << pdf_num(off_top - rows) << " cm\n/" << name << " Do\nQ\n";
  return true;
}

void PortablePdfDevice::text(double x, double y, const char* str, double rot,
                             double hadj, const pGEcontext gc) {
  if (appending || R_ALPHA(gc->col) == 0) return;
  double size;
  FontSettings2 font = locateFont(gc, &size);
  if (font.file[0] == '\0') return;

  std::vector<textshaping::Point> loc;
  std::vector<uint32_t> id;
  std::vector<int> cluster;
  std::vector<unsigned int> font_index;
  std::vector<FontSettings2> fallbacks;
  std::vector<double> fallback_scaling;
  int err = textshaping::ver2::string_shape(str, font, size, 72.0, loc, id,
                                            cluster, font_index, fallbacks,
                                            fallback_scaling);
  if (err != 0 || id.empty()) return;

  size_t n_glyphs = id.size();
  size_t str_len = std::strlen(str);
  // Shaping clusters are codepoint indices; map them to byte offsets in str
  std::vector<size_t> char_offsets;
  char_offsets.reserve(str_len + 1);
  for (size_t b = 0; b < str_len; b++) {
    if ((str[b] & 0xC0) != 0x80) char_offsets.push_back(b);
  }
  char_offsets.push_back(str_len);
  size_t n_chars = char_offsets.size() - 1;
  double rad = rot * DEG_TO_RAD;
  double c = std::cos(rad), sn = std::sin(rad);

  // Anchor adjustment needs the string advance
  double width = 0;
  if (hadj != 0) {
    // Same width semantics as strWidth() (bearings included) so the anchor
    // adjustment matches the engine's own justification
    if (textshaping::ver2::string_width(str, font, size, 72.0, 1, &width) !=
        0) {
      width = 0;
    }
  }
  double base_x = x - hadj * width * c;
  double base_y = y - hadj * width * sn;

  std::ostringstream& s = doc.stream();
  s << "q\n";
  setFillColor(s, gc->col);
  if (R_ALPHA(gc->col) != 255) {
    s << "/" << doc.addGState(1.0, R_ALPHA(gc->col) / 255.0) << " gs\n";
  }
  // Tag the whole string with its exact source text. ToUnicode covers most
  // extractors, but /ActualText also handles what per-glyph maps cannot:
  // glyphs reused across different source texts, multi-glyph clusters, and
  // color glyphs drawn as raster images (which are not font glyphs at all)
  s << "/Span <</ActualText <" << utf8_to_utf16be_hex(str) << ">>> BDC\nBT\n";

  std::string current_font;
  for (size_t i = 0; i < n_glyphs; i++) {
    unsigned int fi = i < font_index.size() ? font_index[i] : 0;
    if (fi >= fallbacks.size()) fi = 0;
    const FontSettings2& fs = fallbacks.empty() ? font : fallbacks[fi];
    if (fs.file[0] == '\0') continue;
    PdfFont* pdf_font = doc.getFont(fs);
    if (pdf_font->parse_failed) continue;

    // Recover the glyph's source text from its shaping cluster for the
    // ToUnicode map. The span runs from the glyph's cluster to the next
    // higher cluster value; only the first glyph of a cluster gets the text,
    // so one-to-many glyph mappings are not duplicated on copy. This is what
    // keeps ligatures and contextual forms (which no cmap reaches)
    // searchable and copy-pasteable.
    std::string glyph_text;
    if (i < cluster.size() && cluster[i] >= 0 &&
        (size_t)cluster[i] < n_chars) {
      bool first_in_cluster = true;
      for (size_t j = 0; j < i && first_in_cluster; j++) {
        if (j < cluster.size() && cluster[j] == cluster[i]) {
          first_in_cluster = false;
        }
      }
      if (first_in_cluster) {
        size_t end_char = n_chars;
        for (size_t j = 0; j < n_glyphs; j++) {
          if (j < cluster.size() && (size_t)cluster[j] > (size_t)cluster[i] &&
              (size_t)cluster[j] < end_char) {
            end_char = (size_t)cluster[j];
          }
        }
        size_t start_byte = char_offsets[(size_t)cluster[i]];
        glyph_text.assign(str + start_byte, char_offsets[end_char] - start_byte);
      }
    }
    uint32_t gid = pdf_font->useGlyph(id[i], glyph_text);

    // fallback_scaling is per font; -1 means no scaling
    double fscale = fi < fallback_scaling.size() ? fallback_scaling[fi] : -1.0;
    double gsize = size * (fscale > 0 ? fscale : 1.0);

    double lx = i < loc.size() ? loc[i].x : 0;
    double ly = i < loc.size() ? loc[i].y : 0;
    double gx = base_x + lx * c - ly * sn;
    double gy = base_y + lx * sn + ly * c;

    // Bitmap/color glyphs (emoji) are drawn as embedded images
    bool no_outline = false;
    pdf_font->glyphOutline(id[i], &no_outline);
    if (no_outline &&
        colorGlyph(pdf_font, id[i], gx, gy, gsize, rot, gc)) {
      continue;
    }

    if (pdf_font->resource_name != current_font) {
      s << "/" << pdf_font->resource_name << " " << pdf_num(gsize) << " Tf\n";
      current_font = pdf_font->resource_name;
    }
    s << pdf_num(c) << " " << pdf_num(sn) << " " << pdf_num(-sn) << " "
      << pdf_num(c) << " " << pdf_num(gx) << " " << pdf_num(gy) << " Tm <"
      << pdf_hex16(gid) << "> Tj\n";
  }
  s << "ET\nEMC\nQ\n";
}

double PortablePdfDevice::strWidth(const char* str, const pGEcontext gc) {
  double size;
  FontSettings2 font = locateFont(gc, &size);
  if (font.file[0] == '\0') return 0;
  double width = 0;
  if (textshaping::ver2::string_width(str, font, size, 72.0, 1, &width) !=
      0) {
    return 0;
  }
  return width;
}

void PortablePdfDevice::metricInfo(int c, const pGEcontext gc, double* ascent,
                                   double* descent, double* width) {
  *ascent = *descent = *width = 0;
  double size;
  FontSettings2 font = locateFont(gc, &size);
  if (font.file[0] == '\0') return;
  systemfonts::ver2::glyph_metrics((uint32_t)c, font, size, 72.0, ascent,
                                   descent, width);
}

// ------------------------------------------------- paths as R functions

// Replay a path definition (an R function that draws the path) and return
// the recorded geometry without any painting or graphics state.
static std::string replayPath(PdfDoc& doc, SEXP fn, int* appending) {
  (*appending)++;
  doc.beginCapture();
  SEXP fcall = PROTECT(Rf_lang1(fn));
  Rf_eval(fcall, R_GlobalEnv);
  UNPROTECT(1);
  std::string geom = doc.endCapture();
  (*appending)--;
  return geom;
}

void PortablePdfDevice::strokePath(SEXP path, const pGEcontext gc) {
  if (R_ALPHA(gc->col) == 0 || blankLty(gc->lty)) return;
  std::string geom = replayPath(doc, path, &appending);
  // Stroke only: synthesize a gc with no fill
  R_GE_gcontext tmp = *gc;
  tmp.fill = R_RGBA(255, 255, 255, 0);
  tmp.patternFill = R_NilValue;
  paint(geom, &tmp, true);
}

void PortablePdfDevice::fillPath(SEXP path, int rule, const pGEcontext gc) {
  bool pattern = gc->patternFill != R_NilValue;
  if (!pattern && R_ALPHA(gc->fill) == 0) return;
  std::string geom = replayPath(doc, path, &appending);
  R_GE_gcontext tmp = *gc;
  tmp.col = R_RGBA(255, 255, 255, 0);
  paint(geom, &tmp, rule == R_GE_nonZeroWindingRule);
}

void PortablePdfDevice::fillStrokePath(SEXP path, int rule,
                                       const pGEcontext gc) {
  std::string geom = replayPath(doc, path, &appending);
  paint(geom, gc, rule == R_GE_nonZeroWindingRule);
}

// ------------------------------------------------- graphical definitions

std::string PortablePdfDevice::captureDrawing(SEXP fn) {
  doc.beginCapture();
  std::ostringstream& s = doc.stream();
  // Guard so that clip resets ("Q q") inside the capture stay balanced
  s << "q\n";
  bool saved_clipped = clipped, saved_path_clip = path_clip_active;
  double sx0 = clip_x0, sx1 = clip_x1, sy0 = clip_y0, sy1 = clip_y1;
  int saved_mask = current_mask;
  clipped = false;
  path_clip_active = false;
  current_mask = -1;

  SEXP fcall = PROTECT(Rf_lang1(fn));
  Rf_eval(fcall, R_GlobalEnv);
  UNPROTECT(1);

  s << "Q\n";
  std::string out = doc.endCapture();
  clipped = saved_clipped;
  path_clip_active = saved_path_clip;
  clip_x0 = sx0;
  clip_x1 = sx1;
  clip_y0 = sy0;
  clip_y1 = sy1;
  current_mask = saved_mask;
  return out;
}

void PortablePdfDevice::applyMask() {
  if (current_mask >= 0 && current_mask < (int)masks.size() &&
      !masks[current_mask].empty()) {
    doc.stream() << "/" << masks[current_mask] << " gs\n";
  }
}

SEXP PortablePdfDevice::setPattern(SEXP pattern) {
#if R_GE_version >= 13
  PatternEntry e;
  switch (R_GE_patternType(pattern)) {
    case R_GE_linearGradientPattern: {
      int n = R_GE_linearGradientNumStops(pattern);
      std::vector<double> stops(n);
      std::vector<unsigned int> cols(n);
      for (int i = 0; i < n; i++) {
        stops[i] = R_GE_linearGradientStop(pattern, i);
        cols[i] = R_GE_linearGradientColour(pattern, i);
      }
      int extend = R_GE_linearGradientExtend(pattern);
      if (extend == R_GE_patternExtendRepeat ||
          extend == R_GE_patternExtendReflect) {
        Rf_warning(
            "repeat/reflect gradient extend is not supported; using pad");
        extend = R_GE_patternExtendPad;
      }
      e.name = doc.addAxialPattern(R_GE_linearGradientX1(pattern),
                                   R_GE_linearGradientY1(pattern),
                                   R_GE_linearGradientX2(pattern),
                                   R_GE_linearGradientY2(pattern), stops, cols,
                                   extend, &e.alpha_gs, &e.alpha_sh);
      break;
    }
    case R_GE_radialGradientPattern: {
      int n = R_GE_radialGradientNumStops(pattern);
      std::vector<double> stops(n);
      std::vector<unsigned int> cols(n);
      for (int i = 0; i < n; i++) {
        stops[i] = R_GE_radialGradientStop(pattern, i);
        cols[i] = R_GE_radialGradientColour(pattern, i);
      }
      int extend = R_GE_radialGradientExtend(pattern);
      if (extend == R_GE_patternExtendRepeat ||
          extend == R_GE_patternExtendReflect) {
        Rf_warning(
            "repeat/reflect gradient extend is not supported; using pad");
        extend = R_GE_patternExtendPad;
      }
      e.name = doc.addRadialPattern(
          R_GE_radialGradientCX1(pattern), R_GE_radialGradientCY1(pattern),
          R_GE_radialGradientR1(pattern), R_GE_radialGradientCX2(pattern),
          R_GE_radialGradientCY2(pattern), R_GE_radialGradientR2(pattern),
          stops, cols, extend, &e.alpha_gs, &e.alpha_sh);
      break;
    }
    case R_GE_tilingPattern: {
      int extend = R_GE_tilingPatternExtend(pattern);
      if (extend == R_GE_patternExtendReflect) {
        Rf_warning("reflect tiling pattern extend is not supported");
        extend = R_GE_patternExtendRepeat;
      }
      std::string tile = captureDrawing(R_GE_tilingPatternFunction(pattern));
      e.name = doc.addTilingPattern(R_GE_tilingPatternX(pattern),
                                    R_GE_tilingPatternY(pattern),
                                    R_GE_tilingPatternWidth(pattern),
                                    R_GE_tilingPatternHeight(pattern), tile,
                                    extend);
      break;
    }
    default:
      Rf_warning("unknown pattern type");
      return R_NilValue;
  }

  int index = -1;
  for (size_t i = 0; i < patterns.size(); i++) {
    if (patterns[i].name.empty()) {
      index = (int)i;
      break;
    }
  }
  if (index < 0) {
    index = (int)patterns.size();
    patterns.emplace_back();
  }
  patterns[index] = e;
  return Rf_ScalarInteger(index);
#else
  return R_NilValue;
#endif
}

void PortablePdfDevice::releasePattern(SEXP ref) {
  // grid signals "release everything" with R_NilValue (L_clearDefinitions)
  if (ref == R_NilValue) {
    patterns.clear();
    return;
  }
  int idx = INTEGER(ref)[0];
  if (idx >= 0 && idx < (int)patterns.size()) {
    patterns[idx] = PatternEntry();
  }
}

SEXP PortablePdfDevice::setClipPath(SEXP path, SEXP ref) {
#if R_GE_version >= 13
  int index = -1;
  if (ref != R_NilValue) index = INTEGER(ref)[0];
  if (index < 0 || index >= (int)clip_paths.size() ||
      clip_paths[index].stream.empty()) {
    std::string stream = replayPath(doc, path, &appending);
    ClipPathEntry e{stream, R_GE_clipPathFillRule(path)};
    for (size_t i = 0; i < clip_paths.size(); i++) {
      if (clip_paths[i].stream.empty()) {
        index = (int)i;
        break;
      }
    }
    if (index < 0 || index >= (int)clip_paths.size()) {
      index = (int)clip_paths.size();
      clip_paths.push_back(e);
    } else {
      clip_paths[index] = e;
    }
  }

  // Apply: reset to the enclosing state, re-apply the rectangular clip,
  // then intersect with the clip path
  std::ostringstream& s = doc.stream();
  if (clipped || path_clip_active) s << "Q q\n";
  if (clipped) {
    s << pdf_num(clip_x0) << " " << pdf_num(clip_y0) << " "
      << pdf_num(clip_x1 - clip_x0) << " " << pdf_num(clip_y1 - clip_y0)
      << " re W n\n";
  }
  s << clip_paths[index].stream
    << (clip_paths[index].rule == R_GE_evenOddRule ? "W* n\n" : "W n\n");
  path_clip_active = true;
  applyMask();
  return Rf_ScalarInteger(index);
#else
  return R_NilValue;
#endif
}

void PortablePdfDevice::releaseClipPath(SEXP ref) {
  if (ref == R_NilValue) {
    clip_paths.clear();
  } else {
    int idx = INTEGER(ref)[0];
    if (idx >= 0 && idx < (int)clip_paths.size()) {
      clip_paths[idx] = ClipPathEntry{"", R_GE_nonZeroWindingRule};
    }
  }
  std::ostringstream& s = doc.stream();
  if (clipped || path_clip_active) s << "Q q\n";
  if (clipped) {
    s << pdf_num(clip_x0) << " " << pdf_num(clip_y0) << " "
      << pdf_num(clip_x1 - clip_x0) << " " << pdf_num(clip_y1 - clip_y0)
      << " re W n\n";
  }
  path_clip_active = false;
  applyMask();
}

SEXP PortablePdfDevice::setMask(SEXP mask, SEXP ref) {
#if R_GE_version >= 13
  std::ostringstream& s = doc.stream();
  if (mask == R_NilValue) {
    current_mask = -1;
    s << "/" << doc.addGState(1.0, 1.0, "", "", false, true) << " gs\n";
    return R_NilValue;
  }
  bool luminance = R_GE_maskType(mask) == R_GE_luminanceMask;
  int index = -1;
  if (ref != R_NilValue) index = INTEGER(ref)[0];
  if (index < 0 || index >= (int)masks.size() || masks[index].empty()) {
    std::string stream = captureDrawing(mask);
    // Luminosity masks derive alpha from the form's colour, so the form
    // keeps its RGB transparency group (addForm's default)
    std::string form = doc.addForm(stream, false);
    std::string gs = doc.addGState(1.0, 1.0, "", form, luminance);
    for (size_t i = 0; i < masks.size(); i++) {
      if (masks[i].empty()) {
        index = (int)i;
        break;
      }
    }
    if (index < 0 || index >= (int)masks.size()) {
      index = (int)masks.size();
      masks.push_back(gs);
    } else {
      masks[index] = gs;
    }
  }
  current_mask = index;
  applyMask();
  return Rf_ScalarInteger(index);
#else
  return R_NilValue;
#endif
}

void PortablePdfDevice::releaseMask(SEXP ref) {
  if (ref == R_NilValue) {
    masks.clear();
    current_mask = -1;
    return;
  }
  int idx = INTEGER(ref)[0];
  if (idx >= 0 && idx < (int)masks.size()) {
    masks[idx].clear();
  }
  if (current_mask == idx) current_mask = -1;
}

static const char* blendModeName(int op) {
  switch (op) {
    case R_GE_compositeOver:
      return "Normal";
    case R_GE_compositeMultiply:
      return "Multiply";
    case R_GE_compositeScreen:
      return "Screen";
    case R_GE_compositeOverlay:
      return "Overlay";
    case R_GE_compositeDarken:
      return "Darken";
    case R_GE_compositeLighten:
      return "Lighten";
    case R_GE_compositeColorDodge:
      return "ColorDodge";
    case R_GE_compositeColorBurn:
      return "ColorBurn";
    case R_GE_compositeHardLight:
      return "HardLight";
    case R_GE_compositeSoftLight:
      return "SoftLight";
    case R_GE_compositeDifference:
      return "Difference";
    case R_GE_compositeExclusion:
      return "Exclusion";
    default:
      return nullptr;
  }
}

SEXP PortablePdfDevice::defineGroup(SEXP src, int op, SEXP dst) {
#if R_GE_version >= 15
  std::string form;
  const char* bm = blendModeName(op);

  if (bm != nullptr) {
    // Plain "over" or a PDF blend mode: capture dst and src into one stream
    doc.beginCapture();
    std::ostringstream& s = doc.stream();
    s << "q\n";
    bool saved_clipped = clipped, saved_path_clip = path_clip_active;
    double sx0 = clip_x0, sx1 = clip_x1, sy0 = clip_y0, sy1 = clip_y1;
    int saved_mask = current_mask;
    clipped = false;
    path_clip_active = false;
    current_mask = -1;

    if (dst != R_NilValue) {
      SEXP fcall = PROTECT(Rf_lang1(dst));
      Rf_eval(fcall, R_GlobalEnv);
      UNPROTECT(1);
    }
    if (std::string(bm) != "Normal") {
      s << "/" << doc.addGState(1.0, 1.0, bm) << " gs\n";
    }
    {
      SEXP fcall = PROTECT(Rf_lang1(src));
      Rf_eval(fcall, R_GlobalEnv);
      UNPROTECT(1);
    }

    s << "Q\n";
    std::string captured = doc.endCapture();
    clipped = saved_clipped;
    path_clip_active = saved_path_clip;
    clip_x0 = sx0;
    clip_x1 = sx1;
    clip_y0 = sy0;
    clip_y1 = sy1;
    current_mask = saved_mask;

    form = doc.addForm(captured, false);
  } else if (op == R_GE_compositeAdd || op == R_GE_compositeSaturate) {
    Rf_warning(
        "compositing operator is not expressible in PDF; "
        "approximating with 'over'");
    return defineGroup(src, R_GE_compositeOver, dst);
  } else {
    // Porter-Duff operators, synthesized from alpha soft masks (optionally
    // inverted via a /TR transfer function), paint ordering, and selective
    // painting of the destination. src and dst are captured as isolated
    // forms so their standalone alpha can serve as a mask.
    std::string formD, formS;
    if (dst != R_NilValue) formD = doc.addForm(captureDrawing(dst), false, true);
    formS = doc.addForm(captureDrawing(src), false, true);

    std::ostringstream r;
    auto paint_masked = [&](const std::string& what, const std::string& mask,
                            bool invert) {
      std::string gs = doc.addGState(1.0, 1.0, "", mask, false, false, invert);
      r << "q\n/" << gs << " gs\n/" << what << " Do\nQ\n";
    };
    if (formD.empty()) {
      // No destination: ops collapse to "src", nothing, or "dst" (nothing)
      switch (op) {
        case R_GE_compositeIn:
        case R_GE_compositeAtop:
        case R_GE_compositeDest:
        case R_GE_compositeDestIn:
        case R_GE_compositeDestOut:
        case R_GE_compositeDestAtop:
        case R_GE_compositeClear:
          break;  // empty result
        default:
          r << "/" << formS << " Do\n";
      }
    } else {
      switch (op) {
        case R_GE_compositeClear:
          break;
        case R_GE_compositeSource:
          r << "/" << formS << " Do\n";
          break;
        case R_GE_compositeDest:
          r << "/" << formD << " Do\n";
          break;
        case R_GE_compositeIn:
          paint_masked(formS, formD, false);
          break;
        case R_GE_compositeDestIn:
          paint_masked(formD, formS, false);
          break;
        case R_GE_compositeOut:
          paint_masked(formS, formD, true);
          break;
        case R_GE_compositeDestOut:
          paint_masked(formD, formS, true);
          break;
        case R_GE_compositeAtop:
          r << "/" << formD << " Do\n";
          paint_masked(formS, formD, false);
          break;
        case R_GE_compositeDestAtop:
          r << "/" << formS << " Do\n";
          paint_masked(formD, formS, false);
          break;
        case R_GE_compositeXor:
          paint_masked(formD, formS, true);
          paint_masked(formS, formD, true);
          break;
        case R_GE_compositeDestOver:
          r << "/" << formS << " Do\n";
          r << "/" << formD << " Do\n";
          break;
        default:  // over reached via blendModeName; be safe
          r << "/" << formD << " Do\n";
          r << "/" << formS << " Do\n";
      }
    }
    form = doc.addForm(r.str(), false);
  }

  int index = -1;
  for (size_t i = 0; i < groups.size(); i++) {
    if (groups[i].empty()) {
      index = (int)i;
      break;
    }
  }
  if (index < 0) {
    index = (int)groups.size();
    groups.emplace_back();
  }
  groups[index] = form;
  return Rf_ScalarInteger(index);
#else
  return R_NilValue;
#endif
}

void PortablePdfDevice::useGroup(SEXP ref, SEXP trans) {
  if (ref == R_NilValue) return;
  int idx = INTEGER(ref)[0];
  if (idx < 0 || idx >= (int)groups.size() || groups[idx].empty()) return;
  std::ostringstream& s = doc.stream();
  s << "q\n";
  if (trans != R_NilValue) {
    double* m = REAL(trans);
    // R's 3x2 affine matrix [xx xy x0 yx yy y0] as PDF cm operands
    s << pdf_num(m[0]) << " " << pdf_num(m[3]) << " " << pdf_num(m[1]) << " "
      << pdf_num(m[4]) << " " << pdf_num(m[2]) << " " << pdf_num(m[5])
      << " cm\n";
  }
  s << "/" << groups[idx] << " Do\nQ\n";
}

void PortablePdfDevice::releaseGroup(SEXP ref) {
  if (ref == R_NilValue) {
    groups.clear();
    return;
  }
  int idx = INTEGER(ref)[0];
  if (idx >= 0 && idx < (int)groups.size()) {
    groups[idx].clear();
  }
}

// ----------------------------------------------------------------- glyph

void PortablePdfDevice::glyph(int n, int* glyphs, double* x, double* y,
                              SEXP font, double size, int colour,
                              double rot) {
#if R_GE_version >= 16
  if (appending || n <= 0 || R_ALPHA(colour) == 0) return;
  const char* file = R_GE_glyphFontFile(font);
  int index = R_GE_glyphFontIndex(font);
  if (file == nullptr || file[0] == '\0') return;

  FontSettings2 fset;
  std::strncpy(fset.file, file, PATH_MAX);
  fset.file[PATH_MAX] = '\0';
  fset.index = (unsigned int)index;
#if R_GE_version >= 17
  // Variable font instance requested on the glyph font; tags are packed
  // big-endian and coordinates are 16.16 fixed point
  std::vector<int> axes, coords;
  int n_var = R_GE_glyphFontNumVar(font);
  for (int i = 0; i < n_var; i++) {
    const char* axis = R_GE_glyphFontVarAxis(font, i);
    if (axis == nullptr) continue;
    uint32_t tag = 0;
    for (int j = 0; j < 4; j++) {
      char ch = axis[j] != '\0' ? axis[j] : ' ';
      tag = (tag << 8) | (uint32_t)(unsigned char)ch;
    }
    axes.push_back((int)tag);
    coords.push_back(
        (int)std::lround(R_GE_glyphFontVarValue(font, i) * 65536.0));
  }
  if (!axes.empty()) {
    fset.axes = axes.data();
    fset.coords = coords.data();
    fset.n_axes = (int)axes.size();
  }
#endif
  PdfFont* pdf_font = doc.getFont(fset);
  if (pdf_font->parse_failed) return;

  double gsize = size * scale;
  double rad = rot * DEG_TO_RAD;
  double c = std::cos(rad), sn = std::sin(rad);

  std::ostringstream& s = doc.stream();
  s << "q\n";
  setFillColor(s, (unsigned int)colour);
  if (R_ALPHA(colour) != 255) {
    s << "/" << doc.addGState(1.0, R_ALPHA(colour) / 255.0) << " gs\n";
  }
  s << "BT\n/" << pdf_font->resource_name << " " << pdf_num(gsize) << " Tf\n";
  for (int i = 0; i < n; i++) {
    uint32_t gid = pdf_font->useGlyph((uint32_t)glyphs[i]);
    s << pdf_num(c) << " " << pdf_num(sn) << " " << pdf_num(-sn) << " "
      << pdf_num(c) << " " << pdf_num(x[i]) << " " << pdf_num(y[i]) << " Tm <"
      << pdf_hex16(gid) << "> Tj\n";
  }
  s << "ET\nQ\n";
#endif
}
