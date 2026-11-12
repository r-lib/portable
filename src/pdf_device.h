// Graphics engine device translating GE callbacks into PDF content streams,
// using systemfonts/textshaping for font lookup and text layout.
#pragma once

#define R_NO_REMAP

#include <R.h>
#include <Rinternals.h>
#include <R_ext/GraphicsEngine.h>

#include <systemfonts.h>

#include <string>
#include <vector>

#include "pdf_doc.h"

enum class ColorModel { kRGB, kGray, kCMYK };

class PortablePdfDevice {
 public:
  PortablePdfDevice(const std::string& file, double width, double height,
                    double scaling, const std::string& family,
                    const std::string& title, const std::string& author,
                    const std::string& creator, const std::string& subject,
                    const std::string& keywords,
                    const std::string& creation_date, ColorModel colormodel);
  ~PortablePdfDevice() = default;

  void newPage(unsigned int fill);
  void close();
  void clip(double x0, double x1, double y0, double y1);

  void line(double x1, double y1, double x2, double y2, const pGEcontext gc);
  void polyline(int n, const double* x, const double* y, const pGEcontext gc);
  void polygon(int n, const double* x, const double* y, const pGEcontext gc);
  void path(double* x, double* y, int npoly, const int* nper, bool winding,
            const pGEcontext gc);
  void rect(double x0, double y0, double x1, double y1, const pGEcontext gc);
  void circle(double x, double y, double r, const pGEcontext gc);
  void raster(const unsigned int* raster, int w, int h, double x, double y,
              double width, double height, double rot, bool interpolate,
              const pGEcontext gc);
  void text(double x, double y, const char* str, double rot, double hadj,
            const pGEcontext gc);
  double strWidth(const char* str, const pGEcontext gc);
  void metricInfo(int c, const pGEcontext gc, double* ascent, double* descent,
                  double* width);

  // Paths delivered as an R function that replays the path (grid.path)
  void strokePath(SEXP path, const pGEcontext gc);
  void fillPath(SEXP path, int rule, const pGEcontext gc);
  void fillStrokePath(SEXP path, int rule, const pGEcontext gc);

  // Graphical definitions (R_GE_definitions / R_GE_group)
  SEXP setPattern(SEXP pattern);
  void releasePattern(SEXP ref);
  SEXP setClipPath(SEXP path, SEXP ref);
  void releaseClipPath(SEXP ref);
  SEXP setMask(SEXP mask, SEXP ref);
  void releaseMask(SEXP ref);
  SEXP defineGroup(SEXP src, int op, SEXP dst);
  void useGroup(SEXP ref, SEXP trans);
  void releaseGroup(SEXP ref);

  // grid.glyph() support
  void glyph(int n, int* glyphs, double* x, double* y, SEXP font, double size,
             int colour, double rot);

  double pageWidth() const { return page_width; }
  double pageHeight() const { return page_height; }

 private:
  PdfDoc doc;
  double page_width, page_height;  // device units (points * scaling)
  double scale;
  std::string default_family;
  ColorModel colormodel;
  bool has_page = false;
  bool ever_had_page = false;

  // Clip state: the current clip is the intersection of the rectangular clip
  // and (optionally) a clip path, both applied inside one saved gstate.
  bool clipped = false;
  double clip_x0 = 0, clip_x1 = 0, clip_y0 = 0, clip_y1 = 0;
  bool path_clip_active = false;

  // > 0 while a path/clipPath function is being replayed: primitives emit
  // geometry only, no painting or color state
  int appending = 0;

  // Active pattern fill (index into patterns, -1 = none) is read from
  // gc->patternFill at paint time
  struct PatternEntry {
    std::string name;      // empty = released slot
    std::string alpha_gs;  // ExtGState for gradients with alpha, or empty
  };
  std::vector<PatternEntry> patterns;

  struct ClipPathEntry {
    std::string stream;  // path geometry in device units
    int rule;            // R_GE_nonZeroWindingRule / R_GE_evenOddRule
  };
  std::vector<ClipPathEntry> clip_paths;  // empty stream = released slot

  std::vector<std::string> masks;   // ExtGState names; empty = released slot
  int current_mask = -1;
  std::vector<std::string> groups;  // form XObject names; empty = released

  FontSettings2 locateFont(const pGEcontext gc, double* size_out);
  // Color emission honoring the device color model
  void setStrokeColor(std::ostringstream& s, unsigned int col);
  void setFillColor(std::ostringstream& s, unsigned int col);
  void setStroke(const pGEcontext gc);
  void setFill(const pGEcontext gc);
  // Paint path_ops with the gc state, splitting into separate fill and
  // stroke passes when stroke and fill alphas differ. winding selects the
  // nonzero (vs even-odd) rule for fills.
  void paint(const std::string& path_ops, const pGEcontext gc, bool winding);
  void emitPath(std::ostringstream& s, int n, const double* x,
                const double* y, bool close);
  // Replay an R drawing function while capturing its output
  std::string captureDrawing(SEXP fn);
  // Apply the current mask ExtGState (call after any gstate reset)
  void applyMask();
  // Draw a bitmap (color) glyph as an embedded image; returns false when
  // no raster is available so the caller can fall back to the outline
  bool colorGlyph(PdfFont* font, uint32_t orig_gid, double gx, double gy,
                  double size, double rot, const pGEcontext gc);
};
