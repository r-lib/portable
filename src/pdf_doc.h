// Minimal PDF writer: pages, vector content streams, embedded CID fonts
// (TrueType and CFF), raster image XObjects, and the cross-reference table.
#pragma once

#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <systemfonts.h>

#include "sfnt.h"

// One glyph in the synthesized subset font
struct SubGlyph {
  uint32_t orig_gid;   // glyph id in the source font
  std::string text;    // unicode text for ToUnicode (from the source cmap)
  int advance;         // advance width in font units
};

class PdfFont {
 public:
  // The font is identified by file, index, and variation coordinates;
  // axes/coords are deep-copied so `settings` is self-contained
  PdfFont(const FontSettings2& font, int resource_id);

  // Record a used glyph from the source font, returning the glyph id it
  // has in the synthesized subset (0 is always .notdef)
  uint32_t useGlyph(uint32_t orig_gid);

  // Outline path for a source glyph in font units, or empty when the
  // glyph has no outline (bitmap/color font). Result is cached.
  // Sets *no_outline for glyphs that can never have vector outlines.
  const std::string& glyphOutline(uint32_t orig_gid, bool* no_outline);

  // Write font objects, returns the font resource object number
  int writeObjects(class PdfDoc& doc);

  std::string path;
  unsigned int index;
  FontSettings2 settings;  // for outline/raster extraction at the instance
  std::string resource_name;  // e.g. "F3"
  sfnt::Metrics metrics;
  bool parse_failed = false;
  int subset_id;

  std::map<uint32_t, uint32_t> gid_map;  // source gid -> subset gid
  std::vector<SubGlyph> sub;             // subset glyphs, [0] is .notdef

 private:
  std::vector<int> axes_store, coords_store;  // backing store for settings
  // source gid -> (outline path, is bitmap-only)
  std::map<uint32_t, std::pair<std::string, bool>> outline_cache;
  std::pair<std::string, bool> fetchOutline(uint32_t orig_gid);
};

class PdfDoc {
 public:
  // When one_file_per_page is true, filename is a printf-style pattern with
  // a single integer conversion (e.g. "plot%03d.pdf") and each page is
  // written to its own expanded file as soon as the page ends
  PdfDoc(const std::string& filename, double scaling,
         bool one_file_per_page = false, bool compress = true);

  void beginPage(double width_in, double height_in);
  void endPage();
  // Assemble fonts, pages, catalog and xref; write the file
  bool finish();

  // Device-unit content stream for the current page
  std::ostringstream& stream() { return content; }

  // Capture drawing into a side stream (for groups, masks, tiling patterns).
  // endCapture returns the recorded stream and restores the previous one.
  void beginCapture();
  std::string endCapture();

  // Fonts are keyed by file, face index, and variation coordinates, and
  // shared across pages
  PdfFont* getFont(const FontSettings2& font);

  // Embed an RGBA raster, returns the XObject resource name. kAuto keeps RGB
  // but falls back to gray when all pixels are gray; kGray and kCMYK force
  // conversion to that colour space
  enum class ImageColor { kAuto, kGray, kCMYK };
  std::string addImage(int width, int height, const unsigned char* rgba,
                       bool interpolate = false,
                       ImageColor color = ImageColor::kAuto);

  // Form XObject over the full page (transparency group), for groups and
  // soft masks. isolated requests /I true, so the group's alpha is computed
  // without the backdrop (required for forms used as soft mask sources).
  // Returns the resource name.
  std::string addForm(const std::string& stream, bool gray_group = false,
                      bool isolated = false);

  // Shading patterns. Stops and colors must have the same length. Returns
  // the pattern resource name; when any stop has alpha < 255, an alpha
  // gradient soft mask is also built and *alpha_gs is set to its ExtGState
  // resource name. extend: 1=pad, 2=repeat, 3=reflect, 4=none.
  std::string addAxialPattern(double x1, double y1, double x2, double y2,
                              const std::vector<double>& stops,
                              const std::vector<unsigned int>& cols,
                              int extend, std::string* alpha_gs);
  std::string addRadialPattern(double cx1, double cy1, double r1, double cx2,
                               double cy2, double r2,
                               const std::vector<double>& stops,
                               const std::vector<unsigned int>& cols,
                               int extend, std::string* alpha_gs);
  // Tiling pattern from a captured drawing stream (device units)
  std::string addTilingPattern(double x, double y, double width, double height,
                               const std::string& tile_stream, int extend);

  // Graphics state with constant stroke/fill alpha plus optional blend mode
  // (PDF name, e.g. "Multiply") and soft mask (form resource name).
  std::string addGState(double stroke_alpha, double fill_alpha,
                        const std::string& blend = "",
                        const std::string& smask_form = "",
                        bool smask_luminosity = false,
                        bool clear_smask = false,
                        bool invert_smask = false);

  // Document metadata for the Info dictionary
  // creation_date is a preformatted PDF date string; empty = use current time
  void setInfo(const std::string& title, const std::string& author,
               const std::string& creator, const std::string& subject = "",
               const std::string& keywords = "",
               const std::string& creation_date = "");

  double scaling() const { return scale; }
  double pageWidthPts() const { return page_width_pts; }
  double pageHeightPts() const { return page_height_pts; }
  int objectCount() const { return n_objects; }
  int newObject();

 private:
  std::string filename;
  double scale;
  bool per_page_files;
  // Compress text streams (content, forms, tiling patterns); font and image
  // data is always compressed
  bool compress_streams;
  int page_number = 0;  // pages ever begun, for per-page file naming

  struct Page {
    double width, height;  // physical points
    std::string content;   // device-unit stream
  };

  // Assemble fonts, pages, catalog and xref for the given pages and write
  // them to name; used by both finish() and per-page flushing
  bool writeFile(const std::string& name, const std::vector<Page>& pages);
  // Clear all per-document state (objects, fonts, resources) so the doc can
  // start over for the next per-page file
  void reset();

  // Object store: bodies are kept until finish() so streams can be
  // created incrementally as drawing happens
  std::vector<std::string> objects;  // index + 1 = object number
  int n_objects = 0;

  std::vector<Page> pages;

  std::ostringstream content;          // current page stream
  std::vector<std::ostringstream> stream_stack;  // capture stack

  std::map<std::string, std::unique_ptr<PdfFont>> fonts;
  int font_counter = 0;
  int image_counter = 0;
  int gstate_counter = 0;
  int form_counter = 0;
  int pattern_counter = 0;
  bool page_open = false;
  double page_width_pts = 0, page_height_pts = 0;

  // Global resource registries (name -> object number); every page lists all
  // of them, which keeps resources created inside captured streams usable.
  std::vector<std::pair<std::string, int>> image_objs;
  std::vector<std::pair<std::string, int>> form_objs;
  std::vector<std::pair<std::string, int>> pattern_objs;
  std::vector<std::pair<std::string, int>> shading_objs;
  std::map<std::string, std::string> gstate_names;  // spec key -> name
  std::vector<std::pair<std::string, int>> gstate_objs;

  std::string info_title, info_author, info_creator, info_subject,
      info_keywords, info_creation_date;

  // Gradient helper: stitching function over stops; returns object number
  int addStitchingFunction(const std::vector<double>& stops,
                           const std::vector<unsigned int>& cols, bool gray);
  // Shading dict object; returns object number
  int addShading(bool radial, const double coords[6],
                 const std::vector<double>& stops,
                 const std::vector<unsigned int>& cols, int extend, bool gray);
  std::string addGradientPattern(bool radial, const double coords[6],
                                 const std::vector<double>& stops,
                                 const std::vector<unsigned int>& cols,
                                 int extend, std::string* alpha_gs);
  std::string resourcesString(
      const std::map<std::string, int>& font_objs) const;

  void addStreamObject(int obj, const std::string& dict_extra,
                       const std::string& data, bool compress);
  friend class PdfFont;
};

// Convenience for emitting compact numbers into content streams
std::string pdf_num(double v);
std::string pdf_hex16(uint32_t gid);  // 4-digit hex glyph code
std::string utf8_to_utf16be_hex(const std::string& utf8);
